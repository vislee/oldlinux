
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 编译期自动生成的平台配置头：提供 HAVE_FIONBIO 等探测宏与系统类型 */
#include <ngx_core.h>  /* nginx 核心头：ngx_socket_t、NGX_OK 等基础类型与常量在此声明 */


/*
 * ioctl(FIONBIO) sets a blocking mode with the single syscall
 * while fcntl(F_SETFL, ~O_NONBLOCK) needs to learn before
 * the previous state using fcntl(F_GETFL).
 *
 * ioctl() and fcntl() are syscalls on at least FreeBSD 2.x, Linux 2.2
 * and Solaris 7.
 *
 * ioctl() in Linux 2.4 and 2.6 uses BKL, however fcntl(F_SETFL) uses it too.
 */
/*
 * 中文大意：ioctl(FIONBIO) 用一条系统调用即可直接设置阻塞/非阻塞模式；
 * 而 fcntl(F_SETFL, ~O_NONBLOCK) 必须先用 fcntl(F_GETFL) 读出旧标志再改位，
 * 多一次陷入内核的开销。至少在 FreeBSD 2.x、Linux 2.2、Solaris 7 上
 * ioctl() 与 fcntl() 都是系统调用；Linux 2.4/2.6 中 ioctl() 要持有大内核锁
 * BKL，fcntl(F_SETFL) 同样要拿它，两者开销相当。
 */


#if (HAVE_FIONBIO)  /* 编译期探测到平台支持 ioctl(FIONBIO) 时才编译这一组实现 */

/*
 * 整体功能：把套接字 s 切换为非阻塞 I/O。此后 recv/send 在内核缓冲区
 * 不可读/不可写时立即返回 -1 且 errno=EAGAIN(EWOULDBLOCK)，进程不进入
 * 内核等待队列睡眠、不占用 CPU，何时就绪由事件模块（epoll/kqueue）通知，
 * 这是 nginx 单进程支撑大量并发连接的基础。
 */
int ngx_nonblocking(ngx_socket_t s)  /* s 是 int 型文件描述符，在内核中关联 struct socket/struct sock 对象 */
{
    u_long  nb;  /* ioctl(FIONBIO) 约定经指针传入一个整型开关值，非 0 表示“开” */

    nb = 1;  /* 请求内核把该 fd 的文件状态标志 f_flags 置上 O_NONBLOCK 位 */

    return ioctl(s, FIONBIO, &nb);  /* 一条系统调用直达内核改标志，无需先 F_GETFL，比 fcntl 方式省一次陷入 */
}  /* ngx_nonblocking 结束 */


/*
 * 整体功能：把套接字恢复为阻塞模式。此后 recv/send 在条件不满足时会使
 * 进程睡眠在套接字的等待队列上，直到数据到达或发送缓冲区腾空时被内核唤醒。
 */
int ngx_blocking(ngx_socket_t s)  /* 恢复阻塞：此后条件不满足时进程将睡眠在内核等待队列上 */
{
    u_long  nb;  /* 与置非阻塞共用的开关参数缓冲区 */

    nb = 0;  /* 请求内核清除文件标志中的 O_NONBLOCK 位 */

    return ioctl(s, FIONBIO, &nb);  /* 仍是单条系统调用，与开启非阻塞共用同一 ioctl 命令字 */
}  /* ngx_blocking 结束 */

#endif  /* HAVE_FIONBIO 条件编译分支结束 */


#ifdef __FreeBSD__  /* FreeBSD 平台：用 TCP_NOPUSH 选项实现“攒够再发” */

/*
 * 整体功能：给套接字塞上 TCP_NOPUSH（与 Linux 的 TCP_CORK 对应）。
 * 内核 TCP 栈据此抑制不满一个 MSS 的小段发送，等数据攒齐或解除塞子时
 * 一次性发出，减少网络中的小包数量与协议头开销，常与 writev/sendfile 配合。
 */
int ngx_tcp_nopush(ngx_socket_t s)  /* 开启 FreeBSD 的 TCP_NOPUSH：抑制不满一个 MSS 的小段 */
{
    int  tcp_nopush;  /* setsockopt 的选项值缓冲区（optval），套接字选项均为整数语义 */

    tcp_nopush = 1;  /* 1 表示开启该选项 */

    return setsockopt(s, IPPROTO_TCP, TCP_NOPUSH,  /* 在 IPPROTO_TCP 协议层（而非 SOL_SOCKET 层）设置选项 */
                      (const void *) &tcp_nopush, sizeof(int));  /* optval 指向选项值，optlen 声明其长度 */
}  /* ngx_tcp_nopush(FreeBSD) 结束 */


/*
 * 整体功能：解除 TCP_NOPUSH（拔塞），内核立即把发送缓冲区中积攒的数据
 * 构造报文冲刷出去，相当于一次显式 flush，保证响应尾包不被滞留。
 */
int ngx_tcp_push(ngx_socket_t s)  /* 解除 TCP_NOPUSH：内核立即冲刷积压数据 */
{
    int  tcp_nopush;  /* 选项值缓冲区 */

    tcp_nopush = 0;  /* 0 表示关闭该选项，解除对小段的抑制 */

    return setsockopt(s, IPPROTO_TCP, TCP_NOPUSH,  /* 与开启共用同一选项字 */
                      (const void *) &tcp_nopush, sizeof(int));  /* 关闭瞬间内核触发冲刷，积压数据立刻上网 */
}  /* ngx_tcp_push(FreeBSD) 结束 */

#elif __linux__  /* Linux 平台：对应机制叫 TCP_CORK（“软木塞”） */

/*
 * 整体功能：给套接字塞上 TCP_CORK。开启后内核强制攒包：只有攒满一个 MSS
 * 或解除 cork 才发送（另有 200ms 兜底定时器），把响应头+正文合并成完整报文，
 * 避免琐碎小包拖慢慢启动阶段的拥塞窗口增长并浪费带宽。
 */
int ngx_tcp_nopush(ngx_socket_t s)  /* 开启 Linux 的 TCP_CORK：强制攒包，攒满一个 MSS 才发 */
{
    int  cork;  /* 选项值缓冲区 */

    cork = 1;  /* 1：在 tcp_sock 结构上置上塞子标志 */

    return setsockopt(s, IPPROTO_TCP, TCP_CORK,  /* TCP_CORK 是 Linux 特有选项，语义对应 BSD 的 TCP_NOPUSH */
                      (const void *) &cork, sizeof(int));  /* 内核在 do_tcp_setsockopt 中解析该选项并生效 */
}  /* ngx_tcp_nopush(Linux) 结束 */

/*
 * 整体功能：解除 TCP_CORK，内核立刻冲刷发送缓冲区，把攒下的数据一次性发出
 * ——这正是 ngx_tcp_push 在 Linux 上的真实动作。
 */
int ngx_tcp_push(ngx_socket_t s)  /* 解除 TCP_CORK：立即冲刷发送缓冲区 */
{
    int  cork;  /* 选项值缓冲区 */

    cork = 0;  /* 0：拔掉塞子，解除攒包状态 */

    return setsockopt(s, IPPROTO_TCP, TCP_CORK,  /* 清除选项走同一系统调用入口 */
                      (const void *) &cork, sizeof(int));  /* 解除时立即冲刷，不受 Nagle 算法与定时器约束 */
}  /* ngx_tcp_push(Linux) 结束 */

#else  /* 其余平台：没有 nopush/cork 机制，提供空实现 */

/*
 * 整体功能：不支持攒包选项平台上的占位实现，恒返回成功，
 * 让上层调用代码无需针对平台写额外分支。
 */
int ngx_tcp_nopush(ngx_socket_t s)  /* 无攒包选项平台的空实现 */
{
    return NGX_OK;  /* 直接返回成功，不做任何系统调用 */
}  /* ngx_tcp_nopush(空实现) 结束 */

/*
 * 整体功能：同上，push 操作也是空实现。
 */
int ngx_tcp_push(ngx_socket_t s)  /* 同上：push 也是空实现 */
{
    return NGX_OK;  /* 直接返回成功 */
}  /* ngx_tcp_push(空实现) 结束 */

#endif  /* 平台条件编译结束 */
