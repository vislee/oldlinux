
/*
 * Copyright (C) Igor Sysoev
 */


/*
 * ngx_output_chain.c —— nginx 输出链核心过滤器。
 * 职责：把"内存buf与文件buf混合"的链表整理成底层可一次发送的输出链：
 * 免拷贝buf直接下发(可走sendfile零拷贝)，需拷贝buf先复制进临时buf；
 * 发送用聚集写(writev/sendfile)，部分发送时剩余挂链等待可写事件续传。
 */
#include <ngx_config.h>  /* 平台配置总头：探测宏(如sendfile/文件AIO可用性)决定各编译分支 */
#include <ngx_core.h>    /* 核心数据结构：ngx_buf_t/ngx_chain_t/内存池等定义 */
#include <ngx_event.h>   /* 事件框架：部分发送后依赖EPOLLOUT可写事件续传 */


#define NGX_NONE      1  /* 哨兵值：区别于NGX_OK(0)/错误码，表示"尚未调用过下级过滤器" */


ngx_inline static ngx_int_t  /* 声明为inline：主循环热点路径上的判断，省去调用开销 */
    ngx_output_chain_need_to_copy(ngx_output_chain_ctx_t *ctx, ngx_buf_t *buf);  /* 判断buf是否必须先拷入临时buf */
static ngx_int_t ngx_output_chain_copy_buf(ngx_buf_t *dst, ngx_buf_t *src,  /* 前置声明：把src数据拷进dst临时buf */
                                           ngx_uint_t sendfile);  /* sendfile开关：决定拷贝后dst是否仍带in_file属性 */


/*
 * ngx_output_chain()：输出链"整理+发送"主入口。
 * 把挂起链ctx->in与新输入in统一排队：能直发的buf直接进输出链，不能直发的
 * (无sendfile时的文件buf、下级需要内存数据等)先拷入临时buf；攒一批后调
 * ctx->output_filter(writev/sendfile聚集发送)，部分发送时保留剩余链，
 * 由上层在EPOLLOUT可写事件到来后再次调用本函数续传。
 */
ngx_int_t ngx_output_chain(ngx_output_chain_ctx_t *ctx, ngx_chain_t *in)  /* ctx=过滤器上下文, in=新到的buf链(NULL表示仅冲刷) */
{  /* 函数体开始 */
    int           rc, last;  /* rc=下级过滤器返回值; last=上次发送结果,NGX_NONE表示从未调用过 */
    size_t        size, bsize;  /* size=待分配临时buf大小; bsize=当前buf有效字节数 */
    ngx_chain_t  *cl, *out, **last_out;  /* cl=工作节点; out=待发输出链; last_out=尾节点next指针的地址(尾插法) */

    /*
     * the short path for the case when the ctx->in chain is empty
     * and the incoming chain is empty too or it has the single buf
     * that does not require the copy
     */

    if (ctx->in == NULL) {  /* 快速路径前提：没有上次遗留的未发数据 */

        if (in == NULL) {  /* 新输入也为空 */
            return ctx->output_filter(ctx->filter_ctx, in);  /* 直接透传(冲刷语义)，完全绕过拷贝整理 */
        }  /* 空输入分支结束 */

        if (in->next == NULL  /* 链上只有一个buf */
            && (!ngx_output_chain_need_to_copy(ctx, in->buf)))  /* 且该buf可被底层直接发送 */
        {  /* 免拷贝快速路径成立 */
            return ctx->output_filter(ctx->filter_ctx, in);  /* 原buf直接下传：省一次memcpy，底层可就地writev/sendfile */
        }  /* 单buf判断结束 */
    }  /* 快速路径结束 */

    /* add the incoming buf to the chain ctx->in */

    if (in) {  /* 存在新输入 */
        if (ngx_chain_add_copy(ctx->pool, &ctx->in, in) == NGX_ERROR) {  /* 新链追加到ctx->in尾：只复制链节点引用，不拷贝buf数据 */
            return NGX_ERROR;  /* 链节点分配失败(内存池耗尽) */
        }  /* 追加判断结束 */
    }  /* 有新输入分支结束 */

    last = NGX_NONE;  /* 置为"尚未调用过下级" */
    out = NULL;  /* 输出链清空 */
    last_out = &out;  /* 尾插指针初始指向链头指针本身 */

    for ( ;; ) {  /* 主循环：一轮=整理一批buf→发送一次→分拣结果，直至发完 */

        while (ctx->in) {  /* 内层：把挂起链尽量多地整理进输出链 */

            /*
             * cycle while there are the ctx->in bufs
             * or there are the free output bufs to copy in
             */

            bsize = ngx_buf_size(ctx->in->buf);  /* 当前buf字节数(内存buf取last-pos，文件buf取file_last-file_pos) */

            if (bsize == 0 && !ngx_buf_special(ctx->in->buf)) {  /* 0字节普通buf属上游异常(仅承载flush/last_buf标志的特殊buf合法) */

                ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,  /* 记ALERT级日志 */
                              "zero size buf");  /* 提示发现零长buf */

                ctx->in = ctx->in->next;  /* 丢弃坏buf，链前移 */

                continue;  /* 转去处理下一个 */
            }  /* 零长检查结束 */

            if (!ngx_output_chain_need_to_copy(ctx, ctx->in->buf)) {  /* 该buf无需拷贝(如内存buf且允许直发) */

                /* move the chain link to the output chain */

                cl = ctx->in;  /* 摘下当前节点(数据零拷贝，仅转移链节点所有权) */
                ctx->in = cl->next;  /* 挂起链前移 */

                *last_out = cl;  /* 尾插进输出链 */
                last_out = &cl->next;  /* 尾指针指向新尾节点的next */
                cl->next = NULL;  /* 与原链彻底断开，保证输出链以NULL收尾 */

                continue;  /* 继续整理下一个 */
            }  /* 免拷贝分支结束 */

            if (ctx->buf == NULL) {  /* 还没有用于拷贝的目的缓冲 */

                /* get the free buf */

                if (ctx->free) {  /* free链缓存着已发完可复用的buf：内存复用，省去反复malloc/free与缺页开销 */
                    ctx->buf = ctx->free->buf;  /* 取回链头buf */
                    ctx->free = ctx->free->next;  /* free链前移 */

                } else if (out || ctx->allocated == ctx->bufs.num) {  /* 输出链已有存货(先发再省内存)或分配数已达bufs.num上限(防大响应耗尽内存) */

                    break;  /* 跳出while：先发掉攒好的out，腾出free buf再继续 */

                } else {  /* 需要新分配一块临时buf */

                    size = ctx->bufs.size;  /* 默认按配置粒度bufs.size分配 */

                    if (ctx->in->buf->last_buf) {  /* 当前是响应最后一个buf：可按尾部实际长度分配以省内存 */

                        if (bsize < ctx->bufs.size) {  /* 尾部数据不足一整块 */

                           /*
                            * allocate small temp buf for the small last buf
                            * or its small last part
                            */

                            size = bsize;  /* 按实际字节数分配 */

                        } else if (ctx->bufs.num == 1  /* 配置仅允许1个buf时 */
                                   && (bsize < ctx->bufs.size  /* 且尾buf不足bufs.size的1.25倍 */
                                                     + (ctx->bufs.size >> 2)))  /* (>>2即除4，移位代替乘除) */
                        {  /* 尾部特判成立 */
                            /*
                             * allocate a temp buf that equals
                             * to the last buf if the last buf size is lesser
                             * than 1.25 of bufs.size and a temp buf is single
                             */

                            size = bsize;  /* 宁可下次少拷一次，也不让这块buf留下约1/4空闲内存 */
                        }  /* 1.25倍判断结束 */
                    }  /* last_buf特判结束 */

                    if (!(ctx->buf = ngx_create_temp_buf(ctx->pool, size))) {  /* 从内存池分配临时buf(池与连接同生命周期，统一释放) */
                        return NGX_ERROR;  /* 分配失败 */
                    }  /* 分配判断结束 */

                    ctx->buf->tag = ctx->tag;  /* 打模块标签：update_chains按tag认领归还free链，避免误收别家buf */
                    ctx->buf->recycled = 1;  /* 标记可回收：下级见recycled会强制flush，保证复用前旧数据已写出socket */
                    ctx->allocated++;  /* 已分配计数+1，用于和bufs.num上限比较 */
                }  /* 取buf三路分支结束 */
            }  /* 目的buf就绪 */

            rc = ngx_output_chain_copy_buf(ctx->buf, ctx->in->buf,  /* 把源buf(内存或文件)拷入临时buf：无sendfile通路在此完成read系统调用 */
                                           ctx->sendfile);  /* 透传sendfile开关 */

            if (rc == NGX_ERROR) {  /* 拷贝/读文件失败 */
                return rc;  /* 直接向上报错 */
            }  /* 错误判断结束 */

            if (rc == NGX_AGAIN) {  /* 文件AIO已提交但数据未就绪(NGX_FILE_AIO_READ) */
                if (out) {  /* 输出链已有存货 */
                    break;  /* 先发出去，AIO完成事件后再回来续拷 */
                }  /* 有存货判断结束 */

                return rc;  /* 无货可发：以NGX_AGAIN返回，等AIO完成通知后重入 */
            }  /* AIO未就绪分支结束 */

            /* delete the completed buf from the ctx->in chain */

            if (ngx_buf_size(ctx->in->buf) == 0) {  /* 源buf已全部拷完(读写指针追平) */
                ctx->in = ctx->in->next;  /* 从挂起链摘除 */
            }  /* 摘链判断结束(没拷完则保留，下轮续拷) */

            ngx_alloc_link_and_set_buf(cl, ctx->buf, ctx->pool, NGX_ERROR);  /* 分配链节点挂上装满数据的临时buf(失败宏内转NGX_ERROR) */
            *last_out = cl;  /* 尾插进输出链 */
            last_out = &cl->next;  /* 更新尾指针 */
            ctx->buf = NULL;  /* 交出buf所有权：下轮如需拷贝会重新取用或分配 */
        }  /* while(ctx->in)整理循环结束 */

        if (out == NULL && last != NGX_NONE) {  /* 本轮没整理出数据且已调用过下级 */
            return last;  /* 返回上次发送结果，避免空跑一次系统调用 */
        }  /* 空轮判断结束 */

        last = ctx->output_filter(ctx->filter_ctx, out);  /* 调下级聚集发送(writev/sendfile一次系统调用)；返回剩余链(NULL=发完) */

        ngx_chain_update_chains(&ctx->free, &ctx->busy, &out, ctx->tag);  /* 按tag分拣：发完的buf归还free链复用，未发完的移入busy链，out更新为剩余链 */
        last_out = &out;  /* 尾插指针复位到新out链头 */

        if (last == NGX_ERROR) {  /* 底层写失败(典型：对端提前关闭致EPIPE/ECONNRESET) */
            return last;  /* 向上传递，由HTTP层记日志并终止请求 */
        }  /* 错误判断结束 */
    }  /* for主循环结束 */
}  /* ngx_output_chain()结束 */


/*
 * 判断buf是否必须先拷入临时buf。拷贝与否决定数据通路：
 * 免拷贝→sendfile零拷贝(磁盘页缓存直达socket，省2次用户态/内核态间拷贝)；
 * 需拷贝→退化为read+write两次系统调用。
 */
ngx_inline static ngx_int_t  /* inline：每个输出buf都会调用一次 */
    ngx_output_chain_need_to_copy(ngx_output_chain_ctx_t *ctx, ngx_buf_t *buf)  /* ctx携带各开关, buf为待判缓冲 */
{  /* 函数体开始 */
    if (ngx_buf_special(buf)) {  /* 特殊buf(只承载flush/last_buf/sync等标志，无数据) */
        return 0;  /* 无需拷贝，标志buf直接下传 */
    }  /* 特殊buf判断结束 */

    if (!ctx->sendfile) {  /* 平台无sendfile或被sendfile off指令关闭 */
        if (!ngx_buf_in_memory(buf)) {  /* 数据只存在于文件(页缓存) */
            return 1;  /* 必须read进内存buf，走普通write路径 */
        }  /* 内存判断结束 */

        buf->in_file = 0;  /* 内存buf却带in_file标志时清除，防下级按文件偏移错发 */
    }  /* sendfile开关分支结束 */

    if (ctx->need_in_memory && !ngx_buf_in_memory(buf)) {  /* 下级需直接读数据(如SSL加密/计算长度)，文件buf不满足 */
        return 1;  /* 拷入内存 */
    }  /* need_in_memory判断结束 */

    if (ctx->need_in_temp && (buf->memory || buf->mmap)) {  /* 下级要改写数据(需私有副本)：共享内存/mmap映射页不能就地修改 */
        return 1;  /* 复制成可写临时buf */
    }  /* need_in_temp判断结束 */

    return 0;  /* 免拷贝：走最快通路(直发或sendfile零拷贝) */
}  /* ngx_output_chain_need_to_copy()结束 */


/*
 * 把src buf的数据拷入dst临时buf(单次最多拷到dst容量，剩余下轮续拷)。
 * 两条通路：源在内存→纯用户态memcpy；源在文件→read系统调用(经页缓存)。
 */
static ngx_int_t ngx_output_chain_copy_buf(ngx_buf_t *dst, ngx_buf_t *src,  /* dst=目的临时buf, src=源buf */
                                           ngx_uint_t sendfile)  /* 发送侧是否用sendfile：决定dst的in_file属性 */
{  /* 函数体开始 */
    size_t   size;  /* 本次拷贝字节数 */
    ssize_t  n;  /* read实际返回字节数(可为NGX_ERROR/NGX_AGAIN) */

    size = ngx_buf_size(src);  /* 源buf待拷贝字节数 */

    if (size > (size_t) (dst->end - dst->pos)) {  /* 超过目的buf剩余容量 */
        size = dst->end - dst->pos;  /* 钳制到容量：剩余部分下轮续拷 */
    }  /* 容量钳制结束 */

    if (ngx_buf_in_memory(src)) {  /* 源数据已在内存：用户态memcpy，不陷入内核 */
        ngx_memcpy(dst->pos, src->pos, size);  /* 复制到目的buf写指针处 */
        src->pos += size;  /* 源读指针前移 */
        dst->last += size;  /* 目的写指针前移 */

        if (src->in_file) {  /* 该内存buf同时关联文件区间(如mmap映射的文件头) */
            src->file_pos += size;  /* 文件偏移同步前移，保持两套指针一致 */
        }  /* 偏移同步结束 */

        if (src->last_buf && src->pos == src->last) {  /* 源带响应结束标志且已拷完 */
            dst->last_buf = 1;  /* 标志转移给dst：下级据此收尾(如决定关闭连接) */
        }  /* 标志转移判断结束 */

    } else {  /* 源数据只在文件：须read读入(未用sendfile的通路) */
        n = ngx_read_file(src->file, dst->pos, size, src->file_pos);  /* 从file_pos读size字节到dst->pos：走Linux页缓存，冷页触发磁盘I/O */

        if (n == NGX_ERROR) {  /* 读文件出错 */
            return n;  /* 向上报告 */
        }  /* 错误判断结束 */

#if (NGX_FILE_AIO_READ)  /* 平台支持文件AIO(如Linux原生aio)才编译此段 */
        if (n == NGX_AGAIN) {  /* AIO请求已提交、数据尚未就绪 */
            return n;  /* 立即返回：进程不阻塞，等AIO完成事件唤醒后重入 */
        }  /* AIO未就绪判断结束 */
#endif  /* 文件AIO条件编译结束 */

        if ((size_t) n != size) {  /* 实际读取不足请求量 */
            ngx_log_error(NGX_LOG_ALERT, src->file->log, 0,  /* 记ALERT级日志 */
                          ngx_read_file_n " reads only %d of %d from file",  /* 典型原因：文件被并发截短 */
                          n, size);  /* 附上实际/期望字节数 */
            if (n == 0) {  /* 读到0字节即EOF：文件比元数据记录的短 */
                return NGX_ERROR;  /* 视为致命错误 */
            }  /* EOF判断结束 */
        }  /* 短读判断结束 */

        src->file_pos += n;  /* 源文件偏移前移 */
        dst->last += n;  /* 目的写指针前移 */

        if (!sendfile) {  /* 发送侧不用sendfile */
            dst->in_file = 0;  /* dst按纯内存buf发送(write而非sendfile) */
        }  /* in_file属性设置结束 */

        if (src->last_buf && src->file_pos == src->file_last) {  /* 读到文件末尾且源是响应最后一块 */
            dst->last_buf = 1;  /* 结束标志转移到dst */
        }  /* 标志转移判断结束 */
    }  /* 内存/文件两通路结束 */

    return NGX_OK;  /* 本轮拷贝成功 */
}  /* ngx_output_chain_copy_buf()结束 */


/*
 * 通用链式发送器(output_filter的一种实现，供上游/代理模块使用)：
 * 新输入链先追加进自己的ctx->out，再整体交ngx_send_chain聚集发送。
 */
ngx_int_t ngx_chain_writer(void *data, ngx_chain_t *in)  /* data=创建时绑定的上下文, in=新输入链 */
{  /* 函数体开始 */
    ngx_chain_writer_ctx_t *ctx = data;  /* 还原类型化上下文(连接、out链、限速limit) */

    ngx_chain_t  *cl;  /* 工作链节点 */


    for (/* void */; in; in = in->next) {  /* 遍历新输入链(无初始化子句) */

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, ctx->connection->log, 0,  /* debug级日志 */
                       "WRITER buf: %d", ngx_buf_size(in->buf));  /* 打印每个buf的大小 */

        ngx_alloc_link_and_set_buf(cl, in->buf, ctx->pool, NGX_ERROR);  /* 为每个新buf新建链节点(仅引用数据，零拷贝入队) */
        *ctx->last = cl;  /* 尾插到ctx->out(含上次未发完的剩余部分) */
        ctx->last = &cl->next;  /* 维护尾插指针 */
    }  /* 入队循环结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ctx->connection->log, 0,  /* debug级日志 */
                   "WRITER0: %X", ctx->out);  /* 发送前剩余链头地址 */

    ctx->out = ngx_send_chain(ctx->connection, ctx->out, ctx->limit);  /* 聚集发送：writev/sendfile尽量发完；TCP发送缓冲(SO_SNDBUF)满或对端窗口为0时返回剩余链(部分写) */

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ctx->connection->log, 0,  /* debug级日志 */
                   "WRITER1: %X", ctx->out);  /* 发送后剩余链头(NULL=全部发完) */

    if (ctx->out == NGX_CHAIN_ERROR) {  /* 发送出错(fd不可写/连接被对端复位) */
        return NGX_ERROR;  /* 上级据此终止本次处理 */
    }  /* 错误判断结束 */

    if (ctx->out == NULL) {  /* 所有数据已拷入内核发送缓冲 */
        ctx->last = &ctx->out;  /* 尾插指针复位，下次从链头重新追加 */
        return NGX_OK;  /* 完成：上层可撤销EPOLLOUT关注，减少无谓的事件唤醒 */
    }  /* 发完判断结束 */

    return NGX_AGAIN;  /* 部分发送：剩余链留在ctx->out，等EPOLLOUT(对端滑动窗口张开)后续传 */
}  /* ngx_chain_writer()结束 */
