
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>          /* 引入编译配置头：平台宏与 configure 探测结果 */
#include <ngx_core.h>            /* 引入核心头：连接、内存池等基础结构 */
#include <ngx_event.h>           /* 引入事件模块头：事件对象与事件机制声明 */
#include <ngx_event_connect.h>   /* 引入上游连接(peer connection)相关声明 */
#include <nginx.h>               /* 引入 nginx 版本号定义 */


/* AF_INET only */

/*
 * ngx_event_connect_peer：向上游服务器(后端)发起 TCP 连接，核心是非阻塞 connect()。
 * 内核视角：主动打开方的 TCP 状态机为 CLOSED -> SYN_SENT -> ESTABLISHED。在非阻塞
 * socket 上，connect() 让内核发出 SYN 后立即返回 -1 且 errno==EINPROGRESS(握手
 * 进行中)，进程不被睡眠阻塞；此后 SYN 由内核按 1s/2s/4s... 指数退避重传，直到
 * 握手完成或放弃(超时报 ETIMEDOUT)。若对端端口无进程监听，对端协议栈回 RST，
 * 本端结果为 ECONNREFUSED。握手最终结果随后通过"可写事件就绪 +
 * getsockopt(SO_ERROR)"确认。这使单个 worker 进程能同时发起大量上游连接而
 * 无需一连接一线程。返回值：NGX_OK=已连接；NGX_AGAIN=握手进行中等可写事件；
 * NGX_ERROR/NGX_CONNECT_ERROR=失败。
 */
int ngx_event_connect_peer(ngx_peer_connection_t *pc)
{   /* 函数体开始 */
    int                  rc;     /* connect() 的返回值 */
    ngx_uint_t           instance;   /* 旧事件代际戳，用于取反生成新戳防陈旧事件 */
    u_int                event;  /* 事件添加方式标志：水平触发或边沿(清)触发 */
    time_t               now;    /* 当前时间戳(秒)，用于失败熔断超时判断 */
    ngx_err_t            err;    /* connect() 失败时的 errno */
    ngx_peer_t          *peer;   /* 选中的上游服务器(对端) */
    ngx_socket_t         s;      /* 新建 socket 的 fd */
    ngx_event_t         *rev, *wev;   /* 新连接的读事件与写事件 */
    ngx_connection_t    *c;      /* 上游连接对象 */
    ngx_event_conf_t    *ecf;    /* 事件核心模块配置(connections 上限) */
    struct sockaddr_in   addr;   /* IPv4 目的地址结构(网络字节序) */

    now = ngx_time();   /* 取 nginx 缓存的当前时间(秒级)，避免陷入 gettimeofday 系统调用 */

    /* ngx_lock_mutex(pc->peers->mutex); */

    if (pc->peers->last_cached) {   /* 连接缓存栈非空：存在可复用的空闲上游连接 */

        /* cached connection */

        c = pc->peers->cached[pc->peers->last_cached];   /* 从缓存栈顶弹出空闲连接复用，省去整个 TCP 三次握手开销 */
        pc->peers->last_cached--;                        /* 栈顶指针下移 */

        /* ngx_unlock_mutex(pc->peers->mutex); */

#if (NGX_THREADS)                          /* 多线程支持 */
        c->read->lock = c->read->own_lock;    /* 把读事件锁改回连接自身私有锁 */
        c->write->lock = c->write->own_lock;  /* 写事件锁同理 */
#endif   /* 多线程锁恢复结束 */

        pc->connection = c;   /* 复用连接挂到 peer 连接对象上 */
        pc->cached = 1;       /* 标记为缓存连接，调用方无需再发起 connect */
        return NGX_OK;        /* 直接成功返回 */
    }   /* 缓存复用分支结束 */

    pc->cached = 0;          /* 走新建连接路径 */
    pc->connection = NULL;   /* 尚无连接对象 */

    if (pc->peers->number == 1) {   /* 上游只有一台服务器 */
        peer = &pc->peers->peers[0];   /* 直接取唯一对端 */

    } else {   /* 多台上游：需要负载均衡选择 */

        /* there are several peers */

        if (pc->tries == pc->peers->number) {   /* 剩余尝试次数等于服务器数：说明是第一轮选择 */

            /* it's a first try - get a current peer */

            pc->cur_peer = pc->peers->current++;   /* 轮询(round-robin)算法：取全局游标所指对端并把游标后移 */

            if (pc->peers->current >= pc->peers->number) {   /* 游标越过末尾 */
                pc->peers->current = 0;   /* 回绕到首台，实现循环轮转均摊负载 */
            }   /* 游标回绕分支结束 */
        }   /* 第一轮选择分支结束 */

        if (pc->peers->max_fails == 0) {   /* 未启用失败容忍(max_fails=0) */
            peer = &pc->peers->peers[pc->cur_peer];   /* 直接使用轮询选中的对端 */

        } else {   /* 启用了失败熔断 */

            /* the peers support a fault tolerance */

            for ( ;; ) {   /* 轮转寻找一个可用对端 */
                peer = &pc->peers->peers[pc->cur_peer];   /* 取当前候选对端 */

                if (peer->fails <= pc->peers->max_fails   /* 累计失败次数未超熔断阈值，或 */
                    || (now - peer->accessed > pc->peers->fail_timeout))   /* 距上次失败已超过 fail_timeout，失败记录过期，重新给机会 */
                {   /* 该对端可用 */
                    break;   /* 选中此对端 */
                }   /* 可用对端判断结束 */

                pc->cur_peer++;   /* 该对端处于熔断期：跳到下一台 */

                if (pc->cur_peer >= pc->peers->number) {   /* 越过末尾 */
                    pc->cur_peer = 0;   /* 回绕 */
                }   /* 回绕分支结束 */

                pc->tries--;   /* 剩余尝试次数 -1 */

                if (pc->tries == 0) {   /* 所有对端都已试完且不可用 */
                    /* ngx_unlock_mutex(pc->peers->mutex); */

                    return NGX_ERROR;   /* 全部被熔断，本次连接失败 */
                }   /* 熔断试尽分支结束 */
            }   /* 寻找循环结束 */
        }   /* 失败容忍分支结束 */
    }   /* 多对端选择结束 */

    /* ngx_unlock_mutex(pc->peers->mutex); */


    /* 创建 TCP socket(IPv4 + SOCK_STREAM 字节流)：内核生成 TCP 控制块并分配 fd，本地状态为 CLOSED */
    s = ngx_socket(AF_INET, SOCK_STREAM, IPPROTO_IP, 0);

    if (s == -1) {   /* socket 创建失败(如进程 fd 耗尽 EMFILE) */
        /* 记 ALERT 级日志 */
        ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                      ngx_socket_n " failed");   /* 参数：函数名与失败文案 */
        return NGX_ERROR;   /* socket 创建失败返回 */
    }   /* 创建失败分支结束 */


    ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);   /* 读取事件核心模块配置(connections 上限) */

    /* disable warning: Win32 SOCKET is u_int while UNIX socket is int */

    if ((ngx_uint_t) s >= ecf->connections) {   /* fd 编号超出配置的连接数组容量，为防数组越界必须丢弃 */

        /* 告警日志：socket 编号超出 connections 配额 */
        ngx_log_error(NGX_LOG_ALERT, pc->log, 0,
                      "socket() returned socket #%d while only %d "   /* 格式串(分段)：socket 编号超配额 */
                      "connections was configured, closing the socket",   /* 日志文本续：将关闭 socket */
                      s, ecf->connections);   /* 参数：fd 号与配额 */

        if (ngx_close_socket(s) == -1) {   /* 关闭超限 socket，内核立即释放其资源 */
            /* 关闭失败记日志 */
            ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                          ngx_close_socket_n "failed");   /* 参数：函数名与失败文案 */
        }   /* 关闭失败分支结束 */

        /* TODO: sleep for some time */

        return NGX_ERROR;   /* 连接失败返回 */
    }   /* 超配额分支结束 */


    if (pc->rcvbuf) {   /* 调用方指定了接收缓冲区大小 */
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF,   /* 设置内核 socket 接收缓冲区(SO_RCVBUF)：决定 TCP 接收窗口上限，影响吞吐 */
                       (const void *) &pc->rcvbuf, sizeof(int)) == -1) {   /* 条件续行：与 -1 比较判断是否设置成功 */
            /* 设置失败记日志 */
            ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                          "setsockopt(SO_RCVBUF) failed");   /* 参数：失败文案 */

            if (ngx_close_socket(s) == -1) {   /* 关闭 socket 收尾 */
                /* 关闭失败记日志 */
                ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                              ngx_close_socket_n " failed");   /* 参数：函数名与失败文案 */
            }   /* 关闭失败分支结束 */

            return NGX_ERROR;   /* 缓冲区设置失败返回 */
        }   /* setsockopt 失败分支结束 */
    }   /* 指定 rcvbuf 分支结束 */

    if (ngx_nonblocking(s) == -1) {   /* 置 O_NONBLOCK：这是非阻塞 connect 的前提，此后进程绝不睡眠 */
        /* 设置失败记日志 */
        ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                      ngx_nonblocking_n " failed");   /* 参数：函数名与失败文案 */

        if (ngx_close_socket(s) == -1) {   /* 关闭 socket 失败则记日志 */
            /* 关闭失败记日志 */
            ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                          ngx_close_socket_n " failed");   /* 参数：函数名与失败文案 */
        }   /* 关闭失败分支结束 */

        return NGX_ERROR;   /* 置非阻塞失败返回 */
    }   /* 非阻塞设置分支结束 */

#if (WIN32)   /* Windows 平台分支 */
    /*
     * Winsock assignes a socket number divisible by 4
     * so to find a connection we divide a socket number by 4.
     */

    if (s % 4) {   /* Winsock 句柄恒为 4 的倍数，不满足则为致命异常 */
        /* 日志：句柄不能被 4 整除 */
        ngx_log_error(NGX_LOG_EMERG, pc->log, 0,
                      ngx_socket_n   /* 参数：函数名(字符串拼接) */
                      " created socket %d, not divisible by 4", s);   /* 参数：句柄值与失败文案 */
        exit(1);   /* 无法定位连接槽位，退出进程 */
    }   /* 句柄校验分支结束 */

    c = &ngx_cycle->connections[s / 4];    /* 句柄除以 4 定位连接数组槽位 */
    rev = &ngx_cycle->read_events[s / 4];  /* 同槽位的读事件 */
    wev = &ngx_cycle->write_events[s / 4]; /* 同槽位的写事件 */

#else   /* UNIX 分支 */

    c = &ngx_cycle->connections[s];    /* UNIX 下 fd 即下标：连接/读事件/写事件三张全局表按 fd 严格对齐 */
    rev = &ngx_cycle->read_events[s];   /* 同一 fd 下标的读事件槽位 */
    wev = &ngx_cycle->write_events[s];   /* 同一 fd 下标的写事件槽位 */

#endif   /* 平台分支结束 */

    instance = rev->instance;   /* 保存槽位上的旧代际戳，随后取反生成新戳以作废陈旧事件 */

#if (NGX_THREADS)   /* 多线程支持 */

    if (*(&c->lock)) {   /* 该连接槽位正被其他线程占用(锁字非 0) */
        /* 调试日志：connect 中发生自旋等待 */
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, pc->log, 0,
                       "spinlock in connect, fd:%d", s);   /* 参数：fd 号 */
        ngx_spinlock(&c->lock, 1000);   /* 自旋等待：忙等而非睡眠，适合极短临界区 */
        ngx_unlock(&c->lock);   /* 获得后立即释放 */
    }   /* 自旋等待分支结束 */

#endif   /* 多线程支持结束 */

    ngx_memzero(c, sizeof(ngx_connection_t));    /* 清零复用的连接槽位(残留上一连接旧状态) */
    ngx_memzero(rev, sizeof(ngx_event_t));       /* 清零读事件槽位 */
    ngx_memzero(wev, sizeof(ngx_event_t));       /* 清零写事件槽位 */

    rev->instance = !instance;   /* 读事件代际戳取反：事件机制据此丢弃上一连接遗留的过期事件 */
    wev->instance = !instance;   /* 写事件同理 */

    rev->index = NGX_INVALID_INDEX;   /* 事件在驱动表(select/poll 数组等)中的索引置无效(尚未注册) */
    wev->index = NGX_INVALID_INDEX;   /* 写事件索引同样置无效 */

    rev->data = c;   /* 事件回指所属连接：触发时由 data 取回上下文 */
    wev->data = c;   /* 写事件回指连接 */

    c->read = rev;     /* 连接与读写事件形成双向关联 */
    c->write = wev;   /* 连接持有写事件指针 */
    wev->write = 1;    /* 标记为写事件类型 */

    c->log = pc->log;     /* 连接及其事件统一使用上游模块传入的日志对象 */
    rev->log = pc->log;   /* 读事件绑定日志对象 */
    wev->log = pc->log;   /* 写事件绑定日志对象 */

    c->fd = s;   /* 连接绑定新 fd */

    c->log_error = pc->log_error;   /* 错误日志级别策略(如仅记录对端错误) */

    pc->connection = c;   /* peer 连接对象与连接互相绑定 */

    /*
     * TODO: MT: - atomic increment (x86: lock xadd)
     *             or protection by critical section or mutex
     *
     * TODO: MP: - allocated in a shared memory
     *           - atomic increment (x86: lock xadd)
     *             or protection by critical section or mutex
     */

    c->number = ngx_atomic_inc(ngx_connection_counter);   /* 原子自增全局连接计数器取唯一编号(x86 上为 lock xadd 前缀指令) */

#if (NGX_THREADS)   /* 多线程锁字段初始化 */
    rev->lock = pc->lock;        /* 读写事件锁指向上游模块的共享锁 */
    wev->lock = pc->lock;   /* 写事件锁指向上游共享锁 */
    rev->own_lock = &c->lock;    /* 属主锁指向连接自身锁 */
    wev->own_lock = &c->lock;   /* 写事件属主锁 */
#endif   /* 多线程锁初始化结束 */

    if (ngx_add_conn) {   /* 事件机制提供按连接注册的接口(select/poll/devpoll/rtsig 等) */
        if (ngx_add_conn(c) == NGX_ERROR) {   /* 同时注册读写事件监听，连接状态变化将由事件机制通知 */
            return NGX_ERROR;   /* 注册失败上报 */
        }   /* add_conn 失败分支结束 */
    } /* add_conn 注册分支结束 */

    ngx_memzero(&addr, sizeof(struct sockaddr_in));   /* 清零目的地址结构(sin_zero 必须为 0，这是 POSIX 要求) */

    addr.sin_family = AF_INET;          /* IPv4 协议族 */
    addr.sin_port = peer->port;         /* 目的端口(网络字节序 big-endian) */
    addr.sin_addr.s_addr = peer->addr;  /* 目的 IP(网络字节序 32 位地址) */

    /* 调试日志：输出目的地址与连接编号 */
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, pc->log, 0,
                   "connect to %s, #%d", peer->addr_port_text.data, c->number);   /* 参数：目的地址文本与连接编号 */

    /* 关键调用：非阻塞 connect——内核发出 SYN(本端进入 SYN_SENT)后立即返回，
     * 不等待三次握手完成；仅当目的为本机等特殊情况才可能同步成功返回 0 */
    rc = connect(s, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));

    if (rc == -1) {   /* 返回 -1：非阻塞下通常是 EINPROGRESS，也可能是真实失败 */
        err = ngx_socket_errno;   /* 取出 errno 分类处理 */

        /* Winsock returns WSAEWOULDBLOCK (NGX_EAGAIN) */

        if (err != NGX_EINPROGRESS && err != NGX_EAGAIN) {   /* 非"进行中"错误即真实失败：如 ECONNREFUSED(对端 RST，端口无监听)、ENETUNREACH(路由不可达) */
            ngx_connection_error(c, err, "connect() failed");   /* 记录连接级错误日志 */

            if (ngx_close_socket(s) == -1) {   /* 关闭失败的 socket */
                /* 关闭失败记日志 */
                ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                              ngx_close_socket_n " failed");   /* 参数：函数名与失败文案 */
            }   /* 关闭失败分支结束 */

            c->fd = (ngx_socket_t) -1;   /* 连接对象解绑 fd，槽位可复用 */

            return NGX_CONNECT_ERROR;   /* 上报连接错误，调用方会重试下一台上游 */
        }   /* 真实失败分支结束 */
    }   /* rc==-1 分支结束 */

    if (ngx_add_conn) {   /* 已用 add_conn 注册过事件监听的机制 */
        if (rc == -1) {   /* connect 尚在进行中 */
            /* NGX_EINPROGRESS */
            return NGX_AGAIN;   /* 告知调用方：握手进行中，待可写事件再确认结果 */
        }   /* add_conn 分支结束 */
 
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, pc->log, 0, "connected");   /* 同步连接成功(罕见：目的为本机时握手在 connect 返回前已完成) */
        return NGX_OK;   /* 立即成功返回 */
    }   /* add_conn 分支结束 */

    if (ngx_event_flags & NGX_USE_AIO_EVENT) {   /* aio/iocp 异步事件机制分支 */

        /* aio, iocp */

        if (ngx_blocking(s) == -1) {   /* AIO 机制要求 socket 为阻塞模式，改回阻塞 */
            /* 设置失败记日志 */
            ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                          ngx_blocking_n " failed");   /* 参数：函数名与失败文案 */

            if (ngx_close_socket(s) == -1) {   /* 关闭 socket 失败则记日志 */
                /* 关闭失败记日志 */
                ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                              ngx_close_socket_n " failed");   /* 参数：函数名与失败文案 */
            }   /* 关闭失败分支结束 */

            return NGX_ERROR;   /* 置阻塞失败返回 */
        }   /* 置阻塞失败分支结束 */

        /*
         * aio allows to post operation on non-connected socket
         * at least in FreeBSD.
         * NT does not support it.
         * 
         * TODO: check in Win32, etc. As workaround we can use NGX_ONESHOT_EVENT
         */
 
        rev->ready = 1;   /* AIO 下直接预置读就绪标志 */
        wev->ready = 1;   /* 写方向预置就绪 */

        return NGX_OK;   /* AIO 路径直接返回 */
    }   /* AIO 分支结束 */

    /* kqueue 为边沿/清零触发(状态变化只通知一次)，select/poll 等为水平触发，注册时需区分 */
    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {     /* kqueue */
        event = NGX_CLEAR_EVENT;   /* kqueue 使用 EV_CLEAR：事件需一次性消费完，否则不再通知 */

    } else {                                  /* select、poll、/dev/poll 等水平触发机制 */
        event = NGX_LEVEL_EVENT;   /* 水平触发：只要条件满足，每次轮询都会报告 */
    }   /* 触发方式选择结束 */

    if (ngx_add_event(rev, NGX_READ_EVENT, event) != NGX_OK) {   /* 注册读事件：还可感知握手期间的异常(对端 RST 后 socket 可读且读出 ECONNRESET) */
        return NGX_ERROR;   /* 读事件注册失败返回 */
    }   /* 读事件注册分支结束 */

    if (rc == -1) {   /* connect 仍在进行中(EINPROGRESS) */

        /* NGX_EINPROGRESS */

        if (ngx_add_event(wev, NGX_WRITE_EVENT, event) != NGX_OK) {   /* 注册写事件：三次握手完成进入 ESTABLISHED 时发送缓冲可用、socket 变为可写，以此感知连接建立 */
            return NGX_ERROR;   /* 写事件注册失败返回 */
        }   /* 写事件注册分支结束 */

        return NGX_AGAIN;   /* 返回后调用方挂起请求，等事件循环通知连接结果 */
    }   /* EINPROGRESS 分支结束 */

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, pc->log, 0, "connected");   /* 调试日志：connect 同步成功 */

    wev->ready = 1;   /* 写方向直接标记就绪：连接已建立，发送缓冲区为空 */

    return NGX_OK;   /* 连接成功返回 */
}   /* ngx_event_connect_peer 函数结束 */


/*
 * ngx_event_connect_peer_failed：上游连接失败后的善后——给当前对端记一笔失败，
 * 轮转到下一台服务器并扣减剩余尝试次数。它与 max_fails/fail_timeout 配合实现
 * 简单熔断：失败次数超过 max_fails 的节点在 fail_timeout 秒内被跳过。
 */
void ngx_event_connect_peer_failed(ngx_peer_connection_t *pc)
{   /* 函数体开始 */
    time_t  now;   /* 当前时间戳(秒) */

    now = ngx_time();   /* 取缓存时间 */

    /* ngx_lock_mutex(pc->peers->mutex); */

    pc->peers->peers[pc->cur_peer].fails++;          /* 当前对端失败计数 +1(供 max_fails 熔断判断) */
    pc->peers->peers[pc->cur_peer].accessed = now;   /* 记录失败时刻：fail_timeout 之后该对端重新可用 */

    /* ngx_unlock_mutex(pc->peers->mutex); */

    pc->cur_peer++;      /* 轮转到下一个对端 */

    if (pc->cur_peer >= pc->peers->number) {   /* 越过末尾 */
        pc->cur_peer = 0;   /* 回绕到首台 */
    }   /* 回绕分支结束 */

    pc->tries--;   /* 剩余尝试次数 -1 */

    return;   /* 返回 */
}   /* ngx_event_connect_peer_failed 函数结束 */
