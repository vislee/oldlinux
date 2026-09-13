
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置总头：提供各平台探测宏与基础类型映射 */
#include <ngx_core.h>    /* 核心头：buf/chain/内存池/定时器等基础设施 */
#include <ngx_event.h>   /* 事件框架：写事件(delayed)、ngx_add_timer依赖于此 */
#include <ngx_http.h>    /* HTTP框架：请求/过滤器链/loc配置等定义 */


typedef struct {  /* 写过滤器的每请求上下文结构 */
    ngx_chain_t  *out;  /* 本连接暂存的待发送buf链(部分发送后的剩余部分也在此) */
} ngx_http_write_filter_ctx_t;  /* 结构名：HTTP写过滤上下文 */


static ngx_int_t ngx_http_write_filter_init(ngx_cycle_t *cycle);  /* 前置声明：模块初始化时把本过滤器挂上过滤链顶端 */


ngx_http_module_t  ngx_http_write_filter_module_ctx = {  /* HTTP模块上下文：写过滤器不解析任何指令，钩子全为NULL */
    NULL,                                  /* pre conf */  /* 解析配置前钩子：不需要 */

    NULL,                                  /* create main configuration */  /* 不需要main级配置结构 */
    NULL,                                  /* init main configuration */  /* 不需要main级初始化 */

    NULL,                                  /* create server configuration */  /* 不需要server级配置结构 */
    NULL,                                  /* merge server configuration */  /* 不需要server级合并 */

    NULL,                                  /* create location configuration */  /* 不需要location级配置结构(读core模块的) */
    NULL,                                  /* merge location configuration */  /* 不需要location级合并 */
};  /* 模块上下文定义结束 */


ngx_module_t  ngx_http_write_filter_module = {  /* 模块描述结构 */
    NGX_MODULE,  /* 模块版本/签名宏(0.1.0为固定值) */
    &ngx_http_write_filter_module_ctx,     /* module context */  /* 指向上面的HTTP模块上下文 */
    NULL,                                  /* module directives */  /* 无指令表 */
    NGX_HTTP_MODULE,                       /* module type */  /* 模块类型：HTTP */
    ngx_http_write_filter_init,            /* init module */  /* 模块初始化钩子：注册过滤器 */
    NULL                                   /* init process */  /* 无进程级初始化 */
};  /* 模块描述结束 */


/*
 * ngx_http_write_filter()：HTTP输出过滤链的最后一级。
 * 所有body过滤器的数据最终汇到这里；它持有连接级待发送buf链ctx->out：
 * 新buf追加、按postpone_output攒包、无攒包条件时一次writev/sendfile发出；
 * 部分发送(对端滑动窗口收窄/SO_SNDBUF满)后剩余链留存，等待写事件续传。
 */
ngx_int_t ngx_http_write_filter(ngx_http_request_t *r, ngx_chain_t *in)  /* r=请求, in=上游过滤器传来的新buf链 */
{  /* 函数体开始 */
    int                           last;  /* 暂存链或新链中是否已见last_buf(响应结束标志) */
    off_t                         size, flush, sent;  /* size=待发总字节; flush=强制发送临界点; sent=限速用的发送量快照 */
    ngx_chain_t                  *cl, *ln, **ll, *chain;  /* cl/ln=遍历节点; ll=尾插指针; chain=发送后剩余链 */
    ngx_connection_t             *c;  /* TCP连接对象(fd、写事件、sent计数) */
    ngx_http_core_loc_conf_t     *clcf;  /* core模块location配置(postpone_output/limit_rate等) */
    ngx_http_write_filter_ctx_t  *ctx;  /* 本过滤器的每请求上下文 */

    ctx = ngx_http_get_module_ctx(r->main ? r->main : r,  /* 取主请求的写过滤上下文：子请求共享main的out链，避免多个子请求数据交错发送 */
                                  ngx_http_write_filter_module);  /* 续上行：以模块名为键查上下文 */

    if (ctx == NULL) {  /* 本请求首次调用写过滤 */
        ngx_http_create_ctx(r, ctx, ngx_http_write_filter_module,  /* 从请求内存池分配并清零上下文(失败宏内返回NGX_ERROR) */
                            sizeof(ngx_http_write_filter_ctx_t), NGX_ERROR);  /* 续上行：结构大小与失败返回码 */
    }  /* 创建上下文判断结束 */

    size = 0;  /* 待发字节数清零 */
    flush = 0;  /* flush临界点清零：0表示链中无强制发送点 */
    last = 0;  /* 结束标志清零 */
    ll = &ctx->out;  /* 尾插指针初始指向链头指针 */

    /* find the size, the flush point and the last link of the saved chain */

    for (cl = ctx->out; cl; cl = cl->next) {  /* 遍历已暂存链：统计总量、找强制点、记下表尾 */
        ll = &cl->next;  /* 记录最后一个节点的next地址(追加用) */

        size += ngx_buf_size(cl->buf);  /* 累加每个buf的有效字节数 */

        if (cl->buf->flush || cl->buf->recycled) {  /* flush=立即发出；recycled=buf将被复用，延迟发送会覆写未发数据 */
            flush = size;  /* 记录到此为止的累计量：本次发送不得少于该值 */
        }  /* flush标志判断结束 */

        if (cl->buf->last_buf) {  /* 暂存链里已有响应结束buf */
            last = 1;  /* 置结束标志 */
        }  /* last_buf判断结束 */
    }  /* 暂存链遍历结束 */

    /* add the new chain to the existent one */

    for (ln = in; ln; ln = ln->next) {  /* 遍历新传入链，逐节点追加到暂存链尾 */
        ngx_alloc_link_and_set_buf(cl, ln->buf, r->pool, NGX_ERROR);  /* 新建链节点引用同一buf(零拷贝，仅链结构入队，失败宏内报错) */
        *ll = cl;  /* 尾插 */
        ll = &cl->next;  /* 维护尾插指针 */

        size += ngx_buf_size(cl->buf);  /* 累加新buf字节数 */

        if (cl->buf->flush || cl->buf->recycled) {  /* 新buf也可能携带flush/recycled语义 */
            flush = size;  /* 更新强制发送临界点 */
        }  /* flush标志判断结束 */

        if (cl->buf->last_buf) {  /* 新buf带了响应结束标志 */
            last = 1;  /* 置结束标志 */
        }  /* last_buf判断结束 */
    }  /* 新链追加结束 */

    c = r->connection;  /* 取TCP连接对象(含套接字fd与写事件) */

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug级日志 */
                   "http write filter: l:%d f:" OFF_T_FMT " s:" OFF_T_FMT,  /* 打印last/flush/size */
                   last, flush, size);  /* 三个状态值 */

    clcf = ngx_http_get_module_loc_conf(r->main ? r->main : r,  /* 取主请求所在location的core配置： */
                                        ngx_http_core_module);  /* postpone_output/limit_rate等在此读取 */

    /*
     * avoid the output if there is no last buf, no flush point,
     * there are the incoming bufs and the size of all bufs
     * is smaller than "postpone_output" directive
     */

    if (!last && flush == 0 && in && size < (off_t) clcf->postpone_output) {  /* 攒包：响应未结束、无强制点、有新数据且总量小于postpone_output(默认1460≈1个MSS)则暂不发 */
        return NGX_OK;  /* 数据留在ctx->out，等凑满一个TCP段或响应结束再一次writev发出(应用层配合Nagle/TCP_CORK减少小包) */
    }  /* 攒包判断结束 */

    if (c->write->delayed) {  /* 写事件被延迟(限速定时器未到) */
        return NGX_AGAIN;  /* 上级据此挂起，等定时器到期/EPOLLOUT唤醒后续发 */
    }  /* 延迟判断结束 */

    if (size == 0 && !c->buffered) {  /* 无数据可发且连接各缓冲(如SSL/子请求)均为空 */
        if (!last) {  /* 连结束标志都没有却空转到这里 */
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,  /* 记ALERT级异常日志 */
                          "the http output chain is empty");  /* 提示输出链为空 */
        }  /* 异常判断结束 */
        return NGX_OK;  /* 无事可做，正常返回 */
    }  /* 空链判断结束 */

    sent = c->sent;  /* 快照发送前的累计字节数(内核级计数，来自send()返回值累加) */

    chain = c->send_chain(c, ctx->out,  /* 调连接的发送方法聚集发送(writev或sendfile一次系统调用尽量发完) */
                          clcf->limit_rate ? clcf->limit_rate: OFF_T_MAX_VALUE);  /* 未配limit_rate时传极大值表示不限速 */

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug级日志 */
                   "http write filter %X", chain);  /* 打印发送后剩余链指针 */

    if (clcf->limit_rate) {  /* 配置了限速(字节/秒) */
        sent = c->sent - sent;  /* 本次实际发出的字节数 */
        c->write->delayed = 1;  /* 置延迟标志：事件循环本轮跳过写检查 */
        ngx_add_timer(r->connection->write,  /* 给写事件挂定时器 */
                      (ngx_msec_t) (sent * 1000 / clcf->limit_rate));  /* 毫秒后到期：sent*1000/rate即按令牌速率排定下次可发时刻 */
    }  /* 限速分支结束 */

    if (chain == NGX_CHAIN_ERROR) {  /* 底层writev/sendfile返回致命错误 */
        return NGX_ERROR;  /* 上级据此结束请求(写错误日志、按协议收尾) */
    }  /* 错误判断结束 */

    ctx->out = chain;  /* 保存未发完的剩余链：典型于TCP发送缓冲SO_SNDBUF满或对端接收窗口为0(滑动窗口流控)时的部分写 */

    if (chain || c->buffered) {  /* 还有剩余数据，或下级仍有缓冲未清 */
        return NGX_AGAIN;  /* 请求保持未完成：注册/等待EPOLLOUT可写事件后续传 */
    }  /* 未完判断结束 */

    return NGX_OK;  /* 全部发出：请求的本次输出完成 */
}  /* ngx_http_write_filter()结束 */


static ngx_int_t ngx_http_write_filter_init(ngx_cycle_t *cycle)  /* 模块初始化(cycle=运行周期) */
{  /* 函数体开始 */
    ngx_http_top_body_filter = ngx_http_write_filter;  /* 把自己挂到body过滤链顶端：后初始化的模块会插到更上层并把本函数接到自己next，保证写过滤永远是最后一级 */

    return NGX_OK;  /* 初始化成功 */
}  /* ngx_http_write_filter_init()结束 */
