
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台特性宏（IOV_MAX、NGX_EAGAIN 等） */
#include <ngx_core.h>  /* nginx 核心基础设施：内存池、ngx_array_t、buf/chain */
#include <ngx_event.h>  /* 事件框架：连接、写事件与事件模块标志 */


#define NGX_IOVS  8  /* iovec 栈上预分配数量：常见响应头+正文场景一次够用，避免热路径堆分配 */


/* 整体说明：ngx_writev_chain 用 writev()（集中写，gather write）把 buf 链上多个不连续
 * 内存块拼成 struct iovec 数组，一次系统调用全部拷入内核套接字发送缓冲区——典型场景是
 * 把 HTTP 响应头与响应体一次性发出，省去多次 write() 的用户态/内核态切换开销。
 * writev 的返回值是实际拷入 sndbuf 的字节数：受发送缓冲区容量与对端接收窗口(rwnd)、
 * 拥塞窗口限制，可能只发出一部分（部分写），剩余 buf 经返回值交还调用方，
 * 待 EPOLLOUT/写事件就绪后续发。EAGAIN=缓冲区满；EINTR=信号打断需重试；
 * EPIPE/ECONNRESET=对端已关闭 */
ngx_chain_t *ngx_writev_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)  /* 返回剩余未发送的链; limit:本次最多发送字节数 */
{  /* 函数体开始 */
    u_char        *prev;  /* 上一buf数据起始地址，用于合并物理连续的内存块 */
    ssize_t        n, size;  /* n:writev返回值; size:当前buf待发字节数 */
    off_t          send, sprev, sent;  /* send:本轮累计请求量; sprev:系统调用前的快照; sent:单次实发字节 */
    ngx_uint_t     eintr, complete;  /* eintr:被信号打断; complete:本轮请求是否全部发完 */
    ngx_err_t      err;  /* 保存 errno */
    ngx_array_t    vec;  /* iovec 数组的 ngx_array_t 封装 */
    ngx_chain_t   *cl;  /* buf 链遍历游标 */
    ngx_event_t   *wev;  /* 连接的写事件对象 */
    struct iovec  *iov, iovs[NGX_IOVS];  /* 当前向量项 + 栈上预分配的8个iovec（常见路径零堆分配） */

    wev = c->write;  /* 取出写事件对象 */

    if (!wev->ready) {  /* 写事件未就绪：内核发送缓冲区仍满，EPOLLOUT 尚未到来 */
        return in;  /* 原样退回整条链，等下次写事件再发 */
    }  /* 就绪检查结束 */

#if (HAVE_KQUEUE)  /* 条件编译：kqueue 能提前探测对端关闭 */

    if ((ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) && wev->pending_eof) {  /* kqueue 已报告写方向EOF（对端已关闭） */
        ngx_log_error(NGX_LOG_INFO, c->log, wev->kq_errno,  /* INFO 级日志，附 kq_errno */
                      "kevent() reported about an closed connection");  /* 原日志串：kevent 报告连接已关闭 */

        wev->error = 1;  /* 标记写错误：提前拦截，避免后续 writev 撞上 EPIPE/SIGPIPE */
        return NGX_CHAIN_ERROR;  /* 返回错误标记 */
    }  /* 对端关闭预检结束 */

#endif  /* kqueue 预检结束 */

    send = 0;  /* 累计请求发送量清零 */
    complete = 0;  /* 完成标志清零 */

    vec.elts = iovs;  /* 数组直接使用栈上空间 */
    vec.size = sizeof(struct iovec);  /* 元素大小 */
    vec.nalloc = NGX_IOVS;  /* 容量为8 */
    vec.pool = c->pool;  /* 超过8项时向连接内存池扩容 */

    for ( ;; ) {  /* 主循环：构造向量→writev→按实发量消费buf→必要时再来一轮 */
        prev = NULL;  /* 重置连续性基址 */
        iov = NULL;  /* 重置当前向量项 */
        eintr = 0;  /* 重置EINTR标志 */
        sprev = send;  /* 记录本轮系统调用前的累计值，用于判断"是否全部发出" */

        vec.nelts = 0;  /* 元素个数清零（复用已分配空间） */

        /* 中文：合并相邻buf——内存首尾相接的buf共用同一iovec，减少向量条目与内核拷贝循环的开销 */
        /* create the iovec and coalesce the neighbouring bufs */

        for (cl = in; cl && vec.nelts < IOV_MAX && send < limit; cl = cl->next)  /* 遍历链：受 IOV_MAX(内核单次向量数上限) 与流量限额双重约束 */
        {  /* 循环体开始 */
            if (ngx_buf_special(cl->buf)) {  /* 特殊buf（同步/刷新等标记）不含用户数据 */
                continue;  /* 跳过，不参与本次 writev */
            }  /* 特殊buf过滤结束 */

            size = cl->buf->last - cl->buf->pos;  /* 该buf待发字节数（pos..last 为未发送区间） */

            if (send + size > limit) {  /* 加入它将突破本次流量限额 */
                size = limit - send;  /* 截取限额内的部分（limit 兼作限速与防巨包手段） */
            }  /* 限额截断结束 */

            if (prev == cl->buf->pos) {  /* 与上一buf内存连续：合并进同一个 iovec */
                iov->iov_len += size;  /* 只需扩长前一项 */

            } else {  /* 不连续：新增一个 iovec 项 */
                if (!(iov = ngx_array_push(&vec))) {  /* 追加一项（栈上8个用尽后自动从内存池扩容） */
                    return NGX_CHAIN_ERROR;  /* 扩容失败则放弃 */
                }  /* 追加判断结束 */

                iov->iov_base = (void *) cl->buf->pos;  /* 向量基址=待发数据起点 */
                iov->iov_len = size;  /* 向量长度 */
            }  /* 向量项构造结束 */

            prev = cl->buf->pos + size;  /* 记录本buf已取部分的末址，供下轮连续性判断 */
            send += size;  /* 累计请求量 */
        }  /* iovec 构造循环结束 */

        n = writev(c->fd, vec.elts, vec.nelts);  /* 集中写系统调用：内核把各向量逐项拷入套接字发送缓冲区(sndbuf)，随后由TCP按MSS分段发出 */

        if (n == -1) {  /* 系统调用失败 */
            err = ngx_errno;  /* 取出 errno */

            if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* 两类暂时性失败 */
                if (err == NGX_EINTR) {  /* 被信号打断（一个字节都没拷走） */
                    eintr = 1;  /* 置标志：本轮重试 */
                }  /* EINTR 判断结束 */

                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试日志 */
                               "writev() not ready");  /* EAGAIN=发送缓冲区已满（对端接收窗口耗尽/拥塞控制收紧），需等 EPOLLOUT */

            } else {  /* EPIPE(对端已关闭)/ECONNRESET(收到RST)等真实错误 */
                wev->error = 1;  /* 标记连接写错误 */
                ngx_connection_error(c, err, "writev() failed");  /* 记录错误日志 */
                return NGX_CHAIN_ERROR;  /* 放弃本次发送 */
            }  /* 错误分类结束 */
        }  /* 失败处理结束 */

        sent = n > 0 ? n : 0;  /* 实发字节数（-1/0 归零）；部分写时 sent 小于本轮请求量 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志 */
                       "writev: " OFF_T_FMT, sent);  /* 打印本次实发字节 */

        if (send - sprev == sent) {  /* 本轮请求量==实发量：全部发完 */
            complete = 1;  /* 置完成标志 */
        }  /* 完成判断结束 */

        c->sent += sent;  /* 累加到连接级已发送字节计数（日志与统计用） */

        for (cl = in; cl && sent > 0; cl = cl->next) {  /* 按 sent 消费buf链：已发完的buf置空，部分发送的前移pos */
            if (ngx_buf_special(cl->buf)) {  /* 特殊buf不占字节数 */
                continue;  /* 跳过 */
            }  /* 过滤结束 */

            if (sent == 0) {  /* 已无可消费字节（防御性判断） */
                break;  /* 退出 */
            }  /* 防御判断结束 */

            size = cl->buf->last - cl->buf->pos;  /* 当前buf剩余待发字节数 */

            if (sent >= size) {  /* 整个buf都已发出 */
                sent -= size;  /* 扣减剩余实发量 */
                cl->buf->pos = cl->buf->last;  /* pos推到last：buf变空，可被释放或复用 */

                continue;  /* 继续消费下一个buf */
            }  /* 整buf消费结束 */

            cl->buf->pos += sent;  /* 部分发送：pos前移sent字节，剩余数据原地保留 */

            break;  /* cl 指向这个未发完的buf节点，将作为返回值 */
        }  /* buf消费循环结束 */

        if (eintr) {  /* 被信号打断：无错误、数据也未发出 */
            continue;  /* 重试本轮（重新构造iovec后再writev） */
        }  /* EINTR 处理结束 */

        if (!complete) {  /* 部分写：发送缓冲区已被填满 */
            wev->ready = 0;  /* 清除写就绪：事件模块注册 EPOLLOUT，待 sndbuf 有空位再通知 */
            return cl;  /* 返回剩余链（cl 指向第一个未发完的buf），等下次写事件续发 */
        }  /* 部分写处理结束 */

        if (send >= limit || cl == NULL) {  /* 已达流量限额 或 整条链已发完 */
            return cl;  /* 返回（cl==NULL 表示全部发完） */
        }  /* 限额/完成判断结束 */

        in = cl;  /* 仍有数据且未达限额：套接字缓冲区尚有空间，从断点立刻再发一轮 */
    }  /* 主循环结束 */
}  /* ngx_writev_chain 结束 */
