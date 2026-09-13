
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台特性宏（HAVE_SENDFILE64、NGX_EAGAIN 等） */
#include <ngx_core.h>  /* nginx 核心基础设施：内存池、数组、buf/chain */
#include <ngx_event.h>  /* 事件框架：连接、写事件、ngx_tcp_nopush 封装 */


/*
 * On Linux up to 2.4.21 sendfile() (syscall #187) works with 32-bit
 * offsets only and the including <sys/sendfile.h> breaks the compiling
 * if off_t is 64 bit wide.  So we use own sendfile() definition where offset
 * parameter is int32_t and use sendfile() with the file parts below 2G.
 *
 * Linux 2.4.21 has a new sendfile64() syscall #239.
 */
/*
 * 中文大意：Linux 2.4.21 之前的 sendfile()（系统调用号187）只支持32位文件偏移，
 * 且 off_t 为64位时包含 <sys/sendfile.h> 会编译失败；因此 nginx 自行声明 offset
 * 参数为 int32_t 的 sendfile()，仅对 2G 以内的文件段使用 sendfile。
 * Linux 2.4.21 起新增了 sendfile64()（系统调用号239）。
 */


#define NGX_HEADERS   8  /* 响应头 iovec 数组的栈上预分配容量 */


/* 整体说明：Linux 专属的 sendfile 零拷贝发送。传统"读文件再发网络"路径为
 * 磁盘→页缓存→用户缓冲区→socket发送缓冲区→网卡，共4次数据拷贝外加多次冗余的
 * 用户态/内核态切换；sendfile(out_fd=socket, in_fd=文件, &offset, count) 全程在
 * 内核态完成：数据从页缓存直接进入发送流程（配合网卡 DMA scatter-gather 甚至无需
 * CPU 拷贝），用户态零拷贝。本函数先收集内存buf（HTTP响应头）并在其后有文件buf时
 * 设置 TCP_CORK，使响应头与文件首段聚合为同一个TCP段；文件部分用 sendfile 发送，
 * 纯内存部分退回 writev 集中写 */
ngx_chain_t *ngx_linux_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in,  /* 返回剩余未发送的链; in:待发送buf链 */
                                      off_t limit)  /* limit:本次最多发送的字节数 */
{  /* 函数体开始 */
    int              rc;  /* sendfile/writev 的返回值（实发字节数或 -1） */
    u_char          *prev;  /* 上一buf数据末址，用于合并内存连续的buf */
    off_t            fprev, send, sprev, aligned;  /* fprev:已并入文件的下一偏移; send:累计请求量; sprev:调用前快照; aligned:页对齐后的偏移 */
    size_t           fsize;  /* 本轮交给 sendfile 的文件字节数 */
    ssize_t          size, sent;  /* size:当前buf长度; sent:单次系统调用实发字节 */
    ngx_uint_t       eintr, complete;  /* eintr:被信号打断; complete:本轮是否全部发完 */
    ngx_err_t        err;  /* 保存 errno */
    ngx_buf_t       *file;  /* 命中的文件buf：sendfile 需要其中的文件fd与偏移 */
    ngx_array_t      header;  /* 响应头 iovec 数组 */
    ngx_event_t     *wev;  /* 写事件对象 */
    ngx_chain_t     *cl;  /* 链遍历游标 */
    struct iovec    *iov, headers[NGX_HEADERS];  /* 当前向量项 + 栈上预分配8项 */
#if (HAVE_SENDFILE64)  /* 支持 sendfile64 系统调用时用64位偏移 */
    off_t            offset;  /* sendfile 的入出参：传入起始偏移，内核推进后写回 */
#else  /* 旧内核（syscall #187）只认32位偏移 */
    int32_t          offset;  /* 32位偏移，文件段须位于 2G 以内 */
#endif  /* 偏移宽度选择结束 */

    wev = c->write;  /* 取写事件对象 */

    if (!wev->ready) {  /* 写未就绪：发送缓冲区仍满 */
        return in;  /* 原样退回，等写事件 */
    }  /* 就绪检查结束 */

    send = 0;  /* 累计请求量清零 */

    header.elts = headers;  /* 数组使用栈空间 */
    header.size = sizeof(struct iovec);  /* 元素大小 */
    header.nalloc = NGX_HEADERS;  /* 容量8 */
    header.pool = c->pool;  /* 超出后向连接内存池扩容 */

    for ( ;; ) {  /* 主循环：内存头(TCP_CORK)→文件/头发送→按实发消费buf→必要时再来一轮 */
        file = NULL;  /* 重置：本轮命中的文件buf */
        fsize = 0;  /* 文件发送量清零 */
        eintr = 0;  /* EINTR标志清零 */
        complete = 0;  /* 完成标志清零 */
        sprev = send;  /* 快照本轮开始时的累计量 */

        header.nelts = 0;  /* 头向量个数清零（复用栈数组） */

        prev = NULL;  /* 连续性基址重置 */
        iov = NULL;  /* 当前向量重置 */

        /* 中文：合并相邻buf——内存首尾相接的buf共用同一iovec，减少向量条目与内核遍历开销 */
        /* create the iovec and coalesce the neighbouring bufs */

        for (cl = in;  /* 从链头开始 */
             cl && header.nelts < IOV_MAX && send < limit;  /* 受向量数上限与流量限额双重约束 */
             cl = cl->next)  /* 前进到下一节点 */
        {  /* 循环体开始 */
            if (ngx_buf_special(cl->buf)) {  /* 特殊buf（同步/刷新标记）不产生I/O */
                continue;  /* 跳过 */
            }  /* 特殊buf过滤结束 */

            if (!ngx_buf_in_memory_only(cl->buf)) {  /* 不是"纯内存buf"（即含文件数据的buf） */
                break;  /* 内存头收集到此为止：文件部分要单独交给 sendfile */
            }  /* 类型判断结束 */

            size = cl->buf->last - cl->buf->pos;  /* 该buf待发字节数 */

            if (send + size > limit) {  /* 将超出流量限额 */
                size = limit - send;  /* 截取限额内的部分 */
            }  /* 限额截断结束 */

            if (prev == cl->buf->pos) {  /* 与上一buf内存连续：合并同一向量 */
                iov->iov_len += size;  /* 扩长前一项即可 */

            } else {  /* 不连续：新增向量项 */
                if (!(iov = ngx_array_push(&header))) {  /* 追加一项（8个用尽后从内存池扩容） */
                    return NGX_CHAIN_ERROR;  /* 扩容失败则放弃 */
                }  /* 追加判断结束 */

                iov->iov_base = (void *) cl->buf->pos;  /* 向量基址=待发数据起点 */
                iov->iov_len = size;  /* 向量长度 */
            }  /* 向量项构造结束 */

            prev = cl->buf->pos + size;  /* 记录末址供连续性判断 */
            send += size;  /* 累计请求量 */
        }  /* 内存头 iovec 构造结束 */

        /* 中文：文件数据前还有响应头时设置 TCP_CORK——cork 攒够一个MSS(约1460B)才发，
         * 使响应头与文件首段聚合进同一个TCP段，避免头/体分包引发 Nagle+延迟ACK 空等 */
        /* set TCP_CORK if there is a header before a file */

        if (c->tcp_nopush == NGX_TCP_NOPUSH_UNSET  /* cork 状态为"未设置"（状态机初始态） */
            && header.nelts != 0  /* 且本轮已收集到内存头 */
            && cl  /* 且后面还有节点 */
            && cl->buf->in_file)  /* 且紧跟的是文件数据buf */
        {  /* 条件满足：需要 cork */
            if (ngx_tcp_nopush(c->fd) == NGX_ERROR) {  /* setsockopt(TCP_CORK) 失败 */
                err = ngx_errno;  /* 取errno */

                /* 中文大意：被信号打断(EINTR)的概率极小；即便如此也继续走无 TCP_CORK 的发送 */
                /*
                 * there is a tiny chance to be interrupted, however
                 * we continue a processing without the TCP_CORK
                 */

                if (err != NGX_EINTR) { /* 非信号中断的真实错误 */
                    wev->error = 1;  /* 标记连接写错误 */
                    ngx_connection_error(c, err, ngx_tcp_nopush_n " failed");  /* 记录 setsockopt(TCP_CORK) 失败日志 */
                    return NGX_CHAIN_ERROR;  /* 放弃本次发送 */
                }  /* 真错误处理结束 */

            } else {  /* TCP_CORK 设置成功 */
                c->tcp_nopush = NGX_TCP_NOPUSH_SET;  /* 状态机置"已cork"：发送完后需 ngx_tcp_push 解除冲刷 */

                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志 */
                               "tcp_nopush");  /* 打印 cork 已设置 */
            }  /* cork 设置结束 */
        }  /* TCP_CORK 条件结束 */

        /* 中文：收集文件buf——sendfile 一次只能对应一个文件fd，仅合并同一文件且偏移无缝衔接的段 */
        /* get the file buf */

        /* 没有内存头待发（本轮未收集到或已耗尽）、当前buf是文件数据且未达限额：
         * 满足这些条件才把发送权交给 sendfile 零拷贝路径 */
        if (header.nelts == 0 && cl && cl->buf->in_file && send < limit) {
            file = cl->buf;  /* 记录文件buf：sendfile 需要 fd 与起始偏移 */

            /* coalesce the neighbouring file bufs */  /* 原注释：合并相邻的文件buf */

            do {  /* 把同一文件中偏移连续的文件段并为一次 sendfile，进一步减少系统调用次数 */
                size = (size_t) (cl->buf->file_last - cl->buf->file_pos);  /* 该文件段长度（区间是文件偏移而非内存地址） */

                if (send + size > limit) {  /* 超出限额需要截断 */
                    size = limit - send;  /* 先按限额截 */

                    /* 截断点向上取整到页边界：sendfile 在内核按页建立映射与传输，
                     * 保持后续偏移页对齐，避免最后一页被拆成低效的零散小段 */
                    aligned = (cl->buf->file_pos + size + ngx_pagesize - 1)  /* 先加"页大小-1" */
                                                      & ~(ngx_pagesize - 1);  /* 再按页掩码截位，即向上对齐到 ngx_pagesize 的整数倍 */

                    if (aligned <= cl->buf->file_last) {  /* 对齐点未越过本buf末尾 */
                        size = aligned - cl->buf->file_pos;  /* 改按页对齐后的长度发送 */
                    }  /* 对齐检查结束 */
                }  /* 截断处理结束 */

                fsize += size;  /* 累计本次 sendfile 总长度 */
                send += size;  /* 累计总请求量 */
                fprev = cl->buf->file_pos + size;  /* 记录本段结束偏移 */
                cl = cl->next;  /* 下一节点 */

            } while (cl  /* 继续合并的条件：还有节点 */
                     && cl->buf->in_file  /* 仍是文件数据 */
                     && send < limit  /* 未达限额 */
                     && file->file->fd == cl->buf->file->fd  /* 同一文件fd（跨文件无法一次sendfile） */
                     && fprev == cl->buf->file_pos);  /* 且偏移无缝衔接（上一段末==下一段首） */
        }  /* 文件buf收集结束 */

        if (file) {  /* 本轮有文件数据：走零拷贝路径 */
#if (HAVE_SENDFILE64)  /* 有 sendfile64 系统调用 */
            offset = file->file_pos;  /* 64位起始文件偏移 */
#else  /* 否则用旧版32位系统调用 */
            offset = (int32_t) file->file_pos;  /* 截为32位偏移（syscall #187 的限制） */
#endif  /* 偏移类型选择结束 */
            rc = sendfile(c->fd, file->file->fd, &offset, fsize);  /* 零拷贝发送：页缓存数据经内核直接进入socket发送流程，不途经用户态 */

            if (rc == -1) {  /* 系统调用失败 */
                err = ngx_errno;  /* 取errno */

                if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* 暂时性失败 */
                    if (err == NGX_EINTR) {  /* 被信号打断 */
                        eintr = 1;  /* 置重试标志 */
                    }  /* EINTR判断结束 */

                    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试日志 */
                                   "sendfile() is not ready");  /* EAGAIN=socket发送缓冲区满(或文件页尚未读入内存)，等EPOLLOUT */

                } else {  /* EPIPE/ECONNRESET/EBADF 等真实错误 */
                    wev->error = 1;  /* 标记写错误 */
                    ngx_connection_error(c, err, "sendfile() failed");  /* 记录错误日志 */
                    return NGX_CHAIN_ERROR;  /* 放弃 */
                }  /* 错误分类结束 */
            }  /* 失败处理结束 */

            sent = rc > 0 ? rc : 0;  /* 实发字节数（部分发送时小于 fsize） */

            ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志 */
                           "sendfile: %d, @" OFF_T_FMT " %d:%d",  /* 打印返回值/文件偏移/实发/请求 */
                           rc, file->file_pos, sent, fsize);  /* 四个实参 */

        } else {  /* 无文件数据：纯内存头走 writev 集中写 */
            rc = writev(c->fd, header.elts, header.nelts);  /* 一次系统调用把响应头各片段拷入发送缓冲区 */

            if (rc == -1) {  /* 失败 */
                err = ngx_errno;  /* 取errno */

                if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* 暂时性失败 */
                    if (err == NGX_EINTR) {  /* 信号打断 */
                        eintr = 1;  /* 置重试标志 */
                    }  /* EINTR判断结束 */

                    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试日志 */
                                   "writev() not ready");  /* EAGAIN=发送缓冲区满，等EPOLLOUT */

                } else {  /* 真实错误 */
                    wev->error = 1;  /* 标记写错误 */
                    ngx_connection_error(c, err, "writev() failed");  /* 记录错误日志 */
                    return NGX_CHAIN_ERROR; /* 放弃本次发送 */
                }  /* 错误分类结束 */
            }  /* 失败处理结束 */

            sent = rc > 0 ? rc : 0;  /* 实发字节数 */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "writev: %d", sent);  /* 调试日志：实发量 */
        }  /* sendfile/writev 分支结束 */

        if (send - sprev == sent) {  /* 本轮请求量==实发量：全部发完 */
            complete = 1;  /* 置完成标志 */
        }  /* 完成判断结束 */

        c->sent += sent;  /* 累加连接级已发送字节计数 */

        for (cl = in; cl; cl = cl->next) {  /* 按实发量消费buf链：内存pos与文件file_pos都要推进 */

            if (ngx_buf_special(cl->buf)) {  /* 特殊buf不占字节数 */
                continue;  /* 跳过 */
            }  /* 过滤结束 */

            if (sent == 0) {  /* 已无可消费字节：此后的buf本轮均未发出 */
                break;  /* cl 指向第一个未发送buf，作为返回值 */
            }  /* 消费完毕判断结束 */

            size = ngx_buf_size(cl->buf);  /* 该buf总字节数（内存或文件区间大小） */

            if (sent >= size) {  /* 整个buf都已发出 */
                sent -= size;  /* 扣减剩余实发量 */

                if (ngx_buf_in_memory(cl->buf)) {  /* 含内存数据 */
                    cl->buf->pos = cl->buf->last;  /* pos推到last：内存部分用尽 */
                }  /* 内存推进结束 */

                if (cl->buf->in_file) {  /* 含文件数据 */
                    cl->buf->file_pos = cl->buf->file_last;  /* 文件偏移推到末尾：文件部分用尽 */
                }  /* 文件推进结束 */

                continue;  /* 继续消费下一个buf */
            }  /* 整buf消费结束 */

            if (ngx_buf_in_memory(cl->buf)) {  /* 部分发送且含内存数据 */
                cl->buf->pos += sent;  /* pos前移已发字节数 */
            }  /* 内存部分推进结束 */

            if (cl->buf->in_file) {  /* 部分发送且含文件数据 */
                cl->buf->file_pos += sent;  /* 文件偏移前移：下次sendfile从新偏移继续 */
            }  /* 文件部分推进结束 */

            break;  /* 剩余数据留在此buf，返回调用方 */
        }  /* buf消费循环结束 */

        if (eintr) {  /* 被信号打断：无错误 */
            continue; /* 重试本轮 */
        }  /* EINTR处理结束 */

        if (!complete) { /* 部分发送：内核缓冲区已满 */
            wev->ready = 0;  /* 清除写就绪，等待 EPOLLOUT */
            return cl;  /* 返回剩余链，等下次写事件续发 */
        }  /* 部分写处理结束 */

        if (send >= limit || cl == NULL) {  /* 已达限额或全部发完 */
            return cl;  /* 返回（cl==NULL 表示链已发完） */
        }  /* 限额/完成判断结束 */

        in = cl;  /* 仍有数据且未达限额：从断点继续下一轮 */
    }  /* 主循环结束 */
}  /* ngx_linux_sendfile_chain 结束 */
