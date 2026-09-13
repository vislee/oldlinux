
/*
 * Copyright (C) Igor Sysoev
 */


#ifndef _NGX_CONNECTION_H_INCLUDED_   /* 头文件保护宏开始：防止本头文件被重复包含而引发类型重定义编译错误 */
#define _NGX_CONNECTION_H_INCLUDED_   /* 首次包含时定义保护宏，供上面的 #ifndef 在后续包含时短路 */


#include <ngx_config.h>   /* 编译期平台配置头：引入 configure 探测出的 OS/编译器特性宏(如 HAVE_EPOLL) */
#include <ngx_core.h>   /* nginx 核心头：引入 ngx_str_t/ngx_pool_t/ngx_log_t 等基础类型定义 */


/*
 * ngx_listening_t —— 监听 socket 的用户态镜像。
 * nginx 启动时为每条 listen 指令创建一个该对象：fd 指向内核中 LISTEN 状态的
 * 套接字，backlog 即内核全连接队列(accept 队列)长度；完成三次握手的连接进入
 * 该队列后由事件框架通知 handler 完成 accept。所有监听对象挂入
 * ngx_cycle_t.listening 数组，并在多 worker 进程间共享(见惊群问题)。
 */
typedef struct {   /* 监听对象类型定义开始 */
    ngx_socket_t      fd;   /* 监听套接字描述符：内核 LISTEN 状态 socket 在进程 fd 表中的句柄 */

    struct sockaddr  *sockaddr;   /* 通用地址结构：本监听的 IP:端口，bind() 的实参，也是虚拟主机匹配依据 */
    socklen_t         socklen;    /* size of sockaddr */   /* sockaddr 的字节长度：bind()/getsockname() 的长度参数，内核据此安全拷贝 */
    int               addr;       /* offset to address in sockaddr */   /* IP 地址字段在 sockaddr 内的偏移：IPv4/IPv6 布局不同，据此统一取址 */
    int               addr_text_max_len;   /* 地址文本最大长度：IPv4 最长 "255.255.255.255" 共 15 字符，预留防溢出 */
    ngx_str_t         addr_text;   /* 点分十进制地址文本：日志与 HTTP 处理直接使用，免去运行期反复转换 */

    int               family;   /* 协议族：AF_INET/AF_INET6，决定内核网络层的地址解析规则 */
    int               type;   /* 套接字类型：SOCK_STREAM(TCP 可靠字节流)或 SOCK_DGRAM(UDP 数据报) */
    int               protocol;   /* 协议号：socket() 第 3 参数，TCP 通常取 IPPROTO_TCP(或 0 由内核推断) */
    int               flags;      /* Winsock2 flags */   /* Winsock2 建套接字标志(仅 Windows 有意义) */

    /* accept 回调函数：新连接完成三次握手进入内核全连接队列、事件循环
       收到监听读事件通知后，由它把连接交给上层协议模块(http/mail)处理 */
    void            (*handler)(ngx_connection_t *c); /* handler of accepted
                                                        connection */
    void             *ctx;        /* ngx_http_conf_ctx_t, for example */   /* 模块配置上下文：新连接据此找到协议级配置(如 http{} 的 conf_ctx) */
    void             *servers;    /* array of ngx_http_in_addr_t, for example */   /* 地址上的服务器数组：如 ngx_http_in_addr_t，供端口复用与默认 server 查找 */

    ngx_log_t        *log;   /* 监听 socket 专用日志对象：bind/listen/accept 阶段的错误记录于此 */
    int               backlog;   /* listen() 第 2 参数：全连接队列长度；过小则突发连接被内核丢弃或拒绝 */

    size_t            pool_size;   /* 为每条新连接预分配的内存池大小：减少 malloc 次数与内存碎片 */
    /* accept 后的预读缓冲大小；因 Windows AcceptEx() 在投递时就要指定
       预读缓冲，该参数必须挂在监听对象上 */
    size_t            post_accept_buffer_size; /* should be here because
                                                  of the AcceptEx() preread */
    /* accept 后连接的首包超时；因 TCP_DEFER_ACCEPT/AcceptEx 会延迟建连，
       该超时同样必须随监听对象携带 */
    time_t            post_accept_timeout;     /* should be here because
                                                  of the deferred accept */

    unsigned          new:1;   /* 位域：新创建、尚未使用过的监听对象标志 */
    unsigned          remain:1;   /* 打开监听失败时保留对象以便重试(热升级场景不丢失监听) */
    unsigned          ignore:1;   /* 打开失败已被忽略(如配置测试时)，不升级为致命错误 */

    unsigned          bound:1;       /* already bound */   /* 已完成 bind() 地址绑定 */
    unsigned          inherited:1;   /* inherited from previous process */   /* fd 由旧进程经环境变量继承(平滑升级)：只接管不重新 bind，避免端口冲突 */
    unsigned          nonblocking_accept:1;   /* 监听 socket 已设 O_NONBLOCK：无连接可 accept 时立即返回 EAGAIN，不阻塞 worker */
    unsigned          nonblocking:1;   /* accept 得到的连接 socket 设为非阻塞 I/O：事件驱动模型的前提 */
#if 0   /* 条件编译开关(当前整块停用) */
    unsigned          overlapped:1;  /* Winsock2 overlapped */   /* (停用)Winsock2 重叠 I/O 标志 */
#endif   /* #if 0 块结束 */
    unsigned          shared:1;    /* shared between threads or processes */   /* 该 socket 被多进程/线程共享(所有 worker 继承同一监听 fd)，由此引出惊群问题 */
#if (HAVE_DEFERRED_ACCEPT)   /* 条件编译：平台支持 TCP_DEFER_ACCEPT(Linux) 时才编译 */
    unsigned          deferred_accept:1;   /* 启用 TCP_DEFER_ACCEPT：握手完成但一直无数据的连接，内核不放入 accept 队列 */
#endif   /* 条件编译结束 */

    unsigned          addr_ntop:1;   /* 已把二进制地址转成文本(ngx_inet_ntop)，供日志与 HTTP 使用 */
} ngx_listening_t;   /* 监听对象类型名 ngx_listening_t */


/*
 * 连接错误日志级别枚举：按连接把特定错误升/降级记录，
 * 抑制客户端异常断连(如 RST)造成的日志噪音。
 */
typedef enum {   /* 连接错误日志级别枚举定义 */
     NGX_ERROR_CRIT = 0,   /* crit：严重错误(记录并通常关闭连接)，如 accept 系统调用失败 */
     NGX_ERROR_ERR,   /* err：普通错误级别 */
     NGX_ERROR_INFO,   /* info：提示级别，用于可预期的轻微错误 */
     NGX_ERROR_IGNORE_ECONNRESET   /* 特殊标记：对端 RST(ECONNRESET) 不按错误记录——客户端中途断开属 TCP 常态 */
} ngx_connection_log_error_e;   /* 枚举类型名 ngx_connection_log_error_e */


/*
 * TCP_NOPUSH(TCP_CORK) 三态枚举：sendfile 发送静态文件时先 cork 住 socket，
 * 让响应头与文件数据聚合成完整报文再发出，减少网络中的碎小包(MSS 浪费)。
 */
typedef enum {   /* TCP nopush 三态枚举定义 */
     NGX_TCP_NOPUSH_DISABLED = -1,   /* 已禁用：显式清除 TCP_CORK/TCP_NOPUSH */
     NGX_TCP_NOPUSH_UNSET = 0,   /* 未设置：保持内核默认行为 */
     NGX_TCP_NOPUSH_SET   /* 已设置：内核推迟小段数据发送，攒满一个 MSS 或解除 cork 时才发出 */
} ngx_connection_tcp_nopush_e;   /* 枚举类型名 ngx_connection_tcp_nopush_e */


/*
 * ngx_connection_t —— 连接对象：一条 TCP 连接(或任意 fd)在用户态的完整状态。
 * Linux "一切皆文件"，socket 由内核 fd 表统一管理；本结构把 fd、读/写事件、
 * 收发函数指针、内存池、日志与对端地址组织在一起，是事件驱动框架调度 I/O
 * 的基本操作单元。
 */
struct ngx_connection_s {   /* 连接结构定义(以 struct 声明，便于各头文件前向引用) */
    void               *data;   /* 协议层上下文：http 模块把 ngx_http_request_t 挂在此处，由连接找回请求 */
    ngx_event_t        *read;   /* 读事件：fd 可读(数据到达/FIN/监听队列非空)时由内核经就绪队列通知 */
    ngx_event_t        *write;   /* 写事件：发送缓冲区有空位时触发；写满返回 EAGAIN 后靠它续传，避免忙等 */

    ngx_socket_t        fd;   /* 连接的文件描述符：accept() 的返回值，内核 fd 表项的用户态索引 */

    ngx_recv_pt         recv;   /* 收包函数指针：由具体 I/O 机制(epoll/aio/IOCP)注入，实现 I/O 层多态 */
    ngx_send_chain_pt   send_chain;   /* 聚集写函数指针：一次系统调用写出多块缓冲(writev 语义)，减少 syscall 与上下文切换 */

    ngx_listening_t    *listening;   /* 反向指向产生本连接的监听对象：获知协议族、日志与处理约定 */

    off_t               sent;   /* 本连接累计发送字节数：流量统计与限速模块的数据来源 */

    void               *ctx;   /* 协议模块的连接级上下文(与监听对象上的 ctx 同构) */
    void               *servers;   /* 协议模块的服务器数组指针(与监听对象上的 servers 同构) */


    ngx_log_t          *log;   /* 连接日志：叠加客户端地址信息，便于定位"哪条连接、哪个客户端出错" */

    ngx_pool_t         *pool;   /* 连接私有内存池：生命周期内的小块分配走这里，关闭时整池回收，杜绝泄漏 */

    struct sockaddr    *sockaddr;   /* 对端(客户端)地址：accept() 时由内核填入，访问日志 $remote_addr 的来源 */
    socklen_t           socklen;   /* 对端地址结构长度 */
    ngx_str_t           addr_text;   /* 对端地址的文本形式：避免每次记日志都做二进制到文本的转换 */

#if (NGX_OPENSSL)   /* 条件编译：启用 OpenSSL(https) 时才编译 */
    ngx_ssl_t          *ssl;   /* SSL/TLS 会话句柄：https 连接的收发须经 OpenSSL 加解密层 */
#endif   /* 条件编译结束 */

#if (HAVE_IOCP)   /* 条件编译：Windows IOCP 平台 */
    struct sockaddr    *local_sockaddr;   /* 本端地址：AcceptEx 一次返回本地与对端地址，免去 getsockname() */
    socklen_t           local_socklen;   /* 本端地址长度 */
#endif   /* 条件编译结束 */

    ngx_buf_t          *buffer;   /* 连接级输入缓冲：暂存已从内核读出但协议尚未消费的字节(半包场景) */

    ngx_uint_t          number;   /* 连接序号：进程内单调递增，日志中唯一标识一条连接 */

    unsigned            log_error:2;  /* ngx_connection_log_error_e */   /* 该连接的错误日志级别：取值即上面的 ngx_connection_log_error_e 枚举 */

    unsigned            buffered:1;   /* 连接上挂有待延迟写出的数据(限速/缓冲响应)，发送侧不能关闭 */
    unsigned            single_connection:1;   /* 单连接工作模式(调试/特殊进程)，走精简处理路径 */
    unsigned            unexpected_eof:1;   /* 协议请求未完整却读到 EOF：客户端异常断连的标记 */
    unsigned            timedout:1;   /* 连接读/写定时器已超时(防御 slowloris 类慢速攻击) */
    signed              tcp_nopush:2;   /* TCP nopush 三态(取值见上枚举)：sendfile 前后动态 cork/uncork 优化报文 */
#if (HAVE_IOCP)   /* 条件编译：Windows IOCP 平台 */
    unsigned            accept_context_updated:1;   /* 已对 AcceptEx 建立的 socket 调用过 setsockopt(SO_UPDATE_ACCEPT_CONTEXT) */
#endif   /* 条件编译结束 */

#if (NGX_THREADS)   /* 条件编译：多线程模式 */
    ngx_atomic_t        lock;   /* 保护本连接的原子锁：防止多线程同时操作同一连接状态 */
#endif   /* 条件编译结束 */
};   /* ngx_connection_s 结构定义结束 */


#ifndef ngx_ssl_set_nosendshut   /* 若外部(OpenSSL 相关头)未定义 ngx_ssl_set_nosendshut 宏… */
#define ngx_ssl_set_nosendshut(ssl)   /* …则给空实现：非 SSL 编译时调用点无需条件编译保护 */
#endif   /* 条件编译结束 */


/* 工具函数：创建并初始化一个 IPv4 TCP 流式监听对象(ngx_listening_t) */
ngx_listening_t *ngx_listening_inet_stream_socket(ngx_conf_t *cf,
                                                 in_addr_t addr,   /* 监听的 IPv4 地址(网络字节序)，INADDR_ANY 表示所有网卡 */
                                                 in_port_t port);   /* 监听的 TCP 端口(网络字节序) */
ngx_int_t ngx_set_inherited_sockets(ngx_cycle_t *cycle);   /* 平滑升级时从环境变量恢复父进程传来的监听 fd：只接管，不重新 bind/listen */
ngx_int_t ngx_open_listening_sockets(ngx_cycle_t *cycle);   /* 对全部监听对象执行 socket→setsockopt(SO_REUSEADDR)→bind→listen，失败则启动中止 */
void ngx_close_listening_sockets(ngx_cycle_t *cycle);   /* 关闭全部监听 fd 并从事件表摘除：worker 退出或热升级交接时调用 */
void ngx_close_connection(ngx_connection_t *c);   /* 关闭单条连接：注销读写事件、释放缓冲与内存池、close(fd)，防止资源泄漏 */
/* 连接错误统一记录入口：按连接的 log_error 级别决定记录档位(ECONNRESET 等可降级) */
ngx_int_t ngx_connection_error(ngx_connection_t *c, ngx_err_t err, char *text);


extern ngx_os_io_t  ngx_io;   /* 全局 OS I/O 函数表：平台相关代码填充，连接的收发函数指针由此初始化 */


#endif /* _NGX_CONNECTION_H_INCLUDED_ */   /* 头文件保护宏结束：_NGX_CONNECTION_H_INCLUDED_ */
