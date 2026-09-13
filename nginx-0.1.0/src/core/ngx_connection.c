
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>          /* 平台配置头: 由configure脚本生成, 定义各OS特性宏与基础类型 */
#include <ngx_core.h>            /* nginx核心头: 内存池/数组/连接等核心数据结构与其API声明 */
#include <ngx_event.h>           /* 事件框架头: select/poll/epoll等事件模块的结构与接口声明 */


ngx_os_io_t  ngx_io;             /* 全局I/O方法表: 保存事件模块注册的recv/send等具体I/O函数指针 */


/*
 * 整体功能: 为每条listen指令构造一个监听对象ngx_listening_t并追加到cycle->listening数组。
 * 本函数只做"配置期"的数据准备——填充struct sockaddr_in(sin_family=AF_INET、sin_addr、
 * sin_port); 真正的socket()/bind()/listen()系统调用由ngx_open_listening_sockets()完成。
 * 注意sin_port必须htons()转成网络字节序(大端): x86是小端机, 不转换则内核按大端
 * 解析出的端口完全错误; 而入参addr已按网络序传入, 直接赋值即可。
 */
ngx_listening_t *ngx_listening_inet_stream_socket(ngx_conf_t *cf,    /* 配置解析上下文(内存池/日志) */
                                                 in_addr_t addr,     /* 32位IPv4地址, 网络字节序 */
                                                 in_port_t port)     /* 端口号, 主机字节序 */
{                                                     /* 函数体开始 */
    size_t               len;          /* 临时变量: 记录点分十进制IP文本的长度 */
    ngx_listening_t     *ls;           /* 指向新追加的监听对象 */
    struct sockaddr_in  *addr_in;      /* IPv4套接字地址结构(family+port+addr, 共16字节) */

    if (!(ls = ngx_array_push(&cf->cycle->listening))) {    /* 向监听数组追加1个元素并返回其地址 */
        return NULL;                                        /* 数组扩容(内存池分配)失败 */
    }   /* if结束 */

    ngx_memzero(ls, sizeof(ngx_listening_t));               /* 整块清零, 让所有字段处于可预期的默认状态 */

    if (!(addr_in = ngx_pcalloc(cf->pool, sizeof(struct sockaddr_in)))) {    /* 从配置内存池分配并清零sockaddr_in */
        return NULL;                                        /* 内存分配失败, 中止配置解析 */
    }   /* if结束 */

#if (HAVE_SIN_LEN)                                       /* 仅4.4BSD衍生协议栈定义此特性宏 */
    addr_in->sin_len = sizeof(struct sockaddr_in);       /* BSD内核要求填充地址结构自身长度用于校验 */
#endif                                                   /* 条件编译结束 */
    addr_in->sin_family = AF_INET;                        /* 地址族=IPv4, 内核据此路由到inet协议栈 */
    addr_in->sin_addr.s_addr = addr;                      /* 32位IP, 网络字节序(大端)直接存放 */
    addr_in->sin_port = htons(port);                      /* 端口转网络序: 内核按大端读取, 小端机必须转换 */

    if (!(ls->addr_text.data = ngx_palloc(cf->pool, INET_ADDRSTRLEN + 6))) {    /* 分配"IP:port"显示文本缓冲 */
        return NULL;                                      /* 15字节IP+冒号+5位端口, 分配失败则退出 */
    }   /* if结束 */

    len = ngx_inet_ntop(AF_INET, &addr, ls->addr_text.data, INET_ADDRSTRLEN);    /* 二进制IP转点分十进制, 返回长度 */
    ls->addr_text.len = ngx_snprintf((char *) ls->addr_text.data + len,    /* 紧接IP文本之后拼接":port" */
                                     6, ":%d", port);                      /* 得到形如"1.2.3.4:80"的完整地址串 */

    ls->fd = (ngx_socket_t) -1;                           /* fd置-1: 尚未socket(), -1是"未打开"的约定标记 */
    ls->family = AF_INET;                                 /* 记录协议族, 供socket()第一个参数使用 */
    ls->type = SOCK_STREAM;                               /* 套接字类型=字节流, 在INET族下即TCP */
    ls->protocol = IPPROTO_IP;                            /* 协议号取默认, 由内核按family+type选定TCP */
#if (WIN32)                                              /* Windows平台才有此分支 */
    ls->flags = WSA_FLAG_OVERLAPPED;                     /* 要求重叠I/O套接字, 供IOCP事件模型使用 */
#endif                                                   /* 条件编译结束 */
    ls->sockaddr = (struct sockaddr *) addr_in;           /* 通用地址指针, bind()时原样传给内核 */
    ls->socklen = sizeof(struct sockaddr_in);             /* 地址长度16字节, bind()/getsockname()需要 */
    ls->addr = offsetof(struct sockaddr_in, sin_addr);    /* 记录sin_addr偏移, accept后按偏移提取对端IP */
    ls->addr_text_max_len = INET_ADDRSTRLEN;              /* 纯IP文本最大长度15(不含结尾'\0') */

    return ls;                                            /* 返回监听对象(已挂入cycle->listening数组) */
}   /* ngx_listening_inet_stream_socket结束 */


/*
 * 整体功能: 处理从老master进程继承来的监听fd(平滑升级/热重启时新进程经环境变量
 * 拿到已处于LISTEN状态的socket)。这些fd无需重新socket()/bind()/listen(), 只需
 * 调用getsockname()向内核反查每个fd绑定的本端地址(IP:端口), 重建sockaddr与
 * 可读地址文本, 供日志输出和后续配置匹配使用。
 */
ngx_int_t ngx_set_inherited_sockets(ngx_cycle_t *cycle)    /* 重建各继承监听的本端地址信息 */
{                                             /* 函数体开始 */
    ngx_uint_t           i;        /* 循环下标 */
    ngx_listening_t     *ls;       /* 监听数组首元素指针 */
    struct sockaddr_in  *addr_in;  /* IPv4地址结构临时指针 */

    ls = cycle->listening.elts;                        /* 取监听数组数据区首地址 */
    for (i = 0; i < cycle->listening.nelts; i++) {     /* 逐个处理每个监听对象 */

        /* AF_INET only */                              /* (原有注释)当前版本只支持IPv4 */

        ls[i].sockaddr = ngx_palloc(cycle->pool, sizeof(struct sockaddr_in));    /* 为该监听分配sockaddr_in */
        if (ls[i].sockaddr == NULL) {                   /* 内存分配失败 */
            return NGX_ERROR;                           /* 初始化失败, master退出 */
        }   /* if结束 */

        ls[i].socklen = sizeof(struct sockaddr_in);     /* 先写入缓冲容量: getsockname是值-结果参数 */
        if (getsockname(ls[i].fd, ls[i].sockaddr, &ls[i].socklen) == -1) {    /* 向内核查询fd绑定的本端地址 */
            ngx_log_error(NGX_LOG_CRIT, cycle->log, ngx_socket_errno,    /* 以CRIT级记录内核errno */
                          "getsockname() of the inherited "             /* 日志格式串(两段字符串拼接) */
                          "socket #%d failed", ls[i].fd);               /* 第二段+参数: 监听fd号 */
            ls[i].ignore = 1;                           /* 标记该监听异常, 打开阶段跳过它 */
            continue;                                   /* 继续处理下一个监听 */
        }   /* if结束 */

        addr_in = (struct sockaddr_in *) ls[i].sockaddr;    /* 通用sockaddr还原为sockaddr_in便于取字段 */

        if (addr_in->sin_family != AF_INET) {           /* 校验内核返回的地址族 */
            ngx_log_error(NGX_LOG_CRIT, cycle->log, ngx_socket_errno,    /* 记录CRIT日志 */
                          "the inherited socket #%d has "               /* 格式串前半 */
                          "unsupported family", ls[i].fd);              /* 后半+fd参数 */
            ls[i].ignore = 1;                           /* 非IPv4(如UNIX域socket)则标记忽略 */
            continue;                                   /* 处理下一个 */
        }   /* if结束 */
        ls[i].addr_text_max_len = INET_ADDRSTRLEN;      /* IPv4点分十进制最长15字节+'\0' */

        ls[i].addr_text.data = ngx_palloc(cycle->pool, ls[i].addr_text_max_len);    /* 为地址文本分配缓冲 */
        if (ls[i].addr_text.data == NULL) {             /* 分配失败 */
            return NGX_ERROR;                           /* 初始化失败 */
        }   /* if结束 */

        ls[i].family = addr_in->sin_family;             /* 保存协议族AF_INET */
        ls[i].addr_text.len = ngx_sock_ntop(ls[i].family, ls[i].sockaddr,    /* 二进制地址转点分十进制文本 */
                                            ls[i].addr_text.data,            /* 输出缓冲区 */
                                            ls[i].addr_text_max_len);        /* 缓冲区长度上限 */
        if (ls[i].addr_text.len == 0) {                 /* 转换失败返回0 */
            return NGX_ERROR;                           /* 初始化失败 */
        }   /* if结束 */
    }   /* for循环结束 */

    return NGX_OK;                                      /* 所有继承的监听fd处理完毕 */
}   /* ngx_set_inherited_sockets结束 */


/*
 * 整体功能: 服务端TCP编程的教科书路径, 对每个监听对象依次执行:
 *   socket()  —— 内核新建struct sock等控制块并返回文件描述符fd;
 *   setsockopt(SO_REUSEADDR) —— 允许绑定处于TIME_WAIT状态(TCP四次挥手后主动关闭方
 *      停留2*MSL)的本地地址, 避免重启时报"Address already in use";
 *   bind()    —— 把fd绑定到sockaddr_in中的IP+端口, 内核将其登记进传输层端口哈希表;
 *   listen()  —— socket转入LISTEN状态, 内核创建半连接队列(SYN队列)与全连接队列
 *      (accept队列), 全连接队列长度取backlog与net.core.somaxconn的较小值;
 *   ngx_nonblocking() —— ioctl(FIONBIO)设非阻塞, accept不会阻塞worker。
 * 绑定失败(EADDRINUSE, 如老进程尚未释放端口)时休眠500ms重试, 最多5轮。
 */
ngx_int_t ngx_open_listening_sockets(ngx_cycle_t *cycle)    /* 打开(创建并激活)全部监听socket */
{                                          /* 函数体开始 */
    ngx_uint_t        tries, failed, reuseaddr, i;   /* 重试轮数/失败标志/SO_REUSEADDR值/下标 */
    ngx_err_t         err;                           /* 保存内核错误码errno */
    ngx_log_t        *log;                           /* 日志对象指针, 缓存起来频繁使用 */
    ngx_socket_t      s;                             /* socket()返回的文件描述符 */
    ngx_listening_t  *ls;                            /* 监听数组指针 */

    reuseaddr = 1;                                    /* SO_REUSEADDR选项值置1(允许重用TIME_WAIT地址) */
#if (NGX_SUPPRESS_WARN)                               /* 仅为消除编译器警告的分支 */
    failed = 0;                                       /* 预置失败标志, 避免"可能未初始化"告警 */
#endif                                                /* 条件编译结束 */

    log = cycle->log;                                 /* 缓存日志对象 */

    /* TODO: tries configurable */                    /* (原有TODO注释)重试次数待做成可配置 */

    for (tries = /* STUB */ 5; tries; tries--) {      /* 最多尝试5轮(桩值硬编码) */
        failed = 0;                                   /* 每轮开始时清零失败标志 */

        /* for each listening socket */               /* (原有注释)对每个监听socket */

        ls = cycle->listening.elts;                   /* 取监听数组首地址 */
        for (i = 0; i < cycle->listening.nelts; i++) {    /* 遍历全部监听对象 */

            if (ls[i].ignore) {                       /* 之前标记为不可用的监听(如getsockname失败) */
                continue;                             /* 跳过, 不参与打开 */
            }   /* if结束 */

            if (ls[i].fd != -1) {                     /* fd非-1说明socket已打开(前一轮重试已成功) */
                continue;                             /* 无需重复创建 */
            }   /* if结束 */

            if (ls[i].inherited) {                    /* 该fd是从老master继承来的 */

                /* TODO: close on exit */             /* (原有TODO)退出时应关闭 */
                /* TODO: nonblocking */               /* (原有TODO)应设非阻塞 */
                /* TODO: deferred accept */           /* (原有TODO)应支持延迟accept */

                continue;                             /* 继承fd已是LISTEN态, 跳过下面全部步骤 */
            }   /* if结束 */

            s = ngx_socket(ls[i].family, ls[i].type, ls[i].protocol,    /* 系统调用socket(): 内核创建套接字并返回fd */
                           ls[i].flags);                               /* 参数: 协议族/类型/协议/创建标志 */

            if (s == -1) {                            /* socket()失败(如fd耗尽) */
                ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,    /* 以EMERG级记录errno */
                              ngx_socket_n " %s failed", ls[i].addr_text.data);    /* 日志展开即"socket() 地址 failed" */
                return NGX_ERROR;                     /* 致命错误, master启动失败 */
            }   /* if结束 */

#if (WIN32)                                           /* Windows平台 */
            /*
             * Winsock assignes a socket number divisible by 4
             * so to find a connection we divide a socket number by 4.
             */                                       /* (原有注释)Winsock句柄恒为4的倍数, 句柄/4可作连接表下标 */

            if (s % 4) {                              /* 句柄不是4的倍数则nginx的索引假设被破坏 */
                ngx_log_error(NGX_LOG_EMERG, ls->log, 0,                   /* 记录EMERG错误 */
                              ngx_socket_n " created socket %d", s);       /* 打印异常句柄值 */
                return NGX_ERROR;                     /* 无法继续运行 */
            }   /* if结束 */
#endif                                                /* WIN32专用代码结束 */

            if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR,                 /* 设置套接字层SO_REUSEADDR选项 */
                           (const void *) &reuseaddr, sizeof(int)) == -1) {    /* 选项值1, 选项长度4字节 */
                ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,    /* 失败时记录errno */
                              "setsockopt(SO_REUSEADDR) %s failed",    /* 日志文本 */
                              ls[i].addr_text.data);                   /* 监听地址 */
                return NGX_ERROR;                     /* 致命错误 */
            }   /* if结束 */

            /* TODO: close on exit */                /* (原有TODO)进程退出时自动关闭socket */

            if (!(ngx_event_flags & NGX_USE_AIO_EVENT)) {     /* 非AIO事件模式时监听socket须为非阻塞 */
                if (ngx_nonblocking(s) == -1) {               /* ioctl(FIONBIO)设置O_NONBLOCK标志 */
                    ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,    /* 失败记录errno */
                                  ngx_nonblocking_n " %s failed",           /* 日志展开即"ioctl(FIONBIO, 1) failed" */
                                  ls[i].addr_text.data);                    /* 监听地址 */
                    return NGX_ERROR;                         /* 致命错误 */
                }   /* if结束 */
            }   /* if结束 */

#if 0                                                 /* 被禁用的历史代码 */
            if (ls[i].nonblocking) {                  /* 原设想按配置项决定是否设非阻塞 */
                if (ngx_nonblocking(s) == -1) {       /* 设非阻塞失败 */
                    ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,    /* 记录EMERG日志 */
                                  ngx_nonblocking_n " %s failed",           /* 日志文本 */
                                  ls[i].addr_text.data);                    /* 监听地址 */
                    return NGX_ERROR;                 /* 失败退出 */
                }   /* if结束 */
            }                                         /* if结束 */
#endif                                                /* 禁用代码块结束 */

            if (bind(s, ls[i].sockaddr, ls[i].socklen) == -1) {    /* 绑定IP+端口, 内核登记到端口哈希表 */
                err = ngx_socket_errno;               /* 保存errno, 供分类判断与日志 */
                ngx_log_error(NGX_LOG_EMERG, log, err,             /* 记录EMERG日志并带errno */
                              "bind() to %s failed", ls[i].addr_text.data);    /* 监听地址 */

                if (err != NGX_EADDRINUSE)            /* 非地址占用错误(如绑定1024以下端口权限不足) */
                    return NGX_ERROR;                 /* 无重试意义, 直接失败 */

                if (ngx_close_socket(s) == -1)        /* 端口被占: 先关掉刚创建的socket释放fd */
                    ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,    /* close也失败则记录 */
                                  ngx_close_socket_n " %s failed",         /* 日志展开即"close() 地址 failed" */
                                  ls[i].addr_text.data);                   /* 监听地址 */

                failed = 1;                           /* 置失败标志, 触发整轮500ms后重试 */
                continue;                             /* 处理下一个监听 */
            }   /* if结束 */

            if (listen(s, ls[i].backlog) == -1) {     /* 转LISTEN态: 内核建立SYN队列与accept队列 */
                ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,    /* 失败记录errno */
                              "listen() to %s failed", ls[i].addr_text.data);    /* 监听地址 */
                return NGX_ERROR;                     /* 致命错误 */
            }   /* if结束 */

            /* TODO: deferred accept */              /* (原有TODO)考虑TCP_DEFER_ACCEPT */

            ls[i].fd = s;                            /* 保存成功的fd, 该监听就此就绪 */
        }   /* 内层for(所有监听)结束 */

        if (!failed)                                 /* 本轮所有监听socket都打开成功 */
            break;                                   /* 跳出重试循环 */

        /* TODO: delay configurable */               /* (原有TODO)重试间隔待做成可配置 */

        ngx_log_error(NGX_LOG_NOTICE, log, 0,        /* NOTICE级提示即将重试 */
                      "try again to bind() after 500ms");    /* 日志文本 */
        ngx_msleep(500);                             /* 休眠500ms, 等端口释放(老进程退出/TIME_WAIT结束) */
    }   /* 外层for(重试轮数)结束 */

    if (failed) {                                    /* 5轮过后仍有绑定失败的监听 */
        ngx_log_error(NGX_LOG_EMERG, log, 0, "still can not bind()");    /* 记录最终失败 */
        return NGX_ERROR;                            /* master启动失败 */
    }   /* if结束 */

    return NGX_OK;                                   /* 全部监听socket就绪 */
}   /* ngx_open_listening_sockets结束 */


/*
 * 整体功能: 进程退出/重载配置时关闭全部监听socket。关闭前先把读事件从内核事件表
 * 摘除, 再close()——close回收描述符并让内核释放该socket的监听结构; 同时把
 * cycle->connections连接池中对应槽位的fd置-1, 恢复为空闲槽位。
 */
void ngx_close_listening_sockets(ngx_cycle_t *cycle)    /* 关闭全部监听socket */
{                                            /* 函数体开始 */
    ngx_uint_t        i;       /* 循环下标 */
    ngx_socket_t      fd;      /* 暂存fd(Windows下需换算) */
    ngx_listening_t  *ls;      /* 监听数组指针 */

    if (ngx_event_flags & NGX_USE_IOCP_EVENT) {   /* Windows IOCP事件模式 */
        return;                                   /* 监听socket另有处理路径, 直接返回 */
    }   /* if结束 */

    ngx_accept_mutex_held = 0;                    /* 清除"当前worker持有accept锁"的标志 */
    ngx_accept_mutex = NULL;                      /* 释放accept互斥锁(多worker间防accept惊群) */

    ls = cycle->listening.elts;                   /* 取监听数组首地址 */
    for (i = 0; i < cycle->listening.nelts; i++) {    /* 逐个关闭监听socket */
        fd = ls[i].fd;                            /* 暂存fd, 供连接池下标换算使用 */

#if (WIN32)                                       /* Windows平台 */
        /*
         * Winsock assignes a socket number divisible by 4
         * so to find a connection we divide a socket number by 4.
         */                                       /* (原有注释)句柄/4即为连接表下标 */

        fd /= 4;                                  /* 把套接字句柄换算成连接池下标 */
#endif                                            /* WIN32专用代码结束 */

        if (ngx_event_flags & NGX_USE_RTSIG_EVENT) {           /* realtime信号事件模式 */
            if (cycle->connections[fd].read->active) {         /* 该连接的读事件仍注册在内核 */
                ngx_del_conn(&cycle->connections[fd], NGX_CLOSE_EVENT);    /* 以连接为单位从内核事件表摘除 */
            }   /* if结束 */

        } else {                                               /* select/poll/epoll等通用模式 */
            if (cycle->read_events[fd].active) {               /* 监听fd的读事件处于active状态 */
                ngx_del_event(&cycle->read_events[fd],         /* 从内核事件表删除该读事件 */
                              NGX_READ_EVENT, NGX_CLOSE_EVENT);    /* 事件类型: 读+关闭 */
            }   /* if结束 */
        }   /* if-else结束 */

        if (ngx_close_socket(ls[i].fd) == -1) {   /* close(): 内核回收描述符与socket控制块 */
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,    /* 失败记录errno */
                          ngx_close_socket_n " %s failed",     /* 日志展开即"close() 地址 failed" */
                          ls[i].addr_text.data);               /* 监听地址 */
        }   /* if结束 */

        cycle->connections[fd].fd = (ngx_socket_t) -1;    /* 连接池对应槽位fd置-1, 归还空闲链表语义 */
    }   /* for循环结束 */
}   /* ngx_close_listening_sockets结束 */


/*
 * 整体功能: 关闭一条已建立的连接。次序: SSL关停(可能需异步等待对端close_notify)
 * →删读写定时器→从内核事件表摘除事件→删除posted延迟事件并置closed标志→
 * fd置-1→销毁连接内存池→close()。close()触发TCP四次挥手(FIN/FIN+ACK/ACK);
 * 连接生存期内的所有内存经内存池一次归还——nginx"按生命周期分配内存"理论的体现。
 */
void ngx_close_connection(ngx_connection_t *c)    /* 关闭并回收一条连接 */
{                                     /* 函数体开始 */
    ngx_socket_t  fd;                 /* 先备份fd, 因为连接及其log稍后随内存池一起销毁 */

    if (c->pool == NULL) {            /* 内存池指针为NULL: 说明连接已被关闭过 */
        ngx_log_error(NGX_LOG_ALERT, c->log, 0, "connection already closed");    /* ALERT级告警重复关闭 */
        return;                       /* 直接返回, 防止二次释放 */
    }   /* if结束 */
    
#if (NGX_OPENSSL)                     /* 编译进OpenSSL支持时 */

    if (c->ssl) {                     /* 该连接启用了SSL/TLS */
        if (ngx_ssl_shutdown(c) == NGX_AGAIN) {    /* SSL_shutdown未完成(还在等对端close_notify) */
            c->read->event_handler = ngx_ssl_close_handler;    /* 读事件改挂SSL关停处理器 */
            c->write->event_handler = ngx_ssl_close_handler;   /* 写事件同样改挂关停处理器 */
            return;                    /* 延迟关闭: 等TLS关停事件到来后再收尾 */
        }   /* if结束 */
    }   /* if结束 */
    
#endif                                /* 条件编译结束 */

    if (c->read->timer_set) {         /* 读方向挂有定时器(keepalive/读超时等) */
        ngx_del_timer(c->read);       /* 从定时器红黑树中删除 */
    }   /* if结束 */
    
    if (c->write->timer_set) {        /* 写方向挂有定时器 */
        ngx_del_timer(c->write);      /* 删除写定时器 */
    }   /* if结束 */
    
    if (ngx_del_conn) {               /* 事件模块提供了按连接删除的接口(如rtsig) */
        ngx_del_conn(c, NGX_CLOSE_EVENT);    /* 整个连接一次从内核事件表摘除 */

    } else {                          /* 否则按读写事件分别摘除 */
        if (c->read->active || c->read->disabled) {      /* 读事件在内核事件表中或已禁用 */
            ngx_del_event(c->read, NGX_READ_EVENT, NGX_CLOSE_EVENT);    /* 摘除读事件 */
        }   /* if结束 */

        if (c->write->active || c->write->disabled) {    /* 写事件在内核事件表中或已禁用 */
            ngx_del_event(c->write, NGX_WRITE_EVENT, NGX_CLOSE_EVENT);    /* 摘除写事件 */
        }   /* if结束 */
    }   /* if-else结束 */

#if (NGX_THREADS)                     /* 多线程支持 */

    /*
     * we have to clean the connection information before the closing
     * because another thread may reopen the same file descriptor
     * before we clean the connection
     */                                /* (原有注释)必须在close前清理: 否则他线程可能复用同一fd号造成误操作 */

    if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_OK) {    /* 加锁保护posted事件全局队列 */

        if (c->read->prev) {          /* 读事件挂在posted队列中 */
            ngx_delete_posted_event(c->read);    /* 从队列摘除, 防止之后被误执行 */
        }   /* if结束 */

        if (c->write->prev) {         /* 写事件挂在posted队列中 */
            ngx_delete_posted_event(c->write);   /* 摘除 */
        }   /* if结束 */

        c->read->closed = 1;          /* 读事件置closed: 即便残留陈旧事件也会被忽略 */
        c->write->closed = 1;         /* 写事件置closed */

        if (c->single_connection) {   /* 该连接声明为单线程独占时 */
            ngx_unlock(&c->lock);     /* 释放连接自身的锁 */
            c->read->locked = 0;      /* 清读事件加锁标志 */
            c->write->locked = 0;     /* 清写事件加锁标志 */
        }   /* if结束 */

        ngx_mutex_unlock(ngx_posted_events_mutex);    /* 解锁posted事件队列 */
    }   /* if结束 */

#else                                 /* 单线程编译 */

    if (c->read->prev) {              /* 读事件在posted队列 */
        ngx_delete_posted_event(c->read);    /* 摘除 */
    }   /* if结束 */

    if (c->write->prev) {             /* 写事件在posted队列 */
        ngx_delete_posted_event(c->write);   /* 摘除 */
    }   /* if结束 */

    c->read->closed = 1;              /* 读事件置closed标志 */
    c->write->closed = 1;             /* 写事件置closed标志 */

#endif                                /* 条件编译结束 */

    fd = c->fd;                       /* 备份fd供最后close使用 */
    c->fd = (ngx_socket_t) -1;        /* 连接的fd置-1: 从此刻起该连接不再关联任何socket */
    c->data = NULL;                   /* 清空通用数据指针 */

    ngx_destroy_pool(c->pool);        /* 销毁连接内存池: 连接期间的所有分配一次释放(含c->log) */

    if (ngx_close_socket(fd) == -1) {    /* close(): 内核发FIN进入四次挥手并回收fd */

        /* we use ngx_cycle->log because c->log was in c->pool */    /* (原有注释)c->log已随内存池销毁 */

        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, ngx_socket_errno,    /* 只能改用全局日志记录 */
                      ngx_close_socket_n " failed");    /* 日志文本即"close() failed" */
    }   /* if结束 */
}   /* ngx_close_connection结束 */



/*
 * 整体功能: 连接错误的日志分级。对TCP/IP层面的"常态"错误——ECONNRESET(对端RST
 * 复位)、EPIPE(向已关闭连接写数据)、ENOTCONN、ECONNREFUSED(对端无监听回RST)、
 * EHOSTUNREACH(路由不可达)——按连接的log_error配置映射为INFO/ERR/CRIT;
 * 其余未知错误一律按CRIT记录, 便于暴露真正的程序性故障。
 */
ngx_int_t ngx_connection_error(ngx_connection_t *c, ngx_err_t err, char *text)    /* 连接错误分级记录 */
{                                  /* 函数体开始 */
    ngx_uint_t  level;             /* 最终选定的日志级别 */

    if (err == NGX_ECONNRESET      /* 错误码是对端RST复位连接 */
        && c->log_error == NGX_ERROR_IGNORE_ECONNRESET)    /* 且配置明确要求忽略RST */
    {                              /* if语句体开始 */
        return 0;                  /* 静默返回, 不记日志(如探测类连接) */
    }   /* if结束 */

    if (err == NGX_ECONNRESET      /* 连接被对端RST复位 */
#if !(WIN32)                       /* 非Windows平台才有EPIPE */
        || err == NGX_EPIPE        /* 向已收到RST/FIN的socket写入触发EPIPE */
#endif                             /* 条件编译结束 */
        || err == NGX_ENOTCONN     /* socket尚未完成连接建立 */
        || err == NGX_ECONNREFUSED    /* 对端目标端口无进程监听, 回RST拒绝 */
        || err == NGX_EHOSTUNREACH)   /* 收到ICMP主机不可达, 路由无法送达 */
    {                              /* 复合条件if语句体开始 */

        switch (c->log_error) {    /* 按连接配置的错误敏感度选择日志级别 */

        case NGX_ERROR_IGNORE_ECONNRESET:    /* 忽略RST的级别 */
        case NGX_ERROR_INFO:                 /* info级别 */
            level = NGX_LOG_INFO;  /* 都记为INFO: 客户端异常断开属常态事件 */
            break;                 /* 跳出switch */

        case NGX_ERROR_ERR:        /* err级别 */
            level = NGX_LOG_ERR;   /* 记为ERROR */
            break;                 /* 跳出switch */

        default:                   /* 更高级别配置(alert/crit等) */
            level = NGX_LOG_CRIT;  /* 记为CRIT */
        }   /* switch结束 */

    } else {                       /* 不属于TCP常态错误的未知错误 */
        level = NGX_LOG_CRIT;      /* 一律按CRIT记录, 避免漏报程序性故障 */
    }   /* if-else结束 */

    ngx_log_error(level, c->log, err, text);    /* 按选定级别输出带errno的错误日志 */

    return NGX_ERROR;              /* 向调用者报告出错(调用者通常随后关闭连接) */
}   /* ngx_connection_error结束 */
