
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 编译期配置头：平台探测生成的特性宏（如 HAVE_KQUEUE、IOV_MAX） */
#include <ngx_core.h>  /* nginx 核心头：内存池、ngx_array_t、ngx_err_t 等基础封装 */
#include <ngx_event.h>  /* 事件框架头：ngx_connection_t/ngx_event_t 与事件模块标志 ngx_event_flags */


/* 中文说明：kqueue（BSD 系统的事件机制）在读事件上会直接携带内核探测结果——可读字节数
 * (available)、对端是否已关闭(pending_eof)与错误码(kq_errno)，因此本版本无需发起系统调用
 * 即可预判 EOF 与"暂无数据"；不具备该能力的平台使用文件尾部的通用实现 */
#if (HAVE_KQUEUE)  /* 条件编译开始：kqueue 扩展版 */

/* 整体说明：ngx_readv_chain 用 readv() 实现"分散读"(scatter read)：一次系统调用把套接字
 * 接收缓冲区中的数据拷入 buf 链描述的多个不连续用户内存块。struct iovec 数组
 * (iov_base=缓冲区地址, iov_len=长度)就是交给内核的"搬运清单"；相比逐块 read()，
 * 每合并一块内存就少一次用户态/内核态切换（一次陷入约耗上千个时钟周期），
 * 减少 syscall 次数正是 nginx 高吞吐的关键设计之一 */
ssize_t ngx_readv_chain(ngx_connection_t *c, ngx_chain_t *chain)  /* c:当前连接; chain:待填充的内存buf链 */
{  /* 函数体开始 */
    u_char        *prev;  /* 上一buf的末尾地址，用于判断相邻buf内存是否物理连续 */
    ssize_t        n, size;  /* n:readv 实际拷贝的字节数; size:本次请求读取的总字节数 */
    ngx_err_t      err;  /* 保存系统调用失败时的 errno */
    ngx_array_t    io;  /* iovec 动态数组：拼装好的"分散读"向量表 */
    ngx_event_t   *rev;  /* 连接的读事件对象，kqueue 在此记录 eof/available 状态 */
    struct iovec  *iov;  /* 当前正在填充的向量项：iov_base=用户缓冲区首址, iov_len=长度 */

    rev = c->read; /* 取出该连接的读事件对象 */

    if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {  /* 事件模块支持 kqueue 扩展事件时 */
        /* 调试日志：打印 kevent() 预先探测到的三个读状态 */
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试级日志宏，仅在 NGX_DEBUG 编译时生效 */
                       "readv: eof:%d, avail:%d, err:%d",  /* 格式串，对应下面三个实参 */
                       rev->pending_eof, rev->available, rev->kq_errno);  /* 挂起EOF标志/可读剩余字节/kqueue错误码 */

        if (rev->available == 0) {  /* available==0：内核接收缓冲区已无数据可读 */
            if (rev->pending_eof) {  /* kevent 已报告对端发来 FIN（挂起的EOF） */
                rev->ready = 0;  /* 清除读就绪标志：事件模块不再调度本次读 */
                rev->eof = 1;  /* 置EOF：本端已收到 TCP FIN 报文（对端 close/shutdown 触发的挥手） */

                ngx_log_error(NGX_LOG_INFO, c->log, rev->kq_errno,  /* INFO 级日志，附 kq_errno */
                              "kevent() reported about an closed connection");  /* 原日志串：kevent 报告连接已关闭 */

                if (rev->kq_errno) {  /* kqueue 附带非零错误码（典型 ECONNRESET：收到 RST 复位而非正常 FIN） */
                    rev->error = 1;  /* 标记连接进入错误状态 */
                    ngx_set_socket_errno(rev->kq_errno);  /* 把 kq_errno 恢复到线程 errno，供错误日志格式化 */
                    return NGX_ERROR;  /* 返回错误，上层将终止该连接上的请求处理 */
                }  /* kq_errno 判断结束 */

                return 0;  /* 返回0字节：语义同 read() 返回0，即对端读方向已正常关闭 */

            } else {  /* 无挂起EOF，只是暂时没有数据 */
                return NGX_AGAIN;  /* 省掉一次必然返回 EAGAIN 的 readv 系统调用 */
            }  /* else 分支结束 */
        }  /* available==0 分支结束 */
    }  /* kqueue 预检结束 */

    prev = NULL;  /* 连续性基址清零：强制第一个buf新开一个 iovec 项 */
    iov = NULL;  /* 当前向量指针清空 */
    size = 0;  /* 累计请求读取的总字节数清零 */

    ngx_init_array(io, c->pool, 10, sizeof(struct iovec), NGX_ERROR);  /* 在连接内存池上初始化 iovec 数组并预分配10项；失败则宏内直接 return NGX_ERROR */

    /* 中文：合并内存相邻的buf——前一buf的end与后一buf的last相接即物理连续，共用同一iovec，缩短内核需遍历的向量表 */
    /* coalesce the neighbouring bufs */

    while (chain) {  /* 遍历buf链，把每个可写区登记为一个或多个向量 */
        if (prev == chain->buf->last) {  /* 本buf起点==上一buf末址：两块内存物理连续 */
            iov->iov_len += chain->buf->end - chain->buf->last;  /* 不新开向量，直接扩长前一项的 iov_len */

        } else {  /* 内存不连续：必须新开一个 iovec 项 */
            ngx_test_null(iov, ngx_push_array(&io), NGX_ERROR);  /* 向数组追加一项并取得指针；分配失败则返回 NGX_ERROR */
            iov->iov_base = (void *) chain->buf->last;  /* 向量基址=buf可写区起点（内核数据将拷入此处） */
            iov->iov_len = chain->buf->end - chain->buf->last;  /* 向量长度=buf可写字节数 */
        }  /* 向量项分支结束 */

        size += chain->buf->end - chain->buf->last;  /* 累计请求总量，供后面"实读<请求"的缓冲区排空判断 */
        prev = chain->buf->end;  /* 记录本buf末址，供下一轮连续性判断 */
        chain = chain->next;  /* 前进到链表下一节点 */
    }  /* 向量表构造完成 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志 */
                   "readv: %d, last:%d", io.nelts, iov->iov_len);  /* 打印向量个数与最后一个向量的长度 */

    rev = c->read;  /* 取读事件对象，进入系统调用阶段 */

    do {  /* do-while：仅 EINTR（信号打断且未拷贝任何数据）时重试 */
        n = readv(c->fd, (struct iovec *) io.elts, io.nelts);  /* 分散读系统调用：内核把套接字接收队列的数据按向量表逐项拷入多块用户内存 */

        if (n >= 0) {  /* 成功：n 为实际拷贝字节数（0 表示读到EOF） */
            if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {  /* kqueue 模块下按 available 精确记账 */
                rev->available -= n;  /* 从 kevent 报告的可读字节数中扣除本次已读量 */

                /* 中文：kevent() 与本次读取之间内核可能又收到新数据，实际读到的 n 可能
                 * 超过先前记录的 available，于是差值变成负数——正常现象，稍后钳位回0 */
                /*
                 * rev->available can be negative here because some additional
                 * bytes can be received between kevent() and recv()
                 */

                if (rev->available <= 0) {  /* kqueue 记账的可读数据已耗尽 */
                    if (!rev->pending_eof) {  /* 且对端没有挂起的 FIN */
                        rev->ready = 0;  /* 清除就绪：需等下一次 kqueue 读事件 */
                    }  /* 内层判断结束 */

                    if (rev->available < 0) {  /* 差值为负（新到数据被超额读出） */
                        rev->available = 0;  /* 钳位为0，维持状态语义一致 */
                    }  /* 钳位结束 */
                }  /* 记账调整结束 */

                return n;  /* kqueue 状态精确，直接返回本次读取字节数 */
            }  /* kqueue 记账路径结束 */

            if (n < size) {  /* 实读少于请求：接收缓冲区已被取空 */
                rev->ready = 0;  /* 清除就绪，等下一个读事件再来 */
            }  /* 排空判断结束 */

            if (n == 0) {  /* readv 返回0：对端已发送 FIN，读方向关闭 */
                rev->eof = 1;  /* 置EOF标志，上层据此收尾请求或连接 */
            }  /* EOF判断结束 */

            return n;  /* 返回实际读取的字节数 */
        }  /* 成功路径结束 */

        err = ngx_socket_errno;  /* readv 返回-1：先保存 errno（后续函数调用会覆盖它） */

        if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* 两类"暂时性"失败 */
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试日志 */
                           "readv() not ready");  /* EAGAIN=非阻塞套接字暂无数据(接收缓冲区空); EINTR=被信号打断 */
            n = NGX_AGAIN;  /* 转为 nginx 的"稍后再试"返回值 */

        } else {  /* 其余 errno（ECONNRESET/EBADF/EFAULT 等）为真实错误 */
            n = ngx_connection_error(c, err, "readv() failed");  /* 记录错误日志并返回 NGX_ERROR */
            break;  /* 跳出重试循环 */
        }  /* 错误分类结束 */

    } while (err == NGX_EINTR);  /* 仅因信号打断而重试；EAGAIN 则等事件通知，避免忙转 */

    rev->ready = 0;  /* 收尾：清除读就绪标志 */

    if (n == NGX_ERROR){  /* 发生了真实错误 */
        c->read->error = 1;  /* 标记连接读错误，上层将丢弃该连接上的处理 */
    }  /* 错误标记结束 */

    return n;  /* 返回：读取字节数 / 0(对端FIN) / NGX_AGAIN / NGX_ERROR */
}  /* kqueue 版 ngx_readv_chain 结束 */

/* 中文：以下为无 kqueue 的通用实现——EOF(返回0)与"暂无数据"(EAGAIN)只能从 readv 的返回值和 errno 事后推断 */
#else /* ! NAVE_KQUEUE */

/* 整体说明：通用版 ngx_readv_chain。同样先把 buf 链拼成 iovec 数组再一次性 readv，
 * 区别在于没有 kqueue 的预读信息，最坏情况会多一次"注定无数据"的系统调用开销 */
ssize_t ngx_readv_chain(ngx_connection_t *c, ngx_chain_t *chain)  /* c:连接; chain:待填充buf链 */
{  /* 通用版函数体开始 */
    u_char        *prev;  /* 上一buf末址，用于合并物理连续的buf */
    ssize_t        n, size;  /* n:实读字节数; size:本次请求总字节数 */
    ngx_err_t      err;  /* 保存 errno */
    ngx_array_t    io;  /* iovec 向量表 */
    ngx_event_t   *rev;  /* 读事件对象 */
    struct iovec  *iov;  /* 当前向量项 */

    prev = NULL;  /* 连续性基址清零 */
    iov = NULL;  /* 当前向量清空 */
    size = 0;  /* 请求总量清零 */

    ngx_init_array(io, c->pool, 10, sizeof(struct iovec), NGX_ERROR);  /* 连接内存池上初始化向量表，预分配10项iovec */

    /* 中文：合并相邻buf——首尾相接的buf共用一个iovec，减少向量条目与内核遍历开销 */
    /* coalesce the neighbouring bufs */

    while (chain) {  /* 遍历buf链构造向量表 */
        if (prev == chain->buf->last) {  /* 与上一buf内存连续 */
            iov->iov_len += chain->buf->end - chain->buf->last;  /* 扩长前一个向量即可 */

        } else {  /* 不连续：新开一个向量 */
            ngx_test_null(iov, ngx_push_array(&io), NGX_ERROR);  /* 追加一项，失败返回NGX_ERROR */
            iov->iov_base = chain->buf->last;  /* 向量基址=buf可写区起点 */
            iov->iov_len = chain->buf->end - chain->buf->last;  /* 向量长度=可写字节数 */
        }  /* 向量项分支结束 */

        size += chain->buf->end - chain->buf->last;  /* 累计请求总量 */
        prev = chain->buf->end;  /* 记录末址供下轮判断 */
        chain = chain->next;  /* 下一节点 */
    }  /* 向量表就绪 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志 */
                   "readv: %d:%d", io.nelts, iov->iov_len);  /* 打印向量个数与最后一个向量长度 */

    rev = c->read;  /* 取读事件对象 */

    do {  /* 仅 EINTR 时重试 */
        n = readv(c->fd, (struct iovec *) io.elts, io.nelts);  /* 分散读：一次系统调用把接收缓冲区数据散布到多个不连续用户块 */

        if (n == 0) {  /* 读到0字节：对端FIN，读方向正常关闭 */
            rev->ready = 0;  /* 清除就绪 */
            rev->eof = 1;  /* 置EOF标志 */

            return n;  /* 返回0 */

        } else if (n > 0) {  /* 读到数据 */

            /* 部分读(实读<请求)说明内核接收缓冲区已排空；除非事件模块声明 GREEDY_EVENT
             * (要求尽量一次读尽，省去多余的事件通知)，否则清除就绪标志 */
            if (n < size && !(ngx_event_flags & NGX_HAVE_GREEDY_EVENT)) {
                rev->ready = 0;  /* 清除就绪，等下次读事件 */
            }  /* 部分读判断结束 */

            return n;  /* 返回实读字节数 */
        }  /* 数据分支结束 */

        err = ngx_socket_errno;  /* n==-1：取出 errno */

        if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* 暂时性失败 */
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试日志 */
                           "readv() not ready");  /* EAGAIN=无数据可读; EINTR=被信号打断 */
            n = NGX_AGAIN;  /* 转为"稍后再试" */

        } else {  /* 真实错误 */
            n = ngx_connection_error(c, err, "readv() failed");  /* 记日志并返回NGX_ERROR */
            break;  /* 跳出循环 */
        }  /* 错误分类结束 */

    } while (err == NGX_EINTR);  /* 仅信号打断时重试 */

    rev->ready = 0;  /* 清除读就绪 */

    if (n == NGX_ERROR){  /* 真实错误 */
        c->read->error = 1;  /* 标记连接读错误 */
    }  /* 错误标记结束 */

    return n;  /* 返回最终结果 */
}  /* 通用版 ngx_readv_chain 结束 */

/* 编译开关结束：kqueue 版与通用版二选一 */
#endif /* NAVE_KQUEUE */
