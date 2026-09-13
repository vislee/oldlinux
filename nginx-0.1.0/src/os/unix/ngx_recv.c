
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置头：提供 HAVE_KQUEUE 等探测宏 */
#include <ngx_core.h>  /* nginx 核心类型：ngx_connection_t、NGX_ERROR 等定义 */
#include <ngx_event.h>  /* 事件模块头：ngx_event_t、ngx_event_flags、NGX_AGAIN 等在此定义 */


#if (HAVE_KQUEUE)  /* BSD 系平台：kqueue 事件自带 EOF/可读字节数信息，接收路径可据此短路 */

/*
 * 整体功能：非阻塞地从连接 c 上读取最多 size 字节到 buf（kqueue 版本）。
 * 先利用 kevent() 已报告的状态（available/pending_eof/kq_errno）做判断，
 * 避免注定返回 EAGAIN 的无效 recv() 系统调用。返回值三态：
 * n>0 实读字节数（内核从套接字接收缓冲区 sk_receive_queue 拷给用户）、
 * n==0 对端已发 FIN（TCP 四次挥手的第一次）、NGX_AGAIN 暂无数据、
 * NGX_ERROR 真实错误（如对端 RST 导致的 ECONNRESET）。
 */
ssize_t ngx_unix_recv(ngx_connection_t *c, u_char *buf, size_t size)  /* c 携带 fd 与读写事件，buf/size 是用户缓冲区 */
{
    ssize_t       n;  /* recv() 的返回值：本次实际拷贝的字节数，出错为 -1 */
    ngx_err_t     err;  /* 暂存线程局部的 errno，防止被后续系统调用覆盖 */
    ngx_event_t  *rev;  /* 读事件结构：在用户态缓存内核报告的连接状态 */

    rev = c->read;  /* 取出该连接的读事件，后续所有判断都基于其缓存字段 */

    if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {  /* 当前事件机制是 kqueue 且事件附带 eof/available 附加信息 */
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志：打印 kevent 缓存的三项关键状态 */
                       "recv: eof:%d, avail:%d, err:%d",  /* 原有日志格式串 */
                       rev->pending_eof, rev->available, rev->kq_errno);  /* EOF 挂起标志、剩余可读字节数、关闭时的错误码 */

        if (rev->available == 0) {  /* kevent 报告当前内核接收队列已无可读数据 */
            if (rev->pending_eof) {  /* 且内核已检测到对端关闭（EOF 尚未被应用层消费） */
                rev->ready = 0;  /* 清除就绪标志：事件模块不再把该连接当作可读来调度 */
                rev->eof = 1;  /* 置 EOF 标志：对端已发 FIN，连接进入半关闭 */

                ngx_log_error(NGX_LOG_INFO, c->log, rev->kq_errno,  /* INFO 级记录：连接已被对端关闭 */
                              "kevent() reported about an closed connection");  /* 原有英文日志串 */

                if (rev->kq_errno) {  /* EOF 同时伴随错误码：多为对端 RST 等异常关闭 */
                    rev->error = 1;  /* 连接标记为出错，上层据此记录并关闭连接 */
                    ngx_set_socket_errno(rev->kq_errno);  /* 把 kevent 暂存的错误恢复到当前线程 errno，保证后续日志准确 */

                    if (rev->kq_errno == NGX_ECONNRESET  /* 对端发送 RST 复位连接（绕过四次挥手的异常关闭） */
                        && c->log_error == NGX_ERROR_IGNORE_ECONNRESET)  /* 且配置层面指示忽略该类错误 */
                    {
                        return 0;  /* 伪装成读到 0 字节（EOF）处理，不向上抛错 */
                    }  /* if(ECONNRESET 且配置忽略) 分支结束 */

                    return NGX_ERROR;  /* 其余错误上抛，由读事件处理器统一处理 */
                }  /* if(rev->kq_errno) 分支结束 */

                return 0;  /* 无错误的优雅关闭：按 recv 语义返回 0 */

            } else {  /* 只是暂时无数据，并非连接关闭 */
                return NGX_AGAIN;  /* 返回“稍后再试”：进程不睡眠，等 kqueue 下次通知可读 */
            }  /* if(pending_eof)-else 分支结束 */
        }  /* if(rev->available == 0) 分支结束 */
    }  /* if(NGX_HAVE_KQUEUE_EVENT) 分支结束 */

    do {  /* do-while：仅当 recv 被 EINTR（信号打断）时才重试 */
        n = recv(c->fd, buf, size, 0);  /* 核心调用：内核把 sk_receive_queue 上就绪数据拷入用户 buf；非阻塞下无数据立即返回 -1/EAGAIN */

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志：fd、实读字节数、请求字节数 */
                       "recv: fd:%d %d of %d", c->fd, n, size);  /* 原有日志格式串，便于观察每次系统调用的吞吐 */

        if (n >= 0) {  /* n>0 读到数据、n==0 对端关闭，均属成功路径 */
            if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {  /* kqueue 环境：需要把 available 计数与内核同步 */
                rev->available -= n;  /* 从 kevent 报告的剩余可读字节数中扣除本次已读的量 */

                /*
                 * rev->available can be negative here because some additional
                 * bytes can be received between kevent() and recv()
                 */

                if (rev->available <= 0) {  /* kevent 报告的数据已被读完（或因新数据到达而变成负数） */
                    if (!rev->pending_eof) {  /* 若对端没有挂起的关闭 */
                        rev->ready = 0;  /* 标记不可读：再读必定 EAGAIN，等下一次事件通知 */
                    }  /* if(!rev->pending_eof) 分支结束 */

                    if (rev->available < 0) {  /* kevent 与 recv 之间对端又发来新数据，导致计数变负 */
                        rev->available = 0;  /* 钳位为 0，维持“剩余字节数非负”的不变式 */
                    }  /* if(rev->available < 0) 分支结束 */
                }  /* if(rev->available <= 0) 分支结束 */

                return n;  /* 返回实读字节数，此时用户态计数已与内核同步 */
            }  /* if(NGX_HAVE_KQUEUE_EVENT) 分支结束 */

            if ((size_t) n < size) {  /* 非 kqueue 环境：没读满请求量，说明内核接收缓冲区已被取空 */
                rev->ready = 0;  /* 清除可读标志：下次读需等待新的事件通知 */
            }  /* if(n < size) 分支结束 */

            if (n == 0) {  /* recv 返回 0：对端发送了 FIN，开始四次挥手 */
                rev->eof = 1;  /* 记录 EOF，上层读循环据此终止读取 */
            }  /* if(n == 0) 分支结束 */

            return n;  /* 成功路径统一返回实读字节数 */
        }  /* if(n >= 0) 分支结束 */

        err = ngx_socket_errno;  /* 走到这里 n==-1：立即取出 errno（它是线程局部的，下一个系统调用会覆盖它） */

        if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* EAGAIN：内核接收缓冲区暂无数据；EINTR：被信号打断，应当重试 */
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试级记录“未就绪”，属正常流程而非错误 */
                           "recv() not ready");  /* 原有日志串 */
            n = NGX_AGAIN;  /* 统一转换为 NGX_AGAIN 语义：交还事件模块，等就绪通知再来 */

        } else {  /* 其余 errno 是真实错误：如 ECONNRESET（对端 RST）、ETIMEDOUT 等 */
            n = ngx_connection_error(c, err, "recv() failed");  /* 记录错误日志（内部尊重 ignore_ECONNRESET 配置）并返回 NGX_ERROR */
            break;  /* 跳出 do-while，进入收尾处理 */
        }  /* if-else 判断结束 */

    } while (err == NGX_EINTR);  /* 只有“被信号打断”才自动重试 recv；EAGAIN 则直接返回不空转 */

    rev->ready = 0;  /* 走到收尾：说明出错或需要等待，清除可读标志 */

    if (n == NGX_ERROR){  /* 确认发生了真实错误（注意原代码 if 与左花括号之间无空格，保持原样） */
        rev->error = 1;  /* 在连接上留下出错痕迹，调用方一般会关闭该连接 */
    }  /* if(n == NGX_ERROR) 分支结束 */

    return n;  /* 把 NGX_AGAIN 或 NGX_ERROR 返回给读事件处理器 */
}  /* ngx_unix_recv 的 kqueue 版本结束 */

/* 非 kqueue 平台（Linux epoll、poll/select 等）：事件不携带剩余字节数与 EOF 信息，只能靠 recv 返回值判断 */
#else /* ! NAVE_KQUEUE */

/*
 * 整体功能：非 kqueue 平台的通用接收函数。事件通知里没有“还剩多少字节”
 * 与“对端已关闭”的信息，只能依赖 recv() 返回值三态判断：
 * >0 实读字节数、==0 对端 FIN、-1 时查 errno（EAGAIN 无数据 /
 * EINTR 信号打断 / ECONNRESET 对端复位）。
 */
ssize_t ngx_unix_recv(ngx_connection_t *c, u_char *buf, size_t size)  /* 参数含义与 kqueue 版一致 */
{
    ssize_t       n;  /* recv() 的返回值 */
    ngx_err_t     err;  /* 暂存 errno */
    ngx_event_t  *rev;  /* 读事件结构，用于维护 ready/eof/error 状态 */

    rev = c->read;  /* 取出该连接的读事件指针 */

    do {  /* do-while：仅当 recv 被 EINTR 打断时重试 */
        n = recv(c->fd, buf, size, 0);  /* 核心：内核把接收缓冲区数据拷入用户 buf；非阻塞下无数据立即返回 -1/EAGAIN */

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志：fd、实读字节数、请求字节数 */
                       "recv: fd:%d %d of %d", c->fd, n, size);  /* 原有日志格式串 */

        if (n == 0) {  /* 返回 0：对端已发送 FIN，连接优雅关闭 */
            rev->ready = 0;  /* 清除可读标志 */
            rev->eof = 1;  /* 置 EOF 标志，上层读循环据此收尾 */
            return n;  /* 把 0 返回给调用方 */

        } else if (n > 0) {  /* 读到了实际数据 */

            if ((size_t) n < size  /* 本次没把用户缓冲区读满，说明内核接收队列已被取空 */
                && !(ngx_event_flags & NGX_HAVE_GREEDY_EVENT))  /* 且事件机制不是“贪婪”模式（贪婪模式下可读状态保持，不清 ready） */
            {
                rev->ready = 0;  /* 清可读标志：再读必是 EAGAIN，等下次事件通知 */
            }  /* if(n < size && !greedy) 分支结束 */

            return n;  /* 返回实读字节数 */
        }  /* if(n==0)/else if(n>0) 判断结束 */

        err = ngx_socket_errno;  /* 走到这里 n==-1：立即取出 errno */

        if (err == NGX_EAGAIN || err == NGX_EINTR) {  /* EAGAIN：内核接收缓冲区暂无数据；EINTR：被信号打断 */
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,  /* 调试级记录“未就绪”，不当作错误 */
                           "recv() not ready");  /* 原有日志串 */
            n = NGX_AGAIN;  /* 统一转换为 NGX_AGAIN 语义 */

        } else {  /* 其余 errno 是真实错误：如 ECONNRESET（对端 RST 复位） */
            n = ngx_connection_error(c, err, "recv() failed");  /* 记录错误日志并转换为 NGX_ERROR */
            break;  /* 跳出 do-while，进入收尾处理 */
        }  /* if-else 判断结束 */

    } while (err == NGX_EINTR);  /* 只有被信号打断才自动重试 recv */

    rev->ready = 0;  /* 走到收尾：出错或需等待，清除可读标志 */

    if (n == NGX_ERROR){  /* 确认发生了真实错误 */
        rev->error = 1;  /* 在连接上标记出错 */
    }  /* if(n == NGX_ERROR) 分支结束 */

    return n;  /* 把 NGX_AGAIN 或 NGX_ERROR 返回给读事件处理器 */
}  /* ngx_unix_recv 的通用版本结束 */

#endif /* NAVE_KQUEUE */  /* 与文件开头的 #if (HAVE_KQUEUE) 配对，条件编译结束 */
