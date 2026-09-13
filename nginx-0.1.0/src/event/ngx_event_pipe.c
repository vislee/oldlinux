
/*
 * Copyright (C) Igor Sysoev
 */


/*
 * ngx_event_pipe.c —— nginx 事件管道：上游→本地缓冲→下游的数据搬运核心。
 * 代理/下载场景中TCP数据流的中转站：上游读事件就绪时用readv把内核接收
 * 缓冲区中的数据拷入内存buf链；内存不够用时把积压buf pwrite进临时文件
 * 腾出内存（文件buf此后可走sendfile零拷贝直发客户端）；下游写事件就绪
 * 时经输出过滤器(writev/sendfile)发出。整体受两级流控约束：bufs.num
 * 限制内存buf总数（应用层接收窗口）、max_temp_file_size限制磁盘缓冲、
 * busy_size与下游可写性限制发送速率（对下游的发送窗口）。
 */
#include <ngx_config.h>  /* 平台配置总头：探测宏决定readv/sendfile等特性编译分支 */
#include <ngx_core.h>  /* 核心数据结构：ngx_buf_t/ngx_chain_t/内存池/临时文件 */
#include <ngx_event.h>  /* 事件框架：读写事件就绪通知、ngx_handle_*_event登记 */
#include <ngx_event_pipe.h>  /* 管道上下文ngx_event_pipe_t及各标志位、回调定义 */


static ngx_int_t ngx_event_pipe_read_upstream(ngx_event_pipe_t *p);  /* 前置声明：从上游socket读数据进buf链 */
static ngx_int_t ngx_event_pipe_write_to_downstream(ngx_event_pipe_t *p);  /* 前置声明：把buf链发给下游客户端 */

static ngx_int_t ngx_event_pipe_write_chain_to_temp_file(ngx_event_pipe_t *p);  /* 前置声明：把内存积压刷进临时文件腾buf */
ngx_inline static void ngx_event_pipe_remove_shadow_links(ngx_buf_t *buf);  /* 前置声明：解除buf的shadow派生链 */
ngx_inline static void ngx_event_pipe_free_shadow_raw_buf(ngx_chain_t **free,  /* 前置声明：提前回收文件buf背后的raw内存buf */
                                                          ngx_buf_t *buf);  /* 续行：即将发送的文件buf */
ngx_inline static void ngx_event_pipe_add_free_buf(ngx_chain_t **chain,  /* 前置声明：raw buf按序归还空闲链 */
                                                   ngx_chain_t *cl);  /* 续行：待归还的链节点 */
static ngx_int_t ngx_event_pipe_drain_chains(ngx_event_pipe_t *p);  /* 前置声明：出错后丢弃积压并回收全部buf */


/*
 * ngx_event_pipe()：管道主循环——事件驱动的"写→读→再写"搬运调度。
 * 一轮=先把已有buf发给下游腾出缓冲空间，再从上游读满buf；只要本轮读到
 * 新数据或上游被反压就再来一轮，直到无进展为止。随后按需重新登记上/
 * 下游事件并挂超时定时器，剩余搬运交给事件机制在下次就绪时唤醒续传。
 */
ngx_int_t ngx_event_pipe(ngx_event_pipe_t *p, int do_write)  /* p=管道上下文; do_write=进入时是否先执行一轮写下游 */
{  /* 函数体开始 */
    u_int         flags;  /* 传给ngx_handle_read_event的事件登记方式标志 */
    ngx_event_t  *rev, *wev;  /* 上游读事件/下游写事件对象指针 */

    for ( ;; ) {  /* "写→读"循环：写到腾出空间→读入新数据，直至双方暂无进展 */
        if (do_write) {  /* 要求先发送（首轮冲刷遗留，或上轮读到了新数据） */
            if (ngx_event_pipe_write_to_downstream(p) == NGX_ABORT) {  /* 先推下游：腾出buf才能继续接纳上游数据 */
                return NGX_ABORT;  /* 下游处理发生致命错误(如内存池耗尽) */
            }  /* 写下游判断结束 */
        }  /* 先写判断结束 */

        p->read = 0;  /* 清"本轮已从上游读到数据"标志，由读函数按实情置位 */
        p->upstream_blocked = 0;  /* 清"上游被下游反压"标志，由读函数按实情置位 */

        if (ngx_event_pipe_read_upstream(p) == NGX_ABORT) {  /* 从上游socket尽可能多读数据填入buf链 */
            return NGX_ABORT;  /* 读上游发生致命错误 */
        }  /* 读上游判断结束 */

        if (!p->read && !p->upstream_blocked) {  /* 本轮既无新数据、也非因下游就绪而主动让路 */
            break;  /* 无进展即退出：交还控制权给事件机制，避免应用层忙转 */
        }  /* 无进展判断结束 */

        do_write = 1;  /* 有新数据入缓冲：下轮先写给下游腾buf，再继续读 */
    }  /* 搬运主循环结束 */

    if (p->upstream->fd != -1) {  /* 上游连接仍打开(-1表示已关闭，如eof后主动断开) */
        rev = p->upstream->read;  /* 取上游连接的读事件对象 */

        flags = (rev->eof || rev->error) ? NGX_CLOSE_EVENT : 0;  /* 已eof/出错则按关闭语义登记，事件机制可摘除监听 */

        if (ngx_handle_read_event(rev, flags) == NGX_ERROR) {  /* 按电平/边沿语义把读事件重新登记进事件机制 */
            return NGX_ABORT;  /* 事件登记失败 */
        }  /* 登记判断结束 */

        if (rev->active) {  /* 读事件仍在监听集合中(数据未读完且连接未关) */
            ngx_add_timer(rev, p->read_timeout);  /* 重置读超时定时器，防对端半开连接挂死本端 */
        }  /* 挂定时器判断结束 */
    }  /* 上游连接判断结束 */

    if (p->downstream->fd != -1) {  /* 下游(客户端)连接仍打开 */
        wev = p->downstream->write;  /* 取下游连接的写事件对象 */
        wev->available = p->send_lowat;  /* 设置发送低水位(SO_SNDLOWAT)：缓冲积到低水位才报可写，减少事件抖动 */
        if (ngx_handle_write_event(wev, NGX_LOWAT_EVENT) == NGX_ERROR) {  /* 重新登记写事件并应用低水位设置 */
            return NGX_ABORT;  /* 事件登记失败 */
        }  /* 登记判断结束 */

        if (wev->active) {  /* 写事件仍活跃(还有数据待发往下游) */
            ngx_add_timer(wev, p->send_timeout);  /* 重置发送超时：防客户端停止读取长期占用连接与缓冲 */
        }  /* 挂定时器判断结束 */
    }  /* 下游连接判断结束 */

    return NGX_OK;  /* 本轮管道搬运完成，其余工作等事件唤醒 */
}  /* 函数结束 */


/*
 * ngx_event_pipe_read_upstream()：从上游连接读数据入buf链。
 * 读事件就绪(或预读残留)时循环：优先取free链空闲raw buf，不足且未超
 * bufs.num上限则新分配；buf耗尽时若下游可写先去发下游，否则把p->in
 * 积压pwrite进临时文件腾内存。用ngx_recv_chain(readv聚集读)一次拷入
 * 多个buf；非阻塞下EAGAIN返回NGX_AGAIN挂起等事件；n==0即对端FIN。
 */
ngx_int_t ngx_event_pipe_read_upstream(ngx_event_pipe_t *p)  /* p=管道上下文 */
{  /* 函数体开始 */
    int           n, rc, size;  /* n=readv返回字节数; rc=临时文件写入返回码; size=单buf剩余容量 */
    ngx_buf_t    *b;  /* 新分配的内存buf指针 */
    ngx_chain_t  *chain, *cl, *tl;  /* 本轮供readv散布的buf链; 工作节点; 新分配的链节点 */

    if (p->upstream_eof || p->upstream_error || p->upstream_done) {  /* 上游已eof/出错/被上层宣告结束 */
        return NGX_OK;  /* 无可再读，直接返回 */
    }  /* 终结状态判断结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏：仅NGX_DEBUG编译时展开 */
                   "pipe read upstream: %d", p->upstream->read->ready);  /* 打印上游读事件当前是否就绪 */

    for ( ;; ) {  /* 读循环：每轮=准备buf→readv→按填充量分账，直到无可读 */

        if (p->upstream_eof || p->upstream_error || p->upstream_done) {  /* 循环内复查终结状态(上轮readv可能读到eof) */
            break;  /* 上游已结束，退出读循环 */
        }  /* 终结状态判断结束 */

        if (p->preread_bufs == NULL && !p->upstream->read->ready) {  /* 无预读残留且内核接收队列无数据(事件未就绪) */
            break;  /* 暂无可读数据，挂起等下次读事件 */
        }  /* 就绪判断结束 */

        if (p->preread_bufs) {  /* 存在请求头解析等阶段预读残留的数据buf */

            /* use the pre-read bufs if they exist */

            chain = p->preread_bufs;  /* 直接把预读buf链当作本次的数据来源链 */
            p->preread_bufs = NULL;  /* 摘走后清空，避免重复消费 */
            n = p->preread_size;  /* n记为预读数据实际字节数(相当于已"读满") */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                           "pipe preread: %d", n);  /* 打印预读数据字节数 */

            if (n) {  /* 预读确实带来了数据 */
                p->read = 1;  /* 标记本轮有进展，促使主循环写下游 */
            }  /* 有数据判断结束 */

        } else {  /* 无预读：进入正常的事件驱动读取路径 */

            /*
             * kqueue notifies about the end of file or a pending error.
             * This test allows not to allocate a buf on these conditions
             * and not to call ngx_recv_chain().
             */

            if (p->upstream->read->available == 0  /* kqueue报告已无可用字节 */
                && p->upstream->read->pending_eof)  /* 且挂起eof标志置位(对端FIN且数据已被取尽) */
            {  /* kqueue专有eof检测分支 */
                p->upstream->read->ready = 0;  /* 清就绪标志：eof已消化 */
                p->upstream->read->eof = 0;  /* 清事件层eof标志，避免重复处理 */
                p->upstream_eof = 1;  /* 置管道级eof：上游读方向正常关闭 */
                p->read = 1;  /* eof也算进展：促使主循环把余量发给下游 */

#if (HAVE_KQUEUE)  /* 仅kqueue机制：eof事件可携带内核错误码 */
                if (p->upstream->read->kq_errno) {  /* kqueue在eof时附带内核错误码(如ECONNRESET) */
                    p->upstream->read->error = 1;  /* 事件层置错误标志 */
                    p->upstream_error = 1;  /* 管道层置上游出错 */
                    p->upstream_eof = 0;  /* 出错不算正常eof，撤销eof标志 */

                    ngx_log_error(NGX_LOG_ERR, p->log,  /* 记录错误日志 */
                                  p->upstream->read->kq_errno,  /* 附带内核错误码 */
                                  "readv() failed");  /* 提示该错误本应出现在readv中 */
                }  /* 错误码判断结束 */
#endif  /* HAVE_KQUEUE条件编译结束 */

                break;  /* eof/错误处理完毕，退出读循环 */
            }  /* eof检测分支结束 */

            if (p->free_raw_bufs) {  /* 回收链上有空闲raw buf可复用 */

                /* use the free bufs if they exist */

                chain = p->free_raw_bufs;  /* 取空闲buf链作为readv散布目标 */
                if (p->single_buf) {  /* 单buf模式(如限速场景，同时只允许一个buf在途) */
                    p->free_raw_bufs = p->free_raw_bufs->next;  /* 空闲链头前移一格 */
                    chain->next = NULL;  /* 摘出的节点与后续断开 */
                } else {  /* 多buf模式 */
                    p->free_raw_bufs = NULL;  /* 整条空闲链全部取走 */
                }  /* 单/多buf模式判断结束 */

            } else if (p->allocated < p->bufs.num) {  /* 无空闲buf但分配数未超bufs.num上限 */

                /* allocate a new buf if it's still allowed */

                if (!(b = ngx_create_temp_buf(p->pool, p->bufs.size))) {  /* 从内存池新分配bufs.size大小的内存buf */
                    return NGX_ABORT;  /* 内存池分配失败 */
                }  /* 分配判断结束 */

                p->allocated++;  /* 已分配计数加一，作为内存流控的依据 */

                ngx_alloc_link_and_set_buf(tl, b, p->pool, NGX_ABORT);  /* 为新buf分配链节点并挂接(失败返回ABORT) */
                chain = tl;  /* 本次readv只散布到这一个新buf */

            } else if (!p->cachable && p->downstream->write->ready) {  /* 数据无需缓存留存且下游socket当前可写 */

                /*
                 * if the bufs are not needed to be saved in a cache and
                 * a downstream is ready then write the bufs to a downstream
                 */

                p->upstream_blocked = 1;  /* 置"上游被下游反压"：转向发下游以腾buf */

                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                               "pipe downstream ready");  /* 打印"下游可写"提示 */

                break;  /* 退出读循环，让主循环先写下游再回来读 */

            } else if (p->cachable  /* 响应需整体缓存(如代理缓存场景) */
                       || p->temp_file->offset < p->max_temp_file_size)  /* 或临时文件尚未达上限：允许落盘腾内存 */
            {  /* 落盘腾内存分支 */

                /*
                 * if it's allowed then save some bufs from r->in
                 * to a temporary file, and add them to a r->out chain
                 */

                rc = ngx_event_pipe_write_chain_to_temp_file(p);  /* 把p->in积压pwrite进临时文件，回收其内存buf */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                               "pipe temp offset: %d", p->temp_file->offset);  /* 打印落盘后的文件偏移 */

                if (rc == NGX_AGAIN) {  /* 本次未全部落盘(受单次写量/文件上限约束) */
                    if (ngx_event_flags & NGX_USE_LEVEL_EVENT  /* 条件1：事件机制为电平触发(select/poll) */
                        && p->upstream->read->active  /* 条件2：上游读事件仍注册在监听集中 */
                        && p->upstream->read->ready)  /* 条件3：内核接收队列仍有数据未取 */
                    {  /* 三条件同时成立则需摘除读事件 */
                        if (ngx_del_event(p->upstream->read, NGX_READ_EVENT, 0)  /* 主动摘除读事件：电平模式下不取空内核队列会持续触发 */
                                                                  == NGX_ERROR)  /* 摘除调用的出错判断 */
                        {  /* 摘除失败分支开始 */
                            return NGX_ABORT;  /* 摘事件失败属致命错误 */
                        }  /* 摘除失败分支结束 */
                    }  /* 摘除条件判断结束 */
                }  /* AGAIN分支结束 */

                if (rc != NGX_OK) {  /* 除AGAIN之外的其他错误 */
                    return rc;  /* 直接向上传递错误码 */
                }  /* 错误码判断结束 */

                chain = p->free_raw_bufs;  /* 落盘腾出的raw buf链供本轮readv复用 */
                if (p->single_buf) {  /* 单buf模式 */
                    p->free_raw_bufs = p->free_raw_bufs->next;  /* 空闲链头前移 */
                    chain->next = NULL;  /* 断开摘出节点 */
                } else {  /* 多buf模式 */
                    p->free_raw_bufs = NULL;  /* 整链取走 */
                }  /* 单/多buf模式判断结束 */

            } else {  /* 既不能直发也不能落盘：内存与磁盘缓冲全满 */

                /* if there're no bufs to read in then disable a level event */

                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                               "no pipe bufs to read in");  /* 打印"无buf可用"提示 */
    
                break;  /* 数据滞留内核接收队列：TCP接收窗口收缩，自动抑制上游发送速率 */
            }  /* buf来源分支结束 */

            n = ngx_recv_chain(p->upstream, chain);  /* readv聚集读：一次系统调用把内核队列数据散布到chain各buf */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                           "pipe recv chain: %d", n);  /* 打印readv实际读取的字节数 */

            if (p->free_raw_bufs) {  /* 复用后仍有剩余空闲buf */
                chain->next = p->free_raw_bufs;  /* 把用过的链接回剩余空闲链之前 */
            }  /* 接回判断结束 */
            p->free_raw_bufs = chain;  /* 未装满的buf整体归还空闲链(半满状态保留供下轮续读) */

            if (n == NGX_ERROR) {  /* readv出错(如ECONNRESET连接被对端重置) */
                p->upstream_error = 1;  /* 管道层标记上游出错 */
                return NGX_ERROR;  /* 立即返回错误 */
            }  /* 错误判断结束 */

            if (n == NGX_AGAIN) {  /* 非阻塞socket内核队列已取空(EAGAIN) */
                if (p->single_buf) {  /* 单buf模式 */
                    ngx_event_pipe_remove_shadow_links(chain->buf);  /* 清理该buf的shadow链，恢复独立可复用 */
                }  /* 单buf判断结束 */

                break;  /* 挂起：等内核再次收到数据触发读事件 */
            }  /* AGAIN判断结束 */

            p->read = 1;  /* 本轮实际读到了数据 */

            if (n == 0) {  /* readv返回0：对端发送FIN，读方向关闭 */
                p->upstream_eof = 1;  /* 置管道eof标志 */
                break;  /* 退出读循环 */
            }  /* eof判断结束 */
        }  /* 预读/正常读分支结束 */

        p->read_length += n;  /* 累计已从上游读取的总字节数(响应体长度统计) */
        cl = chain;  /* 从链头开始按readv实际填充量逐buf分账 */

        while (cl && n > 0) {  /* 逐buf消化n字节，n减到0即止 */

            ngx_event_pipe_remove_shadow_links(cl->buf);  /* 该buf即将被填充提交，先解除旧shadow派生链 */

            size = cl->buf->end - cl->buf->last;  /* 当前buf的剩余可写容量 */

            if (n >= size) {  /* 剩余数据足以装满该buf整块容量 */
                cl->buf->last = cl->buf->end;  /* 写指针直接置到buf末尾(填满) */

                /* STUB */ cl->buf->num = p->num++;  /* 调试用buf序号(临时代码)：递增编号 */

                if (p->input_filter(p, cl->buf) == NGX_ERROR) {  /* 满buf交输入过滤器(默认copy:建shadow镜像挂入p->in) */
                    return NGX_ABORT;  /* 过滤器失败(如内存分配失败) */
                }  /* 过滤器判断结束 */

                n -= size;  /* 扣除本buf消耗的字节数 */
                cl = cl->next;  /* 前进到下一buf */

            } else {  /* 剩余数据不足以填满该buf */
                cl->buf->last += n;  /* 写指针只前进n：半满buf留给下轮readv续填 */
                n = 0;  /* 数据分账完毕 */
            }  /* 装满与否判断结束 */
        }  /* 分账循环结束 */

        p->free_raw_bufs = cl;  /* 剩余未动用的buf(可能含半满)作为新的空闲链 */
    }  /* 读循环结束 */

#if (NGX_DEBUG)  /* 仅调试编译：转储各buf链的当前状态 */

    if (p->in || p->busy || p->free_raw_bufs) {  /* 任一链非空才打印标题 */
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0, "pipe buf");  /* 打印"pipe buf"分隔标题 */
    }  /* 判断结束 */

    for (cl = p->in; cl; cl = cl->next) {  /* 遍历p->in：已过滤待发下游的buf链 */
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏：带3个参数 */
                       "pipe buf in " PTR_FMT ", pos " PTR_FMT ", size: %d",  /* 格式串：首址/读位置/有效长度 */
                       cl->buf->start, cl->buf->pos,  /* 实参：buf起始地址与当前读指针 */
                       cl->buf->last - cl->buf->pos);  /* 实参：有效数据字节数 */
    }  /* p->in遍历结束 */

    for (cl = p->busy; cl; cl = cl->next) {  /* 遍历p->busy：已交底层未确认的buf链 */
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏：带3个参数 */
                       "pipe buf busy " PTR_FMT ", pos " PTR_FMT ", size: %d",  /* 格式串：首址/读位置/有效长度 */
                       cl->buf->start, cl->buf->pos,  /* 实参：buf起始地址与当前读指针 */
                       cl->buf->last - cl->buf->pos);  /* 实参：有效数据字节数 */
    }  /* p->busy遍历结束 */

    for (cl = p->free_raw_bufs; cl; cl = cl->next) {  /* 遍历free_raw_bufs：可复用的空闲raw buf链 */
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏：带3个参数 */
                       "pipe buf free " PTR_FMT ", last " PTR_FMT ", size: %d",  /* 格式串：首址/写位置/剩余容量 */
                       cl->buf->start, cl->buf->last,  /* 实参：buf起始地址与当前写指针 */
                       cl->buf->end - cl->buf->last);  /* 实参：剩余可写字节数 */
    }  /* free_raw_bufs遍历结束 */

#endif  /* NGX_DEBUG条件编译结束 */

    if ((p->upstream_eof || p->upstream_error) && p->free_raw_bufs) {  /* 上游已终结但空闲链还剩未过滤的数据(含尾包半满buf) */

        /* STUB */ p->free_raw_bufs->buf->num = p->num++;  /* 调试用buf序号(临时代码)：递增编号 */

        if (p->input_filter(p, p->free_raw_bufs->buf) == NGX_ERROR) {  /* 把最后一段尾包数据也送入过滤器 */
            return NGX_ABORT;  /* 过滤器失败 */
        }  /* 过滤器判断结束 */

        p->free_raw_bufs = p->free_raw_bufs->next;  /* 消费掉队头buf，链前移 */

        if (p->free_bufs) {  /* 上层允许直接释放buf内存(无需保留复用) */
            for (cl = p->free_raw_bufs; cl; cl = cl->next) {  /* 遍历剩余空闲buf */
                ngx_pfree(p->pool, cl->buf->start);   /* 把buf内存块归还内存池 */
            }  /* 释放循环结束 */
        }  /* free_bufs判断结束 */
    }  /* 尾包处理结束 */

    if (p->cachable && p->in) {  /* 整体缓存模式且仍有内存积压 */
        if (ngx_event_pipe_write_chain_to_temp_file(p) == NGX_ABORT) {  /* 读阶段收尾：把p->in全部落盘，保证数据不丢 */
            return NGX_ABORT;  /* 落盘失败 */
        }  /* 落盘判断结束 */
    }  /* cachable判断结束 */

    return NGX_OK;  /* 读上游阶段完成 */
}  /* 函数结束 */


/*
 * ngx_event_pipe_write_to_downstream()：把内存/文件buf链经输出过滤器发给下游。
 * 上游已终结时冲刷p->out与p->in全部残留；正常路径受两级流控约束：
 * 下游写事件就绪才发送，且busy链(已交内核未确认)总量不超过busy_size。
 * 发送后ngx_chain_update_chains按tag把发完buf分拣到free、未发完留busy，
 * 并回收shadow raw buf(数据已下发的raw内存buf复位后供上游读取复用)。
 */
ngx_int_t ngx_event_pipe_write_to_downstream(ngx_event_pipe_t *p)  /* p=管道上下文 */
{  /* 函数体开始 */
    size_t        bsize;  /* busy链总字节数(已下发但未确认回收的量) */
    ngx_uint_t    flush;  /* 是否需强制冲刷的标志 */
    ngx_buf_t    *b;  /* shadow raw buf工作指针 */
    ngx_chain_t  *out, **ll, *cl, *tl;  /* 本轮待发链; 尾插指针; 工作节点; 新分配节点 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                   "pipe write downstream: %d", p->downstream->write->ready);  /* 打印下游写事件是否就绪 */

    for ( ;; ) {  /* 发送循环：每轮=凑一批buf→发送→分拣回收，直至无事可做 */
        if (p->downstream_error) {  /* 下游已出错(客户端异常断开/发送失败) */
            return ngx_event_pipe_drain_chains(p);  /* 丢弃全部积压数据，把buf回收干净后返回 */
        }  /* 出错判断结束 */

        if (p->upstream_eof || p->upstream_error || p->upstream_done) {  /* 上游已终结：进入收尾冲刷阶段 */

            /* pass the p->out and p->in chains to the output filter */

            if (p->out) {  /* 文件buf链(临时文件中的数据)还有残留 */
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                               "pipe write downstream flush out");  /* 打印冲刷p->out */

                if (p->output_filter(p->output_ctx, p->out) == NGX_ERROR) {  /* 整条文件链交输出过滤器(writev/sendfile)发送 */
                    p->downstream_error = 1;  /* 发送失败置下游错误标志 */
                    return ngx_event_pipe_drain_chains(p);  /* 回收全部buf后返回 */
                }  /* 发送判断结束 */

                p->out = NULL;  /* 文件链已全部发出 */
            }  /* p->out冲刷结束 */

            if (p->in) {  /* 内存buf链还有残留 */
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                               "pipe write downstream flush in");  /* 打印冲刷p->in */

                if (p->output_filter(p->output_ctx, p->in) == NGX_ERROR) {  /* 内存链交输出过滤器发送 */
                    p->downstream_error = 1;  /* 发送失败置下游错误标志 */
                    return ngx_event_pipe_drain_chains(p);  /* 回收全部buf后返回 */
                }  /* 发送判断结束 */

                p->in = NULL;  /* 内存链已全部发出 */
            }  /* p->in冲刷结束 */

            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                           "pipe write downstream done");  /* 打印下游发送完成 */

            /* TODO: free unused bufs */

            p->downstream_done = 1;  /* 置下游完成标志：管道传输结束 */
            break;  /* 退出发送循环 */
        }  /* 上游终结判断结束 */

        if (!p->downstream->write->ready) {  /* 下游socket发送缓冲区不可写(对端接收窗口满/网络慢) */
            break;  /* 等EPOLLOUT可写事件再来发送：对下游的应用层流控 */
        }  /* 可写判断结束 */

        /* bsize is the size of the busy bufs */

        bsize = 0;  /* busy累计器清零 */

        for (cl = p->busy; cl; cl = cl->next) {  /* 遍历busy链：已交底层但尚未确认发完的buf */
            bsize += cl->buf->end - cl->buf->start;  /* 按buf整块容量累加(部分发送的也全额计入占用) */
        }  /* busy统计循环结束 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                       "pipe write busy: " SIZE_T_FMT, bsize);  /* 打印busy总字节数 */

        out = NULL;  /* 本轮待发链清空 */
        ll = NULL;  /* 尾插指针清空 */
        flush = 0;  /* 冲刷标志清零 */

        for ( ;; ) {  /* 凑批循环：从p->out/p->in摘buf组链，直到预算用尽或取空 */
            if (p->out) {  /* 优先发文件buf(数据在磁盘页缓存，可sendfile零拷贝) */
                cl = p->out;  /* 取文件链队头 */

                if (bsize + ngx_buf_size(cl->buf) > p->busy_size) {  /* 加入后未确认量将超busy_size上限 */
                    flush = 1;  /* 置冲刷标志：预算满也需发一次已凑部分 */
                    break;  /* 停止凑批 */
                }  /* 预算判断结束 */

                p->out = p->out->next;  /* 文件链前移 */
                ngx_event_pipe_free_shadow_raw_buf(&p->free_raw_bufs,  /* 该buf数据已落盘，其背后的raw内存buf */
                                                   cl->buf);  /* 可提前从free链摘出复用 */

            } else if (!p->cachable && p->in) {  /* 非缓存模式才可直发内存buf(缓存模式必须先落盘留存) */
                cl = p->in;  /* 取内存链队头 */

                if (bsize + ngx_buf_size(cl->buf) > p->busy_size) {  /* 同样受busy_size流控约束 */
                    flush = 1;  /* 置冲刷标志 */
                    break;  /* 停止凑批 */
                }  /* 预算判断结束 */

                p->in = p->in->next;  /* 内存链前移 */

            } else {  /* 两条链都已取空 */
                break;  /* 凑批完成 */
            }  /* 取buf分支结束 */

            bsize += ngx_buf_size(cl->buf);  /* 该buf计入本轮已凑批字节数 */
            cl->next = NULL;  /* 摘下节点与原链断开 */
            ngx_chain_add_link(out, ll, cl);  /* 尾插进本轮待发链out */
        }  /* 凑批循环结束 */

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                       "pipe write: out:" PTR_FMT ", f:%d", out, flush);  /* 打印凑好的链与冲刷标志 */

        if (out == NULL && !flush) {  /* 没凑到数据也无需冲刷 */
            break;  /* 无事可做，退出外层发送循环 */
        }  /* 空链判断结束 */

        if (p->output_filter(p->output_ctx, out) == NGX_ERROR) {  /* 交输出过滤器：writev/sendfile聚集发送给客户端 */
            p->downstream_error = 1;  /* 发送失败置下游错误标志 */
            return ngx_event_pipe_drain_chains(p);  /* 丢弃积压并回收全部buf */
        }  /* 发送判断结束 */

        ngx_chain_update_chains(&p->free, &p->busy, &out, p->tag);  /* 按tag分拣：发完进free，部分发送留busy等续传 */

        for (cl = p->free; cl; cl = cl->next) {  /* 遍历刚回收进free的buf做善后 */

            if (cl->buf->temp_file) {  /* 该buf的数据存于临时文件 */
                if (p->cachable || !p->cyclic_temp_file) {  /* 缓存模式或不允许循环复用临时文件 */
                    continue;  /* 保留文件内容，offset不归零 */
                }  /* 保留判断结束 */

                /* reset p->temp_offset if all bufs had been sent */

                if (cl->buf->file_last == p->temp_file->offset) {  /* 该buf恰是文件末尾：临时数据已全部发完 */
                    p->temp_file->offset = 0;  /* 文件偏移归零，从头复用临时文件(cyclic省磁盘空间) */
                }  /* 归零判断结束 */
            }  /* 临时文件buf判断结束 */

            /* TODO: free buf if p->free_bufs && upstream done */

            /* add the free shadow raw buf to p->free_raw_bufs */

            if (cl->buf->last_shadow) {  /* 该buf是某raw buf的最后一个shadow镜像 */
                b = cl->buf->shadow;  /* 沿shadow指针找到原始raw buf */
                b->pos = b->last = b->start;  /* 读写指针复位：raw buf变空可重新填充 */
                b->shadow = NULL;  /* 断开shadow关联 */
                ngx_alloc_link_and_set_buf(tl, b, p->pool, NGX_ABORT);  /* 为raw buf分配链节点(失败返回ABORT) */
                ngx_event_pipe_add_free_buf(&p->free_raw_bufs, tl);  /* 归还空闲raw链，供上游读取复用 */

                cl->buf->last_shadow = 0;  /* 清末尾镜像标志(已处理) */
            }  /* 末尾镜像判断结束 */

            cl->buf->shadow = NULL;  /* 彻底解除该buf自身的shadow指针 */
        }  /* free善后循环结束 */
    }  /* 发送循环结束 */

    return NGX_OK;  /* 写下游阶段完成 */
}  /* 函数结束 */


/*
 * ngx_event_pipe_write_chain_to_temp_file()：把p->in内存积压刷进临时文件腾内存。
 * 按temp_file_write_size(单次写量)与max_temp_file_size(文件上限)截取一段链，
 * ngx_write_chain_to_temp_file(聚集写/pwrite语义)落盘；随后把各buf转成
 * "文件buf"：记录file_pos/file_last、in_file=1，挂入p->out供sendfile
 * 零拷贝直发；腾出的raw buf复位后归还free_raw_bufs。temp_file->offset
 * 是文件内追加写偏移，cyclic模式下数据发完可归零从头复用文件空间。
 */
static ngx_int_t ngx_event_pipe_write_chain_to_temp_file(ngx_event_pipe_t *p)  /* p=管道上下文 */
{  /* 函数体开始 */
    ssize_t       size, bsize;  /* size=本次写入总字节数; bsize=单buf有效字节数 */
    ngx_buf_t    *b;  /* 工作buf指针 */
    ngx_chain_t  *cl, *tl, *next, *out, **ll, **last_free, fl;  /* 工作节点; fl=栈上伪节点(衔接buf_to_file) */

    if (p->buf_to_file) {  /* 存在"内存→文件"衔接buf(前次部分写入的收尾残留) */
        fl.buf = p->buf_to_file;  /* 栈上伪节点指向该衔接buf */
        fl.next = p->in;  /* 并把它接在p->in之前构成完整待写链 */
        out = &fl;  /* 待写链从栈上伪节点开始 */

    } else {  /* 无衔接buf */
        out = p->in;  /* 待写链就是p->in整条 */
    }  /* 衔接判断结束 */

    if (!p->cachable) {  /* 非整体缓存模式：只需截取有限一段落盘 */

        size = 0;  /* 累计大小清零 */
        cl = out;  /* 从链头开始累加 */
        ll = NULL;  /* 截断点指针清空 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏 */
                       "pipe offset: %d", p->temp_file->offset);  /* 打印临时文件当前追加偏移 */

        do {  /* 至少处理一个buf的计数循环 */
            bsize = cl->buf->last - cl->buf->pos;  /* 当前buf有效数据字节数 */

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, p->log, 0,  /* 调试日志宏：带3个参数 */
                           "pipe buf " PTR_FMT ", pos " PTR_FMT ", size: %d",  /* 格式串：首址/读位置/长度 */
                           cl->buf->start, cl->buf->pos, bsize);  /* 实参：buf首址/读指针/有效字节数 */

            if ((size + bsize > p->temp_file_write_size)  /* 累计将超单次落盘写入量 */
               || (p->temp_file->offset + size + bsize > p->max_temp_file_size))  /* 或文件将超max_temp_file_size上限 */
            {  /* 超限判断 */
                break;  /* 在此buf前截断：其余留在内存 */
            }  /* 超限判断结束 */

            size += bsize;  /* 该buf计入本次落盘量 */
            ll = &cl->next;  /* 记录截断点所在的next指针域 */
            cl = cl->next;  /* 前进到下一buf */

        } while (cl);  /* 直到链尾 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0, "size: %d", size);  /* 打印本次落盘总字节数 */

        if (cl) {  /* 链有剩余(发生了截断) */
           p->in = cl;  /* 剩余部分仍作为p->in内存积压 */
           *ll = NULL;  /* 在截断点把链断开 */

        } else {  /* 全部落盘 */
           p->in = NULL;  /* p->in清空 */
           p->last_in = &p->in;  /* 尾插指针复位指向链头(链已空) */
        }  /* 截断判断结束 */

    } else {  /* cachable模式：p->in整条全部落盘 */
        p->in = NULL;  /* 清空内存积压链 */
        p->last_in = &p->in;  /* 尾插指针复位 */
    }  /* cachable判断结束 */

    if (ngx_write_chain_to_temp_file(p->temp_file, out) == NGX_ERROR) {  /* 聚集写(pwrite语义)把out链写入临时文件 */
        return NGX_ABORT;  /* 文件写入失败 */
    }  /* 写文件判断结束 */

    for (last_free = &p->free_raw_bufs;  /* 从空闲raw链头指针域开始 */
         *last_free != NULL;  /* 未到链尾则继续 */
         last_free = &(*last_free)->next)  /* 前移到下一节点的next域 */
    {  /* 定位链尾循环 */
        /* void */
    }  /* 定位循环结束 */

    if (p->buf_to_file) {  /* 有衔接buf的情形 */
        p->temp_file->offset = p->buf_to_file->last - p->buf_to_file->pos;  /* 衔接buf数据已随前次写入处理，按其长度对齐偏移 */
        p->buf_to_file = NULL;  /* 衔接buf使命完成 */
        out = out->next;  /* 跳过伪节点后的衔接buf本身 */
    }  /* 衔接处理结束 */

    for (cl = out; cl; cl = next) {  /* 逐个把已落盘buf转换成文件buf */
        next = cl->next;  /* 先存后继(随后要断链) */
        cl->next = NULL;  /* 摘下节点 */

        b = cl->buf;  /* 取该buf */
        b->file = &p->temp_file->file;  /* 绑定临时文件对象：数据已从内存转到磁盘 */
        b->file_pos = p->temp_file->offset;  /* 文件内起始偏移=当前追加位置 */
        p->temp_file->offset += b->last - b->pos;  /* 追加偏移前移本buf数据量 */
        b->file_last = p->temp_file->offset;  /* 文件内结束偏移 */

        b->in_file = 1;  /* 标记数据在文件中：输出阶段可走sendfile零拷贝 */
        b->temp_file = 1;  /* 标记为临时文件buf(区别于正式缓存文件buf) */

        ngx_chain_add_link(p->out, p->last_out, cl);  /* 尾插进p->out文件buf链，等待发往下游 */

        if (b->last_shadow) {  /* 该buf是某raw buf的最后一个shadow镜像 */
            b->shadow->last = b->shadow->pos = b->shadow->start;  /* 复位原始raw buf读写指针(数据已安全落盘) */
            ngx_alloc_link_and_set_buf(tl, b->shadow, p->pool, NGX_ABORT);  /* 为raw buf分配链节点(失败返回ABORT) */
            *last_free = tl;  /* 尾接进free_raw_bufs链 */
            last_free = &tl->next;  /* 尾插指针前移 */
        }  /* shadow回收判断结束 */
    }  /* 文件buf转换循环结束 */

    return NGX_OK;  /* 落盘腾内存完成 */
}  /* 函数结束 */


/* the copy input filter */

/*
 * ngx_event_pipe_copy_input_filter()：默认输入过滤器(拷贝式但零数据拷贝)。
 * 不移动数据，而是为已填满的raw buf创建一个shadow"镜像buf"(共享同一块
 * 内存)挂入p->in待发链；原raw buf留在free_raw_bufs，待下游把镜像发完后
 * 再复位复用——buf级分账：读侧与写侧各自独立推进，互不阻塞。
 */
ngx_int_t ngx_event_pipe_copy_input_filter(ngx_event_pipe_t *p, ngx_buf_t *buf)  /* p=管道上下文; buf=已填满的raw buf */
{  /* 函数体开始 */
    ngx_buf_t    *b;  /* 新建的shadow镜像buf */
    ngx_chain_t  *cl;  /* 链节点 */

    if (buf->pos == buf->last) {  /* 空buf(readv一个字节都没装进该buf) */
        return NGX_OK;  /* 无需过滤，直接跳过 */
    }  /* 空buf判断结束 */

    if (p->free) {  /* free链上有已发完、可复用的buf结构 */
        b = p->free->buf;  /* 取其buf结构复用 */
        p->free = p->free->next;  /* free链前移 */

    } else {  /* 无可复用buf结构 */
        if (!(b = ngx_alloc_buf(p->pool))) {  /* 从内存池新分配一个buf结构体 */
            return NGX_ERROR;  /* 分配失败 */
        }  /* 分配判断结束 */
    }  /* 复用判断结束 */

    ngx_memcpy(b, buf, sizeof(ngx_buf_t));  /* 整体复制buf结构：镜像与原buf指向同一块内存(零数据拷贝) */
    b->shadow = buf;  /* 镜像记录原buf：发完后靠它复位原buf */
    b->tag = p->tag;  /* 打上管道tag，便于update_chains按主分拣归属 */
    b->last_shadow = 1;  /* 标记为该raw buf的最后一个镜像(发完才允许回收raw buf) */
    b->recycled = 1;  /* 标记可回收：提示底层发送完须尽快归还 */
    buf->shadow = b;  /* 原buf反向指向镜像，构成双向shadow环 */

    ngx_alloc_link_and_set_buf(cl, b, p->pool, NGX_ERROR);  /* 为镜像buf分配链节点(失败返回ERROR) */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0, "buf #%d", b->num);  /* 调试日志：打印buf序号 */

    ngx_chain_add_link(p->in, p->last_in, cl);  /* 镜像节点尾插进p->in待发链 */

    return NGX_OK;  /* 过滤完成 */
}  /* 函数结束 */


/*
 * ngx_event_pipe_remove_shadow_links()：解除buf的整条shadow派生链。
 * 该raw buf即将被重新填充(readv)或独立使用前，把它的所有shadow镜像buf
 * 的in_file/temp_file/flush等标志清零并逐个断链，防止上一轮响应的
 * 旧状态(如"数据在文件中")污染新一轮数据。
 */
ngx_inline static void ngx_event_pipe_remove_shadow_links(ngx_buf_t *buf)  /* buf=即将复用的raw buf */
{  /* 函数体开始 */
    ngx_buf_t  *b, *next;  /* b=当前遍历的shadow镜像; next=暂存下一个 */

    if (buf->shadow == NULL) {  /* 该buf没有shadow派生链 */
        return;  /* 无需处理 */
    }  /* 无shadow判断结束 */

    b = buf->shadow;  /* 从第一个shadow镜像开始 */

    while (!b->last_shadow) {  /* 逐个处理中间镜像(last_shadow是链终止标志) */
        next = b->shadow;  /* 先记下一个镜像 */

        b->in_file = 0;  /* 清"数据在文件"标志 */
        b->temp_file = 0;  /* 清"临时文件"标志 */
        b->flush = 0;  /* 清冲刷标志 */
        b->zerocopy_busy = 0;  /* 清零拷贝在途标志 */

        b->shadow = NULL;  /* 断开该镜像的shadow指针 */
        b = next;  /* 前进到下一个 */
    }  /* 中间镜像循环结束 */

    b->in_file = 0;  /* 末尾镜像同样清文件标志 */
    b->temp_file = 0;  /* 清临时文件标志 */
    b->flush = 0;  /* 清冲刷标志 */
    b->zerocopy_busy = 0;  /* 清零拷贝在途标志 */
    b->last_shadow = 0;  /* 清末尾标志 */

    b->shadow = NULL;  /* 末尾镜像断链 */

    buf->shadow = NULL;  /* 原buf的shadow头指针清空：恢复独立自由身 */
}  /* 函数结束 */


/*
 * ngx_event_pipe_free_shadow_raw_buf()：文件buf发往下游前提前回收raw内存buf。
 * 沿shadow环找到last_shadow的原始raw buf，若它还在free链中则从链上摘除——
 * 因数据已安全落盘，raw内存不必等下游发完即可复用，提高buf周转率。
 */
ngx_inline static void ngx_event_pipe_free_shadow_raw_buf(ngx_chain_t **free,  /* free=空闲raw buf链头指针域 */
                                                          ngx_buf_t *buf)  /* buf=即将发送的文件buf */
{  /* 函数体开始 */
    ngx_buf_t    *s;  /* s=shadow环中带last_shadow的原始raw buf */
    ngx_chain_t  *cl, **ll;  /* cl=遍历节点; ll=前驱节点的next域指针 */

    if (buf->shadow == NULL) {  /* 该buf无shadow关联 */
        return;  /* 直接返回 */
    }  /* 无shadow判断结束 */

    for (s = buf->shadow; !s->last_shadow; s = s->shadow) { /* void */ }  /* 沿shadow环走到末尾的原始raw buf */

    ll = free;  /* ll初始指向链头指针域 */

    for (cl = *free ; cl; cl = cl->next) {  /* 在free链中查找该raw buf */
        if (cl->buf == s) {  /* 找到了目标raw buf */
            *ll = cl->next;  /* 摘除该节点(前驱跨接后继) */
            break;  /* 摘除完成 */
        }  /* 命中判断结束 */

        if (cl->buf->shadow) {  /* 遇到仍带shadow的buf：其数据尚未完全消化 */
            break;  /* 放弃摘除(越过它摘取会破坏复用次序) */
        }  /* shadow检查结束 */

        ll = &cl->next;  /* 记录前驱，继续向后找 */
    }  /* 查找循环结束 */
}  /* 函数结束 */


/*
 * ngx_event_pipe_add_free_buf()：把回收的raw buf归还空闲链。
 * 插入位置有讲究：若链头是半满buf(pos!=last，可续读)则插在其后，
 * 保证半满buf仍排链头优先被readv续填；若链头是全空buf则插到最前。
 * 维持"先半满后全空"的复用次序，减少小buf浪费。
 */
ngx_inline static void ngx_event_pipe_add_free_buf(ngx_chain_t **chain,  /* chain=空闲链头指针域 */
                                                   ngx_chain_t *cl)  /* cl=待归还的链节点 */
{  /* 函数体开始 */
    if (*chain == NULL) {  /* 空闲链为空 */
        *chain = cl;  /* 归还节点直接成为链头 */
        return;  /* 完成 */
    }  /* 空链判断结束 */

    if ((*chain)->buf->pos != (*chain)->buf->last) {  /* 链头buf是半满的(可继续readv续填) */
        cl->next = (*chain)->next;  /* 新节点插在链头之后 */
        (*chain)->next = cl;  /* 半满buf保持链头优先复用 */

    } else {  /* 链头buf是全空的 */
        cl->next = (*chain);  /* 新节点插到最前面 */
        (*chain) = cl;  /* 成为新链头 */
    }  /* 插入位置判断结束 */
}  /* 函数结束 */


/*
 * ngx_event_pipe_drain_chains()：下游出错后的"排水"例程。
 * 依次取空busy/out/in三条积压链并丢弃数据：带last_shadow的buf将其
 * 原始raw buf复位归还free_raw_bufs，普通buf头还回p->free复用链，
 * 确保连接异常关闭时buf与链节点不泄漏。
 */
static ngx_int_t ngx_event_pipe_drain_chains(ngx_event_pipe_t *p)  /* p=管道上下文 */
{  /* 函数体开始 */
    ngx_buf_t    *b;  /* raw buf工作指针 */
    ngx_chain_t  *cl, *tl;  /* cl=当前处理的链; tl=暂存后继节点 */

    for ( ;; ) {  /* 外层循环：排完一条链再取下一条 */
        if (p->busy) {  /* 优先排busy链(已交底层未确认的buf) */
            cl = p->busy;  /* 取走整条busy链 */
            p->busy = NULL;  /* busy指针清空 */

        } else if (p->out) {  /* 其次排文件buf链 */
            cl = p->out;  /* 取走整条out链 */
            p->out = NULL;  /* out指针清空 */

        } else if (p->in) {  /* 再次排内存buf链 */
            cl = p->in;  /* 取走整条in链 */
            p->in = NULL;  /* in指针清空 */

        } else {  /* 三条链全空：积压数据已全部丢弃 */
            return NGX_OK;  /* 排水完成 */
        }  /* 取链分支结束 */

        while (cl) {  /* 遍历本条链的每个节点 */
            if (cl->buf->last_shadow) {  /* 该buf是某raw buf的末尾镜像 */
                b = cl->buf->shadow;  /* 找到原始raw buf */
                b->pos = b->last = b->start;  /* 读写指针复位：raw buf变空 */
                b->shadow = NULL;  /* 断开shadow关联 */
                ngx_alloc_link_and_set_buf(tl, b, p->pool, NGX_ABORT);  /* 为raw buf分配链节点(失败返回ABORT) */
                ngx_event_pipe_add_free_buf(&p->free_raw_bufs, tl);  /* 归还空闲raw链供复用 */

                cl->buf->last_shadow = 0;  /* 清末尾镜像标志 */
            }  /* 末尾镜像判断结束 */

            cl->buf->shadow = NULL;  /* 解除该buf自身的shadow指针 */
            tl = cl->next;  /* 暂存后继节点 */
            cl->next = p->free;  /* 当前节点头插进p->free复用链 */
            p->free = cl;  /* 更新free链头 */
            cl = tl;  /* 前进到后继 */
        }  /* 节点回收循环结束 */
    }  /* 排水外层循环结束 */
}  /* 函数结束 */
