
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>   /* 编译期配置头：HAVE_MSGHDR_MSG_CONTROL 等平台探测宏在此定义 */
#include <ngx_core.h>   /* 核心头：ngx_int_t/ngx_socket_t 等基础类型与日志 API */
#include <ngx_channel.h>   /* 通道头：ngx_channel_s 命令消息结构(命令/进程pid/fd)与本文件函数的声明 */


/*
 * ngx_write_channel：master 向 worker(或反向)发送一条通道命令消息。
 * 底层是 Unix 域 socket 上的 sendmsg()：消息分两部分——
 * 普通数据(iov)装载 ngx_channel_s 结构体(command/pid/fd)；
 * 附属数据(ancillary data/控制信息)在 ch->fd != -1 时额外携带一个文件描述符，
 * 内核按 SCM_RIGHTS 协议在接收进程中"克隆"出指向同一打开文件表项的新 fd，
 * 这就是 Unix 域 socket 独有的 fd 传递能力（监听 socket 就是靠它移交给 worker 的）。
 */
ngx_int_t ngx_write_channel(ngx_socket_t s, ngx_channel_t *ch, size_t size,   /* s：通道 socket；ch：命令消息；size：消息长度 */
                            ngx_log_t *log)  /* 日志对象 */
{
    ssize_t             n;   /* sendmsg 返回值：已发送字节数，-1 表示出错 */
    ngx_err_t           err;   /* 暂存错误码，防止被后续日志调用覆盖 */
    struct iovec        iov[1];   /* 分散读/聚集写(scatter-gather)向量：此处仅一块，装命令结构体 */
    struct msghdr       msg;   /* sendmsg 的消息头：普通数据(iov)与附属数据(控制信息)的总装容器 */

#if (HAVE_MSGHDR_MSG_CONTROL)   /* 平台支持 msghdr.msg_control（Linux/BSD 现代附属数据接口） */

    union {   /* 联合体：既可按 cmsghdr 结构化访问，又提供 CMSG_SPACE 对齐的字节缓冲 */
        struct cmsghdr  cm;   /* 控制信息头：长度/层级/类型，其后紧跟附加数据(一个 int fd) */
        char            space[CMSG_SPACE(sizeof(int))];   /* 按 CMSG_SPACE 对齐的原始空间，恰好容纳一个待传递的 int */
    } cmsg;   /* 附属数据缓冲 */

    if (ch->fd == -1) {   /* 本条命令不携带 fd（如通知退出、重开日志等纯控制消息） */
        msg.msg_control = NULL;   /* 不挂控制缓冲 */
        msg.msg_controllen = 0;   /* 控制长度 0：本次 sendmsg 无附属数据 */

    } else {   /* 需要传递 fd（典型：把新 worker 的通道端点通告给其他进程） */
        msg.msg_control = (caddr_t) &cmsg;   /* 挂上控制缓冲 */
        msg.msg_controllen = sizeof(cmsg);   /* 声明缓冲总长 */

        cmsg.cm.cmsg_len = sizeof(cmsg);   /* 单条控制信息的总长度（头+数据） */
        cmsg.cm.cmsg_level = SOL_SOCKET;  /* 层级：套接字层 */
        cmsg.cm.cmsg_type = SCM_RIGHTS;   /* 类型：SCM_RIGHTS，告诉内核按"传递访问权限(fd)"协议处理 */
        *(int *) CMSG_DATA(&cmsg.cm) = ch->fd;   /* 把待传递的 fd 写入控制信息的数据区(CMSG_DATA 跳过 cmsg 头) */
    }   /* 携带 fd 与否 if/else 结束 */

#else   /* 老平台走 4.3BSD Reno 风格的 msg_accrights 接口（只能传一个 fd） */

    if (ch->fd == -1) {   /* 不携带 fd */
        msg.msg_accrights = NULL;   /* 无附属数据指针 */
        msg.msg_accrightslen = 0;   /* 长度为 0 */

    } else {   /* 携带 fd */
        msg.msg_accrights = (caddr_t) &ch->fd;   /* 直接把 fd 变量的地址挂为附属数据 */
        msg.msg_accrightslen = sizeof(int);   /* 长度恰为一个 int */
    }   /* 携带 fd 与否 if/else 结束 */

#endif   /* 平台附属数据接口分支结束 */

    iov[0].iov_base = (char *) ch;   /* 向量指向命令结构体本身 */
    iov[0].iov_len = size;   /* 长度取调用者给定的 size（即 sizeof(ngx_channel_t)） */

    msg.msg_name = NULL;   /* 已连接的 Unix 域 socket 不需要目的地址 */
    msg.msg_namelen = 0;   /* 地址长度为 0 */
    msg.msg_iov = iov;   /* 挂上普通数据向量 */
    msg.msg_iovlen = 1;   /* 仅一个向量元素 */

    n = sendmsg(s, &msg, 0);   /* 系统调用：内核把普通数据与附属数据(fd)打包成一条消息发出 */

    if (n == -1) {   /* 发送出错 */
        err = ngx_errno;   /* 先取错误码再分支 */

        if (err == NGX_EAGAIN) {   /* EAGAIN：非阻塞 socket 的发送缓冲已满——命令属短消息，稍后重试即可 */
            return NGX_AGAIN;   /* 返回"稍后再试"而非硬错误 */
        }   /* EAGAIN if 结束 */

        ngx_log_error(NGX_LOG_ALERT, log, err, "sendmsg() failed");   /* 其他错误为严重问题，ALERT 告警 */
        return NGX_ERROR;   /* 返回失败 */
    }   /* 发送出错 if 结束 */

    return NGX_OK;   /* 发送成功 */
}   /* ngx_write_channel 结束 */


/*
 * ngx_read_channel：从通道 socket 接收一条命令消息（worker 端事件循环中调用）。
 * recvmsg() 同时收取普通数据与附属数据：若是 OPEN_CHANNEL 命令，内核会把
 * 附属数据中的 fd 安装为本进程的新描述符。返回 0 表示对端(master)已关闭通道，
 * 上层据此让 worker 自杀，防止成为无人管理的失控进程；无数据时返回 NGX_AGAIN。
 */
ngx_int_t ngx_read_channel(ngx_socket_t s, ngx_channel_t *ch, size_t size,   /* s：通道 socket；ch：消息接收缓冲；size：期望长度 */
                           ngx_log_t *log)   /* 日志对象 */
{   
    ssize_t             n;   /* recvmsg 返回值：读到字节数；0=对端关闭(EOF)；-1=出错 */
    ngx_err_t           err;   /* 暂存错误码 */
    struct iovec        iov[1];   /* 数据向量：装收到的命令结构体 */
    struct msghdr       msg;   /* 消息头：普通数据与附属数据的接收容器 */

#if (HAVE_MSGHDR_MSG_CONTROL)   /* 平台分支：现代附属数据接口 */
    union {   /* 附属数据接收缓冲：可按 cmsghdr 结构访问 */
        struct cmsghdr  cm;   /* 控制信息头 */
        char            space[CMSG_SPACE(sizeof(int))];   /* 容纳对端传来的一个 int fd 的对齐空间 */
    } cmsg;   /* 缓冲定义 */
#else   /* 老平台 accrights 接口 */
    int                 fd;   /* 老接口：接收到的 fd 直接落入这个变量 */
#endif   /* 平台分支结束 */

    iov[0].iov_base = (char *) ch;   /* 消息体写入调用者的 ngx_channel_s 结构 */
    iov[0].iov_len = size;   /* 期望读取的字节数 */

    msg.msg_name = NULL;   /* 无需源地址 */
    msg.msg_namelen = 0;   /* 地址长度 0 */
    msg.msg_iov = iov;   /* 普通数据向量 */
    msg.msg_iovlen = 1;   /* 单个向量 */

#if (HAVE_MSGHDR_MSG_CONTROL)   /* 平台分支：挂控制缓冲 */
    msg.msg_control = (caddr_t) &cmsg;   /* 提供控制缓冲以接收附属数据 */
    msg.msg_controllen = sizeof(cmsg);   /* 缓冲长度 */
#else   /* 老接口分支 */
    msg.msg_accrights = (caddr_t) &fd;   /* 老接口：接收缓冲为 int fd */
    msg.msg_accrightslen = sizeof(int);   /* 长度恰为一个 int */
#endif   /* 平台分支结束 */

    n = recvmsg(s, &msg, 0);   /* 系统调用：接收消息；若带 SCM_RIGHTS，内核同时为本进程安装新 fd */

    if (n == -1) {   /* 接收出错 */
        err = ngx_errno;   /* 取错误码 */

        if (err == NGX_EAGAIN) {   /* EAGAIN：非阻塞 socket 暂无数据——事件驱动下属常态 */
            return NGX_AGAIN;   /* 通知上层下次再读 */
        }   /* EAGAIN if 结束 */

        ngx_log_error(NGX_LOG_ALERT, log, err, "recvmsg() failed");   /* 其他错误 ALERT 告警 */
        return NGX_ERROR;   /* 返回失败 */
    }   /* 接收出错 if 结束 */

    if ((size_t) n < sizeof(ngx_channel_t)) {   /* 读到的字节不足一条完整消息：通道协议被破坏 */
        ngx_log_error(NGX_LOG_ALERT, log, 0,   /* ALERT 告警 */
                      "recvmsg() returned not enough data");   /* 指明是短读 */
        return NGX_ERROR;   /* 返回失败 */
    }   /* 短读 if 结束 */

#if (HAVE_MSGHDR_MSG_CONTROL)   /* 平台分支：解析附属数据 */

    if (ch->command == NGX_CMD_OPEN_CHANNEL) {   /* 只有"通告新进程"命令携带 fd（新 worker 通道端点的通告） */

        if (cmsg.cm.cmsg_len < sizeof(cmsg)) {   /* 附属数据长度不足：至少应装下一个 int */
            ngx_log_error(NGX_LOG_ALERT, log, 0,   /* ALERT 告警 */
                          "recvmsg() returned too small ancillary data");   /* 指明附属数据过短 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* 附属长度 if 结束 */

        if (cmsg.cm.cmsg_level != SOL_SOCKET || cmsg.cm.cmsg_type != SCM_RIGHTS)   /* 校验层级/类型确为套接字层的 SCM_RIGHTS，防止把垃圾数据当 fd */
        {
            ngx_log_error(NGX_LOG_ALERT, log, 0,   /* ALERT 告警 */
                          "recvmsg() returned invalid ancillary data "   /* 消息前半段 */
                          "level %d or type %d",   /* 消息后半段（字符串拼接） */
                          cmsg.cm.cmsg_level, cmsg.cm.cmsg_type);   /* 打印实际收到的 level/type */
            return NGX_ERROR;   /* 返回失败 */
        }   /* 附属校验 if 结束 */

        ch->fd = *(int *) CMSG_DATA(&cmsg.cm);   /* 从控制信息数据区取出内核安装好的 fd，填回消息结构供上层使用 */
    }   /* OPEN_CHANNEL if 结束 */

    if (msg.msg_flags & (MSG_TRUNC|MSG_CTRUNC)) {   /* 内核置位说明普通数据或控制数据被截断（接收缓冲不够大） */
        ngx_log_error(NGX_LOG_ALERT, log, 0,   /* ALERT 告警 */
                      "recvmsg() truncated data");   /* 指明发生了截断 */
    }   /* 截断 if 结束 */

#else   /* 老接口分支 */

    if (ch->command == NGX_CMD_OPEN_CHANNEL) {   /* 同样只有 OPEN_CHANNEL 命令携带 fd */
        if (msg.msg_accrightslen != sizeof(int)) {   /* 老接口要求附属长度恰为一个 int，否则视为没收到 fd */
            ngx_log_error(NGX_LOG_ALERT, log, 0,   /* ALERT 告警 */
                          "recvmsg() returned no ancillary data");   /* 指明未收到附属数据 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* 附属长度 if 结束 */

        ch->fd = fd;   /* 取出接收到的 fd 填回消息结构 */
    }   /* OPEN_CHANNEL if 结束 */

#endif   /* 平台分支结束 */

    return n;   /* 返回实际字节数：上层见到 0(EOF) 即 master 已关闭通道，worker 将自杀退出 */
}   /* ngx_read_channel 结束 */


/*
 * ngx_add_channel_event：把通道 fd 注册进事件驱动框架(epoll/select 等)统一监听。
 * 与客户端连接一样构造 ngx_connection_t 与读/写事件对象并挂上就绪回调，
 * 体现 nginx"一切皆事件"的设计：进程间通道与网络连接共用同一套事件机制。
 */
ngx_int_t ngx_add_channel_event(ngx_cycle_t *cycle, ngx_fd_t fd,   /* cycle：周期对象；fd：通道描述符 */
                                ngx_int_t event, ngx_event_handler_pt handler)   /* 关心的事件类型(读/写)与就绪时的处理函数 */
{
    ngx_event_t       *ev, *rev, *wev;   /* ev 按参数二选一；rev/wev 为该 fd 的读/写事件对象 */
    ngx_connection_t  *c;   /* 通道对应的连接对象 */

    c = &cycle->connections[fd];   /* 按 fd 下标从周期连接表中取槽位 */
    rev = &cycle->read_events[fd];   /* 对应的读事件槽位 */
    wev = &cycle->write_events[fd];   /* 对应的写事件槽位 */

    ngx_memzero(c, sizeof(ngx_connection_t));   /* 清零连接对象，从头初始化 */
    ngx_memzero(rev, sizeof(ngx_event_t));   /* 清零读事件对象 */
    ngx_memzero(wev, sizeof(ngx_event_t));   /* 清零写事件对象 */

    c->fd = fd;   /* 连接对象绑定通道 fd */
    c->pool = cycle->pool;   /* 挂上周期内存池供后续分配 */

    c->read = rev;   /* 连接对象指回读事件 */
    c->write = wev;   /* 连接对象指回写事件 */

    c->log = cycle->log;   /* 连接级日志对象 */
    rev->log = cycle->log;   /* 读事件日志对象 */
    wev->log = cycle->log;   /* 写事件日志对象 */
    rev->index = NGX_INVALID_INDEX;   /* 事件表索引初值：尚不在 epoll 事件集中 */
    wev->index = NGX_INVALID_INDEX;   /* 同上 */
    rev->data = c;   /* 读事件回指连接对象，handler 借此取上下文 */
    wev->data = c;   /* 写事件同样回指连接对象 */

#if (NGX_THREADS)   /* 线程支持编译分支 */
    rev->lock = &c->lock;   /* 线程模式：读事件与连接共享实例锁 */
    wev->lock = &c->lock;   /* 写事件共享同一把锁 */
    rev->own_lock = &c->lock;   /* 读事件的自有锁指针 */
    wev->own_lock = &c->lock;   /* 写事件的自有锁指针 */
#endif   /* 线程支持分支结束 */

    ev = (event == NGX_READ_EVENT) ? rev : wev;   /* 按调用者关心的事件类型选出要挂回调的事件对象 */

    ev->event_handler = handler;   /* 绑定就绪回调（worker 端即 ngx_channel_handler） */

    if (ngx_add_conn && (ngx_event_flags & NGX_USE_EPOLL_EVENT) == 0) {   /* 事件模块提供 add_conn 且非 epoll（select/poll 等）：按"整连接"同时注册读写 */
        if (ngx_add_conn(c) == NGX_ERROR) {   /* 连接级注册失败 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* add_conn 失败 if 结束 */
    
    } else {  /* 单边事件注册分支 */
        if (ngx_add_event(ev, event, 0) == NGX_ERROR) {   /* epoll 等支持单边注册的事件模块：仅注册关心方向的事件 */
            return NGX_ERROR;   /* 注册失败 */
        }   /* add_event 失败 if 结束 */
    }   /* 注册方式 if/else 结束 */

    return NGX_OK;   /* 通道事件注册成功，此后通道消息由事件循环分发 */
}   /* ngx_add_channel_event 结束 */


/*
 * ngx_close_channel：关闭 socketpair 的两端，彻底拆除该进程的通道。
 * spawn 失败回滚或 master 清理已退出 worker 的槽位时调用；
 * 对端读取时会得到 EOF(返回 0)，从而感知通道已关闭。
 */
void ngx_close_channel(ngx_fd_t *fd, ngx_log_t *log)   /* fd：channel[2] 两端描述符；log：日志对象 */
{
    if (close(fd[0]) == -1) {   /* 关闭通道第一端 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno, "close() failed");   /* 失败则 ALERT 告警 */
    }   /* 关闭第一端 if 结束 */

    if (close(fd[1]) == -1) {   /* 关闭通道第二端 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno, "close() failed");   /* 失败则 ALERT 告警 */
    }   /* 关闭第二端 if 结束 */
}   /* ngx_close_channel 结束 */
