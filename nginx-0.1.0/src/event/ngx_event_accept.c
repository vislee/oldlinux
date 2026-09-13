
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>                    /* 引入编译配置头：平台相关宏与 configure 探测结果(是否有 epoll/kqueue 等) */
#include <ngx_core.h>                      /* 引入核心头：内存池、连接、字符串等基础数据结构与接口 */
#include <ngx_event.h>                     /* 引入事件模块头：事件对象、监听结构、accept 互斥锁等声明 */
#include <nginx.h>                         /* 引入 nginx 版本号定义 */


typedef struct {                           /* 定义 accept 阶段专用的日志上下文结构 */
    int      flag;                         /* 标志位：置 -1 表示 accept 阶段禁用连接编号日志(此时尚无连接号) */
    u_char  *name;                         /* 指向监听地址文本("IP:PORT")，出错时写入日志便于定位端口 */
} ngx_accept_log_ctx_t;                    /* 结构体类型名 */


/* 前置声明：关闭刚 accept 的 socket 的静态函数 */
static void ngx_close_accepted_socket(ngx_socket_t s, ngx_log_t *log);
/* 前置声明：accept 失败时生成日志后缀的回调函数 */
static size_t ngx_accept_log_error(void *data, char *buf, size_t len);


/*
 * ngx_event_accept：监听 socket 读事件的处理函数，本质是 accept() 的完整封装。
 * 内核视角：TCP 三次握手完全由协议栈自动完成——收到客户端 SYN 后挂入半连接队列
 * (SYN 队列，受 tcp_max_syn_backlog 限制)并回 SYN+ACK，本端连接处于 SYN_RCVD 状态；
 * 收到客户端最后一个 ACK 后连接转为 ESTABLISHED，移入全连接队列(accept 队列)，
 * 队列长度为 min(listen() 的 backlog, net.core.somaxconn)。因此"监听 fd 可读"
 * 就等价于"全连接队列非空"，这正是本函数被事件机制调用的前提。accept() 系统调用
 * 本身极轻：只从队头摘下一个连接，由内核分配新 fd(与客户端构成唯一五元组)并返回。
 * 本函数随后为新 fd 建立连接对象与读写事件、注册进事件机制、设置 TCP_NODELAY
 * (禁用 Nagle 算法，保证 HTTP 小包低延迟)，再回调 listening->handler 交给
 * 协议层接管；同时处理 EAGAIN(队列已被取空)、ECONNABORTED(握手完成前被 RST)、
 * fd 数超配额等边界情况。
 */
void ngx_event_accept(ngx_event_t *ev)
{   /* 函数体开始 */
    ngx_uint_t             instance, accepted;   /* instance:事件代际戳(用于丢弃陈旧事件)；accepted:本次共 accept 的连接数 */
    socklen_t              len;                  /* 对端地址长度，accept() 的值-结果参数 */
    struct sockaddr       *sa;                   /* 指向存放对端地址的缓冲区(位于连接池内) */
    ngx_err_t              err;                  /* 保存 accept() 失败时的 errno */
    ngx_log_t             *log;                  /* 新连接专用的日志对象 */
    ngx_pool_t            *pool;                 /* 新连接的内存池 */
    ngx_socket_t           s;                    /* accept() 返回的新连接 fd(内核分配的文件描述符) */
    ngx_event_t           *rev, *wev;            /* 新连接的读事件与写事件对象 */
    ngx_connection_t      *c, *ls;               /* c:新建连接对象；ls:监听连接对象(由事件 data 携带) */
    ngx_event_conf_t      *ecf;                  /* 事件核心模块配置(最大连接数、multi_accept 等) */
    ngx_accept_log_ctx_t  *ctx;                  /* accept 日志上下文 */

    ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);   /* 读取事件核心模块的配置 */

    if (ngx_event_flags & NGX_USE_RTSIG_EVENT) {     /* realtime 信号机制：信号只报告"有事件"，不报告数量 */
        ev->available = 1;                           /* 故本轮最多只取 1 个连接 */

    } else if (!(ngx_event_flags & NGX_HAVE_KQUEUE_EVENT)) {   /* 非 kqueue(select/poll/epoll 等)机制 */
        ev->available = ecf->multi_accept;           /* available 由 multi_accept 配置决定一轮最多连续 accept 几个 */
    }                                                /* kqueue 则不同：内核事件会精确填充待 accept 的连接数 */

    ls = ev->data;                                   /* 事件 data 指向监听连接，取回监听 socket 上下文 */

    /* 调试日志：输出监听地址与本轮预计可取的连接数 */
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "accept on %s, ready: %d",   /* 日志格式串：监听地址与待取连接数 */
                   ls->listening->addr_text.data, ev->available);   /* 参数：监听地址文本与 available 计数 */

    ev->ready = 0;      /* 清除监听事件就绪标志：本次触发即将被消费 */
    accepted = 0;       /* 连接计数清零 */
    pool = NULL;        /* 内存池指针置空，迫使循环首轮创建新池 */

    do {                /* 循环 accept：直到全连接队列取空或达到本轮上限 */

        if (pool == NULL) {                          /* 上一轮的 pool 已随连接移交，需要分配新池 */

            /*
             * Create the pool before accept() to avoid the copying of
             * the sockaddr.  Although accept() can fail it is uncommon
             * case and besides the pool can be got from the free pool list
             */

            if (!(pool = ngx_create_pool(ls->listening->pool_size, ev->log))) {   /* accept 前先建内存池(预分配 sockaddr 空间，避免事后拷贝) */
                return;                              /* 内存不足：放弃本轮，等下次可读事件再试 */
            }   /* 建池失败分支结束 */
        }   /* pool==NULL 判断结束 */

        if (!(sa = ngx_palloc(pool, ls->listening->socklen))) {   /* 从池中分配对端地址缓冲区(AF_INET 为 sizeof(struct sockaddr_in)) */
            ngx_destroy_pool(pool);                  /* 分配失败：销毁内存池，池内所有内存一次释放 */
            return;   /* 内存不足：放弃本次 accept */
        }   /* sa 分配失败分支结束 */

        if (!(log = ngx_palloc(pool, sizeof(ngx_log_t)))) {   /* 从池中分配本连接私有的日志对象 */
            ngx_destroy_pool(pool);   /* 销毁内存池，池内内存一次释放 */
            return;   /* 放弃本次处理 */
        }   /* 日志对象分配失败分支结束 */

        ngx_memcpy(log, ls->log, sizeof(ngx_log_t));   /* 复制监听 socket 的日志属性(级别等)：连接日志继承自监听 */
        pool->log = log;                               /* 日志对象挂到内存池：池内后续分配失败也能正确记日志 */

        if (!(ctx = ngx_palloc(pool, sizeof(ngx_accept_log_ctx_t)))) {   /* 分配 accept 日志上下文 */
            ngx_destroy_pool(pool);   /* 销毁内存池回收资源 */
            return;   /* 放弃本次处理 */
        }   /* 上下文分配失败分支结束 */

        /* -1 disables the connection number logging */
        ctx->flag = -1;                               /* 置 -1：accept 阶段还没有连接编号，日志中禁用连接号 */
        ctx->name = ls->listening->addr_text.data;    /* 记录监听地址文本，供日志回调输出 "on IP:PORT" */

        log->data = ctx;                     /* 上下文挂入日志对象 */
        log->handler = ngx_accept_log_error; /* 注册日志后缀回调：出错时追加 " while accept() on IP:PORT" */

        len = ls->listening->socklen;        /* 每轮 accept 前重置地址长度：len 是值-结果参数，内核会写入实际地址长度 */

        /* 核心：从内核全连接队列(accept 队列)队头取出一个 ESTABLISHED 连接，内核创建新 fd 并填入对端地址 */
        s = accept(ls->fd, sa, &len);
        if (s == -1) {                       /* -1 表示失败：队列空或出错 */
            err = ngx_socket_errno;          /* 读取 errno 以区分具体错误 */

            if (err == NGX_EAGAIN) {                        /* EAGAIN：非阻塞 accept 且全连接队列已被取空(可能被其他事件循环/进程先取走) */
#if 0                                                    /* 以下为被预处理禁用的调试代码 */
                if (!(ngx_event_flags & NGX_USE_RTSIG_EVENT))   /* (禁用代码)仅非 rtsig 机制才记录 EAGAIN 日志 */
                {                                        /* (禁用代码)分支体开始 */
                    /* (禁用代码)记录队列被取空前已完成的 accept 次数 */
                    ngx_log_error(NGX_LOG_NOTICE, log, err,
                                  "EAGAIN after %d accepted connection(s)",   /* (禁用代码)格式串：EAGAIN 前已取的连接数 */
                                  accepted);   /* (禁用代码)参数：accepted 计数 */
                }                                        /* (禁用代码)分支体结束 */
#endif                                                   /* 禁用块结束 */

                ngx_destroy_pool(pool);   /* 队列已空：销毁本轮预分配的内存池 */
                return;                   /* 返回事件循环，等待内核下一次"入队完成"的可读通知 */
            }   /* EAGAIN(队列取空)分支结束 */

            /* 其余 accept 错误：记 ALERT 级日志 */
            ngx_log_error(NGX_LOG_ALERT, ev->log, err,
                          "accept() on %s failed",   /* 格式串：accept 失败 */
                          ls->listening->addr_text.data);   /* 参数：监听地址文本 */

            if (err == NGX_ECONNABORTED) {          /* ECONNABORTED：连接在握手完成前已被客户端 RST(Linux 特有行为) */
                if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {   /* kqueue 事件机制 */
                    ev->available--;                /* 该连接已被计入可读数，扣减以修正剩余计数 */
                }   /* kqueue 计数修正分支结束 */

                if (ev->available) {                /* 队列里可能还有其他正常连接 */
                    /* reuse the previously allocated pool */
                    continue;                       /* 复用已分配的内存池，继续 accept 下一个连接 */
                }   /* 仍有剩余连接分支结束 */
            }   /* ECONNABORTED 分支结束 */

            ngx_destroy_pool(pool);   /* 其余错误不可恢复：销毁内存池 */
            return;                   /* 结束本次事件处理 */
        }   /* accept 失败处理结束 */

#if (NGX_STAT_STUB)                     /* 状态统计桩编译开关 */
        (*ngx_stat_accepted)++;         /* 全局"已接受连接总数" +1(供 stub_status 页面展示) */
#endif   /* 状态统计桩块结束 */

        /* 计算本进程的 accept 压力值：fd 数越接近上限值越大；大于 0 时事件循环会主动让出 accept 锁，
         * 把接受新连接的机会让给其他 worker，避免单进程 fd 耗尽 */
        ngx_accept_disabled = (ngx_uint_t) s + NGX_ACCEPT_THRESHOLD
                                                            - ecf->connections;   /* 减去连接数上限：得到当前 fd 压力值 */

        /* disable warning: Win32 SOCKET is u_int while UNIX socket is int */

        if ((ngx_uint_t) s >= ecf->connections) {   /* fd 编号超出配置的连接数组容量，为防数组越界必须丢弃 */

            /* 告警日志：新 fd 超出 connections 配额，丢弃该连接 */
            ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
                          "accept() on %s returned socket #%d while "   /* 格式串(分段拼接)：fd 超配额 */
                          "only %d connections was configured, "   /* 日志文本续 */
                          "closing the connection",   /* 日志文本续：将关闭该连接 */
                          ls->listening->addr_text.data, s, ecf->connections);   /* 参数：地址文本、fd 号、配额 */

            ngx_close_accepted_socket(s, log);   /* 关闭超限连接：close() 使内核发 FIN 走四次挥手断开 */
            ngx_destroy_pool(pool);   /* 销毁内存池 */
            return;   /* 结束本次事件处理 */
        }   /* fd 超配额分支结束 */

#if (NGX_STAT_STUB)                     /* 状态统计桩编译开关 */
        (*ngx_stat_active)++;           /* 全局"活跃连接数" +1 */
#endif   /* 状态统计桩块结束 */

        /* set a blocking mode for aio and non-blocking mode for the others */

        if (ngx_inherited_nonblocking) {             /* 监听 fd 继承了非阻塞标志的情形(如平滑升级时) */
            if ((ngx_event_flags & NGX_USE_AIO_EVENT)) {   /* AIO 机制恰好相反：要求连接 socket 为阻塞模式 */
                if (ngx_blocking(s) == -1) {         /* ioctl(FIONBIO,0) 清除 O_NONBLOCK，改回阻塞模式 */
                    /* 设置阻塞模式失败：记日志 */
                    ngx_log_error(NGX_LOG_ALERT, log, ngx_socket_errno,
                                  ngx_blocking_n " failed");   /* 参数：函数名与失败文案 */

                    ngx_close_accepted_socket(s, log);   /* 关闭该连接 */
                    ngx_destroy_pool(pool);   /* 销毁内存池 */
                    return;   /* 放弃该连接 */
                }   /* 置阻塞失败分支结束 */
            }   /* AIO 模式分支结束 */

        } else {                                     /* 普通情形：需要显式设置非阻塞 */
            if (!(ngx_event_flags & (NGX_USE_AIO_EVENT|NGX_USE_RTSIG_EVENT))) {   /* select/poll/epoll/kqueue 等常规事件机制 */
                if (ngx_nonblocking(s) == -1) {      /* 置 O_NONBLOCK：之后读写绝不睡眠阻塞进程，一切由事件机制驱动 */
                    /* 设置非阻塞失败：记日志 */
                    ngx_log_error(NGX_LOG_ALERT, log, ngx_socket_errno,
                                  ngx_nonblocking_n " failed");   /* 参数：函数名与失败文案 */

                    ngx_close_accepted_socket(s, log);   /* 关闭连接：内核发 FIN 断开 */
                    ngx_destroy_pool(pool);   /* 销毁内存池 */
                    return;   /* 放弃该连接 */
                }   /* 置非阻塞失败分支结束 */
            }   /* 常规事件机制分支结束 */
        }   /* 非阻塞模式设置结束 */

#if (WIN32)                             /* Windows 平台分支 */
        /*
         * Winsock assignes a socket number divisible by 4
         * so to find a connection we divide a socket number by 4.
         */

        if (s % 4) {                     /* Winsock 句柄恒为 4 的倍数，不满足则为致命异常 */
            /* 日志：句柄不能被 4 整除 */
            ngx_log_error(NGX_LOG_EMERG, ev->log, 0,
                          "accept() on %s returned socket #%d, "   /* 格式串(分段)：句柄非 4 倍数 */
                          "not divisible by 4",   /* 日志文本续 */
                          ls->listening->addr_text.data, s);   /* 参数：地址文本与句柄 */
            exit(1);                     /* 无法定位连接槽位，只能退出进程 */
        }   /* 句柄校验分支结束 */

        c = &ngx_cycle->connections[s / 4];     /* 句柄除以 4 定位连接数组槽位 */
        rev = &ngx_cycle->read_events[s / 4];   /* 同槽位的读事件 */
        wev = &ngx_cycle->write_events[s / 4];  /* 同槽位的写事件 */
#else                                   /* UNIX 分支 */
        c = &ngx_cycle->connections[s];     /* UNIX 下 fd 即数组下标：连接/读事件/写事件三张全局表按 fd 严格对齐 */
        rev = &ngx_cycle->read_events[s];   /* 同一 fd 下标的读事件槽位 */
        wev = &ngx_cycle->write_events[s];   /* 同一 fd 下标的写事件槽位 */
#endif   /* 平台分支结束 */

        instance = rev->instance;        /* 保存槽位上的旧代际戳，随后取反生成新戳以作废陈旧事件 */

#if (NGX_THREADS)                       /* 多线程支持 */

        if (*(&c->lock)) {               /* 该连接槽位正被其他线程占用(锁字非 0) */
            /* 调试日志：accept 中发生自旋等待 */
            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                           "spinlock in accept, fd:%", s);   /* 参数：fd 号 */
            ngx_spinlock(&c->lock, 1000);   /* 自旋等待：忙等而非睡眠，适合极短临界区 */
            ngx_unlock(&c->lock);        /* 获得后立即释放，保证槽位初始化的独占 */
        }   /* 自旋等待分支结束 */

#endif   /* 多线程支持结束 */

        ngx_memzero(rev, sizeof(ngx_event_t));      /* 清零复用的读事件槽位(残留上一连接的旧状态) */
        ngx_memzero(wev, sizeof(ngx_event_t));      /* 清零写事件槽位 */
        ngx_memzero(c, sizeof(ngx_connection_t));   /* 清零连接对象槽位 */

        c->pool = pool;                  /* 连接绑定内存池：连接关闭时统一销毁，避免逐块 free */

        c->listening = ls->listening;    /* 记录来源监听端口，供协议层按端口区分配置 */
        c->sockaddr = sa;                /* 对端地址(accept 填充，五元组之一) */
        c->socklen = len;                /* 对端地址实际长度 */

        rev->instance = !instance;       /* 读事件代际戳取反：事件机制据此识别并丢弃上一连接遗留的过期事件 */
        wev->instance = !instance;       /* 写事件同理 */

        rev->index = NGX_INVALID_INDEX;   /* 事件在驱动表(select/poll 数组等)中的索引置为无效，尚未注册 */
        wev->index = NGX_INVALID_INDEX;   /* 写事件索引同样置无效 */

        rev->data = c;   /* 事件回指所属连接：事件触发时由 data 取回完整上下文 */
        wev->data = c;   /* 写事件回指连接 */

        c->read = rev;    /* 连接持有读写事件指针，与事件形成双向关联 */
        c->write = wev;   /* 连接持有写事件指针 */

        c->fd = s;               /* 连接绑定新 fd，此后收发均通过它进行 */
        c->unexpected_eof = 1;   /* 预置"意外 EOF"标志：尚未收到对端关闭，读到 0 需特殊处理 */

        wev->write = 1;   /* 标记为写事件类型 */
        wev->ready = 1;   /* 写方向恒就绪：新连接的 TCP 发送缓冲区为空、通告窗口未耗尽，可直接写 */

        if (ngx_event_flags & (NGX_USE_AIO_EVENT|NGX_USE_RTSIG_EVENT)) {   /* aio/rtsig 这类一次性通知机制 */
            /* epoll, rtsig, aio, iocp */
            rev->ready = 1;              /* 先预置读就绪，避免依赖后续状态查询 */
        }   /* 一次性通知机制分支结束 */

        if (ev->deferred_accept) {       /* 启用了 TCP_DEFER_ACCEPT：内核推迟 accept 直到连接带数据到达 */
            rev->ready = 1;              /* 此时队头连接必已携带请求数据，首次读必然成功，直接标记就绪 */
        }   /* 延迟 accept 分支结束 */

        c->ctx = ls->ctx;                /* 继承监听 socket 上的模块上下文表(如 HTTP 的配置指针) */
        c->servers = ls->servers;        /* 继承该监听地址关联的虚拟主机数组，供按 Host 头匹配 */

        c->recv = ngx_recv;              /* 挂接缺省读函数：封装 recv()，把 EAGAIN 转化为事件等待 */
        c->send_chain = ngx_send_chain;  /* 挂接聚集写函数：writev 一次系统调用发多个缓冲区，减少内核态切换 */

        c->log = log;    /* 连接及其读写事件统一使用本连接的日志对象 */
        rev->log = log;   /* 读事件绑定日志对象 */
        wev->log = log;   /* 写事件绑定日志对象 */

        /*
         * TODO: MT: - atomic increment (x86: lock xadd)
         *             or protection by critical section or light mutex
         *
         * TODO: MP: - allocated in a shared memory
         *           - atomic increment (x86: lock xadd)
         *             or protection by critical section or light mutex
         */

        c->number = ngx_atomic_inc(ngx_connection_counter);   /* 原子自增全局连接计数器取唯一编号(x86 上是 lock xadd 前缀指令) */

#if (NGX_THREADS)                       /* 多线程锁字段初始化 */
        rev->lock = &c->lock;       /* 读写事件的锁指向连接自身的锁 */
        wev->lock = &c->lock;   /* 写事件锁指向连接锁 */
        rev->own_lock = &c->lock;   /* own_lock：事件跨线程转移时的属主锁 */
        wev->own_lock = &c->lock;   /* 写事件属主锁 */
#endif   /* 多线程锁初始化结束 */

        /* 调试日志：输出新 fd 与连接编号 */
        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "accept: fd:%d c:%d", s, c->number);   /* 参数：fd 与连接编号 */

        if (c->listening->addr_ntop) {   /* 配置要求把二进制地址转为可读文本 */
            /* 从连接池分配存放地址文本的缓冲区 */
            c->addr_text.data = ngx_palloc(c->pool,
                                           c->listening->addr_text_max_len);   /* 参数：缓冲区最大长度 */
            if (c->addr_text.data == NULL) {   /* 分配失败 */
                ngx_close_accepted_socket(s, log);   /* 关闭连接并清理退出 */
                ngx_destroy_pool(pool);   /* 销毁内存池 */
                return;   /* 放弃该连接 */
            }   /* 文本缓冲区分配失败分支结束 */
    
            /* 二进制 IP(网络字节序)转点分十进制文本，供日志与 $remote_addr 等使用 */
            c->addr_text.len = ngx_sock_ntop(c->listening->family, c->sockaddr,
                                             c->addr_text.data,   /* 参数：输出缓冲区 */
                                             c->listening->addr_text_max_len);   /* 参数：缓冲区上限 */
            if (c->addr_text.len == 0) {   /* 地址转换失败 */
                ngx_close_accepted_socket(s, log);   /* 关闭连接 */
                ngx_destroy_pool(pool);   /* 销毁内存池 */
                return;   /* 放弃该连接 */
            }   /* 地址转换失败分支结束 */
        }   /* 地址转文本分支结束 */

#if (NGX_DEBUG)                         /* 调试编译开关 */
        {                                /* 局部作用域：仅用于声明临时变量 */

        uint32_t            *addr;       /* debug_connection 配置的地址数组元素 */
        in_addr_t            i;          /* 循环下标 */
        struct sockaddr_in  *addr_in;    /* IPv4 地址结构视图 */

        addr_in = (struct sockaddr_in *) sa;   /* 把通用 sockaddr 强转为 IPv4 结构以读取 sin_addr 字段 */
        addr = ecf->debug_connection.elts;     /* 取 debug_connection 配置的客户端地址数组 */
        for (i = 0; i < ecf->debug_connection.nelts; i++) {   /* 逐一比对客户端来源 IP */
            if (addr[i] == addr_in->sin_addr.s_addr) {   /* 命中指定要调试的客户端 */
                log->log_level = NGX_LOG_DEBUG_CONNECTION|NGX_LOG_DEBUG_ALL;   /* 对该连接打开全部调试日志 */
                break;                     /* 命中即退出循环 */
            }   /* 地址匹配分支结束 */
        }   /* 调试地址遍历结束 */

        }                                /* 局部作用域结束 */
#endif   /* 调试代码块结束 */

        if (ngx_add_conn && (ngx_event_flags & NGX_USE_EPOLL_EVENT) == 0) {   /* 非 epoll 机制：需把新连接显式注册进事件驱动表(select/poll/devpoll 提供 add_conn) */
            if (ngx_add_conn(c) == NGX_ERROR) {   /* 同时注册读与写两类事件的监听 */
                ngx_close_accepted_socket(s, log);   /* 关闭连接 */
                ngx_destroy_pool(pool);   /* 销毁内存池 */
                return;   /* 放弃该连接 */
            }   /* 注册失败分支结束 */
        }   /* 非 epoll 注册分支结束 */

        pool = NULL;     /* 内存池所有权已随连接移交，置空防止下一轮循环误销毁 */

        log->data = NULL;     /* 清空日志上下文：日志对象已随连接移交给 handler，之后不再按 accept 场景解释 */
        log->handler = NULL;   /* 清空日志回调函数 */

        ls->listening->handler(c);   /* 回调监听端口注册的处理器(HTTP 模块即 ngx_http_init_request)：新连接进入协议解析阶段 */

        if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {   /* kqueue 会精确报告剩余待 accept 数 */
            ev->available--;             /* 每取走一个连接扣减一次 */
        }   /* kqueue 扣减分支结束 */

        accepted++;      /* 本轮成功 accept 计数 +1 */

    } while (ev->available);   /* 队列仍有剩余连接(或未达 multi_accept 上限)则继续循环取 */
}   /* ngx_event_accept 函数结束 */


/*
 * ngx_trylock_accept_mutex：以非阻塞方式尝试抢占 accept 互斥锁(共享内存中的原子
 * 变量，0 表示空闲，非 0 表示持有者的 pid)。
 * 理论背景——"惊群"(thundering herd)：多个 worker 都在监听 fd 上等待读事件时，
 * 内核每完成一次握手(连接进入全连接队列)会唤醒所有等待进程，但只有一个 accept()
 * 成功，其余进程被白白调度又空手而归，高并发下严重浪费 CPU。
 * nginx 0.1.0 的解法：只有抢到锁的 worker 才把监听 socket 的读事件注册进事件
 * 机制(ngx_enable_accept_events)，其余 worker 摘除监听事件，新连接只唤醒持锁
 * 进程；未抢到锁的进程仍照常处理自己已建立的连接。
 */
ngx_int_t ngx_trylock_accept_mutex(ngx_cycle_t *cycle)
{   /* 函数体开始 */
    if (*ngx_accept_mutex == 0   /* 锁当前空闲(0=无人持有) */
        && ngx_atomic_cmp_set(ngx_accept_mutex, 0, ngx_pid))   /* 原子 CAS(硬件 lock cmpxchg)：期望 0 则写入本进程 pid，多进程下仅一个成功 */
    {   /* 抢锁成功分支 */
        /* 调试日志：成功获得 accept 互斥锁 */
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "accept mutex locked");   /* 参数：日志文案 */

        if (!ngx_accept_mutex_held) {   /* 本进程此前未持锁(避免重复注册监听事件) */
            if (ngx_enable_accept_events(cycle) == NGX_ERROR) {   /* 把所有监听 socket 的读事件注册进事件机制 */
                *ngx_accept_mutex = 0;   /* 注册失败：回滚释放锁 */
                return NGX_ERROR;   /* 注册监听事件失败：释放锁后向上层报告错误 */
            }   /* 注册失败回滚分支结束 */

            ngx_accept_mutex_held = 1;   /* 标记本进程持有 accept 锁 */
        }   /* 首次持锁分支结束 */

        return NGX_OK;   /* 持锁成功返回 */
    }   /* 抢锁分支结束 */

    if (ngx_accept_mutex_held) {   /* 抢锁失败且本进程还持着上一轮的锁 */
        if (ngx_disable_accept_events(cycle) == NGX_ERROR) {   /* 摘除监听读事件，把 accept 机会让给新持锁者 */
            return NGX_ERROR;   /* 摘除失败上报 */
        }   /* 摘除失败分支结束 */

        ngx_accept_mutex_held = 0;   /* 清除本进程的持锁标志 */
    }   /* 让锁分支结束 */

    return NGX_OK;   /* 无论是否持锁都正常返回 */
}   /* ngx_trylock_accept_mutex 函数结束 */


/*
 * ngx_enable_accept_events：持锁 worker 专用——把所有监听 socket 的读事件重新
 * 注册进事件机制。此后内核全连接队列一有新连接(ESTABLISHED 入队)就只唤醒本进程。
 */
ngx_int_t ngx_enable_accept_events(ngx_cycle_t *cycle)
{   /* 函数体开始 */
    ngx_uint_t        i;   /* 循环下标 */
    ngx_listening_t  *s;   /* 监听 socket 数组指针 */

    s = cycle->listening.elts;   /* 取监听数组首地址 */
    for (i = 0; i < cycle->listening.nelts; i++) {   /* 遍历每一个监听端口 */

        /*
         * we do not need to handle the Winsock sockets here (divide a socket
         * number by 4) because this function would never called
         * in the Winsock environment
         */

        if (ngx_event_flags & NGX_USE_RTSIG_EVENT) {   /* realtime 信号机制按连接整体注册 */
            if (ngx_add_conn(&cycle->connections[s[i].fd]) == NGX_ERROR) {   /* 按连接对象同时注册读写事件 */
                return NGX_ERROR;   /* 注册失败上报 */
            }   /* rtsig 注册失败分支结束 */

        } else {   /* select/poll/epoll 等常规机制 */
            /* 把监听 fd 的读事件加入事件驱动表：监听 socket 可读 = 内核全连接队列非空 */
            if (ngx_add_event(&cycle->read_events[s[i].fd], NGX_READ_EVENT, 0)
                                                                  == NGX_ERROR)   /* 条件续行：与 NGX_ERROR 比较 */
            {   /* 注册失败分支 */
                return NGX_ERROR;   /* 注册失败上报 */
            }   /* add_event 失败分支结束 */
        }   /* rtsig/常规机制分支结束 */
    }   /* 监听数组遍历结束 */

    return NGX_OK;   /* 全部注册成功 */
}   /* ngx_enable_accept_events 函数结束 */


/*
 * ngx_disable_accept_events：失去 accept 锁的 worker 调用——把所有监听 socket
 * 的读事件从事件机制中摘除，不再参与新连接的竞争(化解惊群)。
 */
ngx_int_t ngx_disable_accept_events(ngx_cycle_t *cycle)
{   /* 函数体开始 */
    ngx_uint_t        i;   /* 循环下标 */
    ngx_listening_t  *s;   /* 监听 socket 数组指针 */

    s = cycle->listening.elts;   /* 取监听数组首地址 */
    for (i = 0; i < cycle->listening.nelts; i++) {   /* 遍历每一个监听端口 */

        /*
         * we do not need to handle the Winsock sockets here (divide a socket
         * number by 4) because this function would never called
         * in the Winsock environment
         */

        if (ngx_event_flags & NGX_USE_RTSIG_EVENT) {   /* realtime 信号机制按连接摘除 */
            if (!cycle->connections[s[i].fd].read->active) {   /* 该监听连接的读事件本就未注册 */
                continue;              /* 跳过，无需摘除 */
            }   /* 未注册则跳过分支结束 */

            /* 从内核事件表中摘除该连接的事件监听 */
            if (ngx_del_conn(&cycle->connections[s[i].fd], NGX_DISABLE_EVENT)
                                                                  == NGX_ERROR)   /* 条件续行：与 NGX_ERROR 比较 */
            {   /* 摘除失败分支 */
                return NGX_ERROR;   /* 摘除失败上报 */
            }   /* del_conn 失败分支结束 */

        } else {   /* select/poll/epoll 等常规机制 */
            if (!cycle->read_events[s[i].fd].active) {   /* 该监听读事件本就未注册 */
                continue;              /* 跳过 */
            }   /* 未注册则跳过分支结束 */

            /* 从事件驱动表中摘除监听 fd 的读事件(不关闭 socket，仅不再通知) */
            if (ngx_del_event(&cycle->read_events[s[i].fd], NGX_READ_EVENT,
                                               NGX_DISABLE_EVENT) == NGX_ERROR)   /* 条件续行：与 NGX_ERROR 比较 */
            {   /* 摘除失败分支 */
                return NGX_ERROR;   /* 摘除失败上报 */
            }   /* del_event 失败分支结束 */
        }   /* rtsig/常规机制分支结束 */
    }   /* 监听数组遍历结束 */

    return NGX_OK;   /* 全部摘除成功 */
}   /* ngx_disable_accept_events 函数结束 */


/*
 * ngx_close_accepted_socket：关闭刚 accept 的 fd。close() 后内核回收 socket 资源，
 * 若发送缓冲区仍有未发完的数据，按 SO_LINGER 语义发送 FIN 并进入 FIN_WAIT1 ->
 * FIN_WAIT2/TIME_WAIT 的四次挥手序列，最终由内核释放五元组。
 */
static void ngx_close_accepted_socket(ngx_socket_t s, ngx_log_t *log)
{   /* 函数体开始 */
    if (ngx_close_socket(s) == -1) {   /* 关闭失败(如 fd 已被并发关闭) */
        /* 记 ALERT 级日志 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_socket_errno,
                      ngx_close_socket_n " failed");   /* 参数：函数名与失败文案 */
    }   /* 关闭失败分支结束 */
}   /* ngx_close_accepted_socket 函数结束 */


/*
 * ngx_accept_log_error：日志后缀回调。accept 出错时 ngx_log_error 调用本函数，
 * 在错误信息后追加 " while accept() on IP:PORT"，指明是哪个监听端口出错。
 */
static size_t ngx_accept_log_error(void *data, char *buf, size_t len)
{   /* 函数体开始 */
    ngx_accept_log_ctx_t  *ctx = data;   /* 从日志对象的 data 指针还原 accept 上下文 */

    return ngx_snprintf(buf, len, " while accept() on %s", ctx->name);   /* 格式化日志后缀并返回其长度 */
}   /* ngx_accept_log_error 函数结束 */
