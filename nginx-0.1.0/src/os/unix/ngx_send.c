
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置头：提供 HAVE_KQUEUE 等探测宏 */
#include <ngx_core.h>  /* nginx 核心类型：ngx_connection_t、NGX_ERROR 等定义 */
#include <ngx_event.h>  /* 事件模块头：ngx_event_t、ngx_event_flags、NGX_AGAIN 等在此定义 */


/*
 * 整体功能：非阻塞地把 buf 中 size 字节写入连接 c（发送路径）。
 * send() 只负责把数据从用户缓冲区拷入内核发送缓冲区（sk_write_queue），
 * 返回值即内核本次接受的字节数；真正的网络发送由 TCP 层按 MSS 分段，
 * 在滑动窗口（对端接收能力）与拥塞窗口（网络承载能力）约束下逐步完成。
 * 内核发送缓冲区满时返回 -1 且 errno=EAGAIN，调用方等待可写事件后重试。
 */
ssize_t ngx_unix_send(ngx_connection_t *c, u_char *buf, size_t size)  /* c 为连接对象，buf/size 为待发送数据 */
{
    ssize_t       n;  /* send() 返回值：实际拷入内核发送缓冲区的字节数，出错为 -1 */
    ngx_err_t     err;  /* 暂存线程局部的 errno */
    ngx_event_t  *wev;  /* 写事件结构：在用户态缓存可写/出错状态 */

    wev = c->write;  /* 取出该连接的写事件指针 */

#if (HAVE_KQUEUE)  /* kqueue 平台：关闭事件可能已由 kevent 预先报告 */

    if ((ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) && wev->pending_eof) {  /* kevent 已报告写端关闭（对端关闭或 RST），再 send 注定失败 */
        ngx_log_error(NGX_LOG_INFO, c->log, wev->kq_errno,  /* INFO 级记录，避免一次注定 ECONNRESET 的系统调用 */
                      "kevent() reported about an closed connection");  /* 原有英文日志串 */

        wev->error = 1;  /* 写事件标记出错 */
        return NGX_ERROR;  /* 不再调用 send，直接向调用方报错 */
    }  /* if(pending_eof) 分支结束 */

#endif  /* HAVE_KQUEUE 条件编译分支结束 */

    for ( ;; ) {  /* 无限循环：只有 EINTR 场景需要再次调用 send，其余分支一律 return */
        n = send(c->fd, buf, size, 0);  /* 核心：把用户数据拷入内核发送缓冲区，之后由 TCP 按 MSS 分段并受双窗口约束发出 */

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志：fd、内核接受字节数、请求字节数 */
                       "send: fd:%d %d of %d", c->fd, n, size);  /* 原有日志格式串，便于观察部分发送情况 */

        if (n > 0) {  /* 至少送出去了一部分（非阻塞下部分发送是常态） */
            if (n < (ssize_t) size) {  /* 未被全部接受：内核发送缓冲区剩余空间不足 */
                wev->ready = 0;  /* 清除可写标志：剩余部分要等可写事件到来再续发 */
            }  /* if(n < size) 分支结束 */

            return n;  /* 返回实际拷入字节数，调用方据此推进 buf 指针与剩余长度 */
        }  /* if(n > 0) 分支结束 */

        err = ngx_socket_errno;  /* 走到这里 n<=0：立即取出 errno */

        if (n == 0) {  /* send 返回 0：TCP 语义下（size>0 时）几乎不可能出现 */
            ngx_log_error(NGX_LOG_ALERT, c->log, err, "send() returned zero");  /* ALERT 级告警：提示内核行为异常，需要人工关注 */
            wev->ready = 0;  /* 清除可写标志 */
            return n;  /* 把 0 返回给调用方处理 */
        }  /* if(n == 0) 分支结束 */

        if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* EAGAIN：内核发送缓冲区已满（对端接收窗口小或网络慢）；EINTR：被信号打断 */
            wev->ready = 0;  /* 暂时不可写：清除就绪标志，等 epoll/kqueue 通知可写后再发 */

            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试级记录“暂不能发”，属正常流控而非错误 */
                           "send() not ready");  /* 原有日志串 */

            if (err == NGX_EAGAIN) {  /* 缓冲区满：非阻塞语义下应整批重试 */
                return NGX_AGAIN;  /* 返回“稍后再发”，进程不睡眠、不占 CPU */
            }  /* 是 EINTR 则不返回，落到循环开头重试 send */

        } else {  /* 其余 errno 是真实错误：如 EPIPE（本端已关写）、ECONNRESET（对端 RST） */
            wev->error = 1;  /* 写事件标记出错，上层一般会关闭连接 */
            ngx_connection_error(c, err, "recv() failed");  /* 记录错误日志（原代码日志串沿用了 "recv() failed"，保持原样不改） */
            return NGX_ERROR;  /* 向调用方上抛错误 */
        }  /* if-else 判断结束 */
    }  /* for(;;) 循环体结束 */
}  /* ngx_unix_send 函数结束 */
