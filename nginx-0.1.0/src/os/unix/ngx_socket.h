
/*
 * Copyright (C) Igor Sysoev
 */


#ifndef _NGX_SOCKET_H_INCLUDED_  /* 头文件保护宏：判断是否首次被包含 */
#define _NGX_SOCKET_H_INCLUDED_  /* 定义保护宏；重复包含时本文件剩余内容被预处理器忽略，避免重定义错误 */


#include <ngx_config.h>  /* 平台配置头：间接引入 sys/socket.h 等，提供 SHUT_WR、O_NONBLOCK 等常量 */


#define NGX_WRITE_SHUTDOWN SHUT_WR  /* “只关闭写方向”的 shutdown 语义：本端发 FIN 后不再写，但仍可读对端数据（TCP 半关闭） */

typedef int  ngx_socket_t;  /* Unix 下套接字就是 int 型文件描述符：进程 fd 数组的下标，内核中经 struct file 关联到 struct socket/struct sock */

#define ngx_socket(af, type, proto, flags)   socket(af, type, proto)  /* 封装 socket(2) 系统调用；flags 参数仅 Windows 需要，Unix 下直接丢弃 */
#define ngx_socket_n        "socket()"  /* 操作名描述串：系统调用失败时供 ngx_log_error 原样打印，便于定位出错点 */


#if (HAVE_FIONBIO)  /* 平台支持 ioctl(FIONBIO) 时，声明真正的函数实现（见 ngx_socket.c） */

int ngx_nonblocking(ngx_socket_t s);  /* 设为非阻塞：一条 ioctl 完成，是事件驱动模型的前提 */
int ngx_blocking(ngx_socket_t s);  /* 恢复阻塞模式 */

#define ngx_nonblocking_n   "ioctl(FIONBIO)"  /* 出错日志用的操作名 */
#define ngx_blocking_n      "ioctl(!FIONBIO)"  /* 同上：清除非阻塞标志的操作名 */

#else  /* 无 FIONBIO 支持时退化为 fcntl 宏实现 */

#define ngx_nonblocking(s)  fcntl(s, F_SETFL, O_NONBLOCK)  /* 用 fcntl 置 O_NONBLOCK；严格来说应先 F_GETFL 取旧标志再按位或，此简化写法会丢失其他文件状态标志 */
#define ngx_nonblocking_n   "fcntl(O_NONBLOCK)"  /* 出错日志用的操作名 */

#endif  /* HAVE_FIONBIO 条件编译分支结束 */

int ngx_tcp_nopush(ngx_socket_t s);  /* 开启攒包选项（TCP_NOPUSH/TCP_CORK）的跨平台接口声明 */
int ngx_tcp_push(ngx_socket_t s);  /* 解除攒包并冲刷发送缓冲区的跨平台接口声明 */

#ifdef __linux__  /* Linux 平台底层选项名是 TCP_CORK */

#define ngx_tcp_nopush_n   "setsockopt(TCP_CORK)"  /* 出错日志用的操作名 */
#define ngx_tcp_push_n     "setsockopt(!TCP_CORK)"  /* 同上：解除 cork 的操作名 */

#else  /* 其余平台（如 FreeBSD）使用 TCP_NOPUSH */

#define ngx_tcp_nopush_n   "setsockopt(TCP_NOPUSH)"  /* 出错日志用的操作名 */
#define ngx_tcp_push_n     "setsockopt(!TCP_NOPUSH)"  /* 同上：解除 nopush 的操作名 */

#endif  /* __linux__ 条件编译分支结束 */


#define ngx_shutdown_socket    shutdown  /* 直接映射到 shutdown(2)：可只关一个方向，与 close 的全关不同，且作用于套接字本身而非单个 fd */
#define ngx_shutdown_socket_n  "shutdown()"  /* 出错日志用的操作名 */

#define ngx_close_socket    close  /* 直接映射到 close(2)：递减 fd 引用计数，减到 0 才真正发起四次挥手并释放内核 sock 对象 */
#define ngx_close_socket_n  "close()"  /* 出错日志用的操作名 */


#endif /* _NGX_SOCKET_H_INCLUDED_ */  /* 与文件开头的 #ifndef 配对，头文件保护结束 */
