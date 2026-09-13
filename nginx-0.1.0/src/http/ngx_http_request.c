
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置头：编译期探测的平台宏与基础类型定义 */
#include <ngx_core.h>  /* 核心头：内存池/缓冲区/字符串/日志等基础设施声明 */
#include <ngx_event.h>  /* 事件框架头：ngx_event_t/定时器/epoll封装等事件驱动设施 */
#include <ngx_http.h>  /* HTTP框架头：请求结构ngx_http_request_t与各阶段接口声明 */


static void ngx_http_init_request(ngx_event_t *ev);  /* 前置声明：读事件入口——为连接创建/复用请求对象后开始解析 */
#if (NGX_HTTP_SSL)  /* 条件编译：仅启用SSL模块时才需要HTTPS探测逻辑 */
static void ngx_http_ssl_handshake(ngx_event_t *rev);  /* 前置声明：嗅探首字节判断客户端说的是SSL还是明文HTTP */
#endif  /* NGX_HTTP_SSL条件编译结束 */
static void ngx_http_process_request_line(ngx_event_t *rev);  /* 前置声明：读事件驱动的请求行解析循环 */
static void ngx_http_process_request_headers(ngx_event_t *rev);  /* 前置声明：读事件驱动的头部行解析循环 */
static ssize_t ngx_http_read_request_header(ngx_http_request_t *r);  /* 前置声明：recv把请求头字节读入header_in缓冲 */
static ngx_int_t ngx_http_alloc_large_header_buffer(ngx_http_request_t *r,  /* 前置声明：默认头缓冲耗尽时换大缓冲并搬移断点 */
                                                    ngx_uint_t request_line);  /* request_line=1表示换缓冲时请求行尚未解析完 */
static ngx_int_t ngx_http_process_request_header(ngx_http_request_t *r);  /* 前置声明：头部收齐后的语义校验(Host/Content-Length等) */

static void ngx_http_set_write_handler(ngx_http_request_t *r);  /* 前置声明：响应未发完时把连接挂到写事件上继续发 */

static void ngx_http_block_read(ngx_event_t *ev);  /* 前置声明：处理/发送阶段屏蔽读事件，防请求体多余字节误触发 */
static void ngx_http_read_discarded_body_event(ngx_event_t *rev);  /* 前置声明：丢弃请求体阶段的读事件回调 */
static ngx_int_t ngx_http_read_discarded_body(ngx_http_request_t *r);  /* 前置声明：读走并直接扔掉请求体字节 */

static void ngx_http_set_keepalive(ngx_http_request_t *r);  /* 前置声明：响应完毕后转入keepalive等待连接复用 */
static void ngx_http_keepalive_handler(ngx_event_t *ev);  /* 前置声明：keepalive空闲连接上的读事件——等下一个请求 */
static void ngx_http_set_lingering_close(ngx_http_request_t *r);  /* 前置声明：lingering关闭——先读净客户端残留数据再挥手 */
static void ngx_http_lingering_close_handler(ngx_event_t *ev);  /* 前置声明：lingering阶段的读事件回调 */

static void ngx_http_client_error(ngx_http_request_t *r,  /* 前置声明：解析类错误统一记日志并终结请求 */
                                  int client_error, int error);  /* client_error=NGX_HTTP_PARSE_*错误码; error=响应状态码 */
static size_t ngx_http_log_error(void *data, char *buf, size_t len);  /* 日志拼接回调：把"正在做什么+客户端+URL"附加进错误日志 */


/* NGX_HTTP_PARSE_... errors */  /* 原注释：以下为解析错误码到日志文案的映射表(下标=错误码-NGX_HTTP_CLIENT_ERROR) */

static char *client_header_errors[] = {  /* 客户端请求解析错误的日志文案表：顺序与NGX_HTTP_PARSE_*错误码一一对应 */
    "client %s sent invalid method",  /* 方法名不是合法大写字母串(如小写get) */
    "client %s sent invalid request",  /* 请求行整体语法错误 */
    "client %s sent too long URI",  /* URI超过large_client_header_buffers上限 */
    "client %s sent invalid method in HTTP/0.9 request",  /* HTTP/0.9请求(无版本段)却用了不支持的方法 */

    "client %s sent invalid header, URL: %s",  /* 头部行语法非法 */
    "client %s sent too long header line, URL: %s",  /* 单条头部行超长 */
    "client %s sent HTTP/1.1 request without \"Host\" header, URL: %s",  /* HTTP/1.1强制Host头(虚拟主机靠它选server) */
    "client %s sent invalid \"Content-Length\" header, URL: %s",  /* Content-Length不是合法十进制数 */
    "client %s sent POST method without \"Content-Length\" header, URL: %s",  /* POST必须声明正文长度，否则无法定界 */
    "client %s sent plain HTTP request to HTTPS port, URL: %s",  /* 明文HTTP发到了SSL端口 */
    "client %s sent invalid \"Host\" header \"%s\", URL: %s"  /* Host与所有server_name都不匹配且开启restrict */
};  /* client_header_errors 错误文案表定义结束 */


ngx_http_header_t  ngx_http_headers_in[] = {  /* 常用请求头登记表：头部名映射到headers_in结构体中对应指针字段的偏移 */
    { ngx_string("Host"), offsetof(ngx_http_headers_in_t, host) },  /* Host：虚拟主机选server的依据，HTTP/1.1必备 */
    { ngx_string("Connection"), offsetof(ngx_http_headers_in_t, connection) },  /* Connection：close/keep-alive决定TCP连接是否复用 */
    { ngx_string("If-Modified-Since"),  /* If-Modified-Since：缓存协商头，命中则回304省带宽 */
                         offsetof(ngx_http_headers_in_t, if_modified_since) },  /* 该头值在结构体中的存放偏移 */
    { ngx_string("User-Agent"), offsetof(ngx_http_headers_in_t, user_agent) },  /* UA：识别MSIE/Opera等浏览器quirk用 */
    { ngx_string("Referer"), offsetof(ngx_http_headers_in_t, referer) },  /* Referer：来源页，防盗链等模块使用 */
    { ngx_string("Content-Length"),  /* Content-Length：请求体长度，读体/丢弃都靠它定界 */
                            offsetof(ngx_http_headers_in_t, content_length) },  /* 该头值在结构体中的存放偏移 */

    { ngx_string("Range"), offsetof(ngx_http_headers_in_t, range) },  /* Range：断点续传/分段下载的字节区间请求 */
#if 0  /* 禁用的条件块：If-Range暂不解析 */
    { ngx_string("If-Range"), offsetof(ngx_http_headers_in_t, if_range) },  /* If-Range：配合Range做缓存一致性校验(未启用) */
#endif  /* 禁用块结束 */

#if (NGX_HTTP_GZIP)  /* 条件编译：gzip压缩协商相关头 */
    { ngx_string("Accept-Encoding"),  /* Accept-Encoding：客户端支持的压缩编码(gzip)协商 */
                           offsetof(ngx_http_headers_in_t, accept_encoding) },  /* 该头值在结构体中的存放偏移 */
    { ngx_string("Via"), offsetof(ngx_http_headers_in_t, via) },  /* Via：途经的代理链路记录 */
#endif  /* NGX_HTTP_GZIP条件编译结束 */

    { ngx_string("Authorization"),  /* Authorization：认证凭证(HTTP Basic等) */
                             offsetof(ngx_http_headers_in_t, authorization) },  /* 该头值在结构体中的存放偏移 */

    { ngx_string("Keep-Alive"), offsetof(ngx_http_headers_in_t, keep_alive) },  /* Keep-Alive：持久连接的timeout/max参数 */

#if (NGX_HTTP_PROXY)  /* 条件编译：反向代理场景相关头 */
    { ngx_string("X-Forwarded-For"),  /* X-Forwarded-For：代理逐跳追加的客户端真实IP链 */
                           offsetof(ngx_http_headers_in_t, x_forwarded_for) },  /* 该头值在结构体中的存放偏移 */
#endif  /* NGX_HTTP_PROXY条件编译结束 */
    
    { ngx_null_string, 0 }  /* 哨兵项：空名字终结遍历(解析处for循环以name.len==0判断结束) */
};  /* ngx_http_headers_in 请求头登记表定义结束 */


#if 0  /* 整段禁用的历史代码：曾经的占位空handler */
static void ngx_http_dummy(ngx_event_t *wev)  /* 空事件处理器(现由ngx_http_empty_handler承担此职) */
{  /* 函数体开始 */
    return;  /* 什么都不做：仅用于吞掉事件 */
}  /* 函数体结束 */
#endif  /* 禁用块结束 */


/*
 * ngx_http_init_connection()：HTTP层的"连接构造函数"。accept(2)从内核完成队列
 * 取出的新TCP连接由此进入HTTP框架。此刻三次握手已完成、连接挂在epoll上，但
 * 应用层还不知道客户端要什么。本函数只做准备工作：挂日志上下文、把读事件
 * handler设为ngx_http_init_request（事件驱动的核心思想：不主动等待，一切后续
 * 动作由内核通知的可读事件回调驱动），并按post_accept_timeout挂定时器，防御
 * "只完成握手却迟迟不发请求字节"的慢连接占用。整个HTTP请求生命周期自此启程。
 */
void ngx_http_init_connection(ngx_connection_t *c)
{
    ngx_event_t         *rev;  /* 连接的读事件对象：每条连接固定配套一对read/write事件 */
    ngx_http_log_ctx_t  *ctx;  /* 日志上下文：记录"当前动作+客户端地址"，供错误日志拼装 */

    if (!(ctx = ngx_pcalloc(c->pool, sizeof(ngx_http_log_ctx_t)))) {  /* 从连接级内存池分配日志上下文：池分配失败则直接放弃该连接 */
        ngx_http_close_connection(c);  /* 关闭连接：close()触发TCP四次挥手 */
        return;  /* 连接已销毁，立即返回 */
    }  /* 日志上下文分配失败分支结束 */

    ctx->connection = c->number;  /* 记连接序号：日志中区分同一worker内的各条连接 */
    ctx->client = c->addr_text.data;  /* 记对端IP文本：accept时已由inet_ntop格式化的客户端地址 */
    ctx->action = "reading client request line";  /* 当前动作描述：错误日志将带"while reading client request line" */
    c->log->data = ctx;  /* 把上下文挂到连接日志对象：回调按需取用 */
    c->log->handler = ngx_http_log_error;  /* 设置日志拼接回调：nginx日志分"基础消息+上下文回调"两层 */
    c->log_error = NGX_ERROR_INFO;  /* 本连接上的普通错误降为INFO级：客户端异常行为不算服务器故障 */

    rev = c->read;  /* 取出该连接的读事件 */
    rev->event_handler = ngx_http_init_request;  /* 关键注册：首个可读事件(客户端请求字节流到达内核缓冲区)时进入请求初始化 */

    /* STUB: epoll edge */ c->write->event_handler = ngx_http_empty_handler;  /* 写事件先挂空handler：边缘触发下EPOLLOUT会误触发，响应阶段才换成ngx_http_writer */

    if (rev->ready) {  /* 读事件已就绪：deferred accept/rtsig/aio/iocp等机制下accept返回时数据可能已到达 */
        /* the deferred accept(), rtsig, aio, iocp */  /* 原注释：列出ready为真时对应的几种机制 */

        if (ngx_accept_mutex) {  /* 配置了accept_mutex：worker轮流accept，事件投递需在锁保护下进行 */
            if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {  /* 抢posted事件队列锁：防止与其他worker的队列操作竞争 */
                ngx_http_close_connection(c);  /* 加锁失败：放弃这条连接 */
                return;  /* 立即返回 */
            }  /* posted队列锁加锁失败分支结束 */

            ngx_post_event(rev);  /* 读事件入posted队列：推迟到本轮事件循环末尾统一执行，避免持锁状态下重入回调 */

            ngx_mutex_unlock(ngx_posted_events_mutex);  /* 释放posted队列锁 */
            return;  /* 后续处理交给posted队列调度 */
        }  /* accept_mutex 分支结束 */

#if (NGX_STAT_STUB)  /* 统计桩：status模块的全局计数 */
        (*ngx_stat_reading)++;  /* 正在读请求数+1 */
#endif  /* 统计桩结束 */

        ngx_http_init_request(rev);  /* 数据已在内核接收缓冲区：绕过epoll往返直接调用，降低延迟 */
        return;  /* 请求初始化已启动 */
    }  /* 读事件已就绪分支结束 */

    ngx_add_timer(rev, c->listening->post_accept_timeout);  /* 挂post_accept_timeout定时器：accept后客户端迟迟不发首字节，到点强制关连接 */

    if (ngx_handle_read_event(rev, 0) == NGX_ERROR) {  /* 把读事件注册进epoll：从此等待请求数据到来 */
        ngx_http_close_connection(c);  /* epoll注册失败(罕见，如fd耗尽)：关闭连接 */
        return;  /* 返回 */
    }  /* 读事件注册失败分支结束 */

#if 0  /* 禁用的TODO代码：计划学习SO_SNDBUF以支持zerocopy */
    /* TODO: learn SO_SNDBUF (to use in zerocopy) via kqueue's EV_CLEAR event */  /* 原注释：保留的原计划说明 */

    c->write->ready = 0;  /* 清写就绪标志(禁用代码) */
    c->write->event_handler = ngx_http_dummy;  /* 写事件挂占位handler(禁用代码) */

    if (ngx_handle_write_event(c->write, 0) == NGX_ERROR) {  /* 登记写事件(禁用代码) */
        ngx_http_close_connection(c);  /* 失败则关连接(禁用代码) */
        return;  /* 返回(禁用代码) */
    }  /* (禁用)写事件登记失败分支结束 */
#endif  /* 禁用块结束 */

#if (NGX_STAT_STUB)  /* 统计桩 */
    (*ngx_stat_reading)++;  /* 正在读请求数+1：连接正式进入"等待/读取请求"阶段 */
#endif  /* 统计桩结束 */

}  /* 函数结束：连接已挂入事件循环，此后一切进展由事件回调驱动 */


/*
 * ngx_http_init_request()：HTTP请求的"请求构造函数"。首个读事件触发时执行——
 * 内核接收缓冲区有了客户端发来的请求字节流(或keepalive/pipeline复用的连接上
 * 到来了下一个请求)。职责：按IP:PORT与Host两级定位server配置、分配(或复用)
 * ngx_http_request_t、创建请求级内存池、初始化头缓冲，然后把读handler换成
 * 请求行解析器并立即推进解析。本函数同时是keepalive复用与pipeline的汇合点。
 */
static void ngx_http_init_request(ngx_event_t *rev)
{
    ngx_uint_t                 i;  /* 循环下标：地址表遍历用 */
    socklen_t                  len;  /* getsockname的地址长度入/出参 */
    struct sockaddr_in         addr_in;  /* 接收本端地址的结构体(IPv4) */
    ngx_connection_t          *c;  /* 底层TCP连接 */
    ngx_http_request_t        *r;  /* 本函数要创建/复用的HTTP请求对象 */
    ngx_http_in_port_t        *in_port;  /* 监听端口级结构：该端口上所有监听地址的容器 */
    ngx_http_in_addr_t        *in_addr;  /* 地址级结构：某地址上的虚拟主机名表与默认server */
    ngx_http_connection_t     *hc;  /* 连接级HTTP状态：跨请求保存大缓冲free/busy链与上个请求 */
    ngx_http_server_name_t    *server_name;  /* server_name表元素：取默认主机名 */
    ngx_http_core_srv_conf_t  *cscf;  /* server级配置：头缓冲大小/超时等 */
    ngx_http_core_loc_conf_t  *clcf;  /* location级配置：错误日志等 */
#if (NGX_HTTP_SSL)  /* 条件编译：SSL模块的server级配置 */
    ngx_http_ssl_srv_conf_t   *sscf;  /* SSL配置：是否强制https握手 */
#endif  /* NGX_HTTP_SSL条件编译结束 */

    c = rev->data;  /* 事件的data回指连接：nginx事件回调统一从ev->data拿上下文 */

    if (rev->timedout) {  /* post_accept_timeout到点：客户端accept后一直不发首字节 */
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, "client timed out");  /* 记INFO级超时日志 */

#if (NGX_STAT_STUB)  /* 统计桩 */
        (*ngx_stat_reading)--;  /* 该连接不再处于reading，计数回落 */
#endif  /* 统计桩结束 */

        ngx_http_close_connection(c);  /* 关闭连接：慢连接不占着资源 */
        return;  /* 结束 */
    }  /* post_accept 超时分支结束 */

    hc = c->data;  /* 取连接级HTTP状态：keepalive复用时c->data还留着上一轮的hc */

    if (hc) {  /* 已有hc：说明是keepalive/pipeline复用的连接，直接沿用 */

#if (NGX_STAT_STUB)  /* 统计桩 */
        (*ngx_stat_reading)++;  /* 复用连接重新进入reading计数 */
#endif  /* 统计桩结束 */

    } else {  /* 全新连接：还没有HTTP层状态 */
        if (!(hc = ngx_pcalloc(c->pool, sizeof(ngx_http_connection_t)))) {  /* 从连接池分配hc：随连接销毁统一释放 */

#if (NGX_STAT_STUB)  /* 统计桩 */
            (*ngx_stat_reading)--;  /* 分配失败，reading计数回落 */
#endif  /* 统计桩结束 */

            ngx_http_close_connection(c);  /* 内存耗尽：关闭连接 */
            return;  /* 结束 */
        }  /* hc 分配失败分支结束 */
    }  /* 全新连接 else 分支结束 */

    r = hc->request;  /* hc里可能还留着上个请求的结构(keepalive时缓存复用) */

    if (r) {  /* 有旧请求结构：清零后整体复用，省一次分配与缓存冷启动 */
        ngx_memzero(r, sizeof(ngx_http_request_t));  /* 整块清零：所有字段回到初值，旧请求状态不会串到新请求 */

        r->pipeline = hc->pipeline;  /* 恢复管线标志：上一请求尾部还捎带了下一请求的字节 */

        if (hc->nbusy) {  /* 大头缓冲里存着已读到的下一请求前缀 */
            r->header_in = hc->busy[0];  /* 直接以它作本请求的头缓冲：TCP字节流中未消费的数据不能丢 */
        }  /* busy 存量判断结束 */

    } else {  /* 没有可复用的：全新分配请求结构 */
        if (!(r = ngx_pcalloc(c->pool, sizeof(ngx_http_request_t)))) {  /* 从连接池分配请求对象 */

#if (NGX_STAT_STUB)  /* 统计桩 */
            (*ngx_stat_reading)--;  /* 分配失败，reading计数回落 */
#endif  /* 统计桩结束 */

            ngx_http_close_connection(c);  /* 内存耗尽：关连接 */
            return;  /* 结束 */
        }  /* 请求结构分配失败分支结束 */

        hc->request = r;  /* 反向登记：请求结束后hc还持有它供下轮复用 */
    }  /* 全新分配请求分支结束 */

#if (NGX_STAT_STUB)  /* 统计桩 */
    r->stat_reading = 1;  /* 在请求上记阅读标志：关闭时按标志对称递减计数 */
#endif  /* 统计桩结束 */

    c->data = r;  /* 连接的data改指请求：从此读事件的上下文是r(原先是hc) */
    r->http_connection = hc;  /* 请求回指hc：取大缓冲free/busy链时用 */

    c->sent = 0;  /* 已发送字节清零：本请求响应的发送量从头统计 */
    r->signature = NGX_HTTP_MODULE;  /* 模块签名标记：供模块用安全转型校验这是HTTP请求 */

    /* find the server configuration for the address:port */  /* 原注释：开始按地址:端口定位server配置 */

    /* AF_INET only */  /* 原注释：本版仅支持IPv4 */

    in_port = c->servers;  /* 取本连接所属监听端口的地址表 */
    in_addr = in_port->addrs.elts;  /* 地址数组首元素 */

    r->port = in_port->port;  /* 记监听端口数值 */
    r->port_text = &in_port->port_text;  /* 记端口文本(日志/绝对重定向用) */

    i = 0;  /* 默认选第0个地址(即通配地址) */

    if (in_port->addrs.nelts > 1) {  /* 同一端口绑了多个具体IP：需区分客户端连的是哪个IP */

        /*
         * There are several addresses on this port and one of them
         * is the "*:port" wildcard so getsockname() is needed to determine
         * the server address.
         *
         * AcceptEx() already gave this address.
         */  /* 原注释：多地址+通配地址并存时须查本端地址；AcceptEx已在accept时带回 */

#if (WIN32)  /* Windows分支：AcceptEx已提供本端地址，免一次系统调用 */
        if (c->local_sockaddr) {  /* 已有本端地址缓存 */
            r->in_addr =  /* 赋值起行：本端IPv4地址取自 sockaddr_in(网络字节序) */
                   ((struct sockaddr_in *) c->local_sockaddr)->sin_addr.s_addr;  /* 直接取网络字节序IP */

        } else {  /* 没有则回落到getsockname */
#endif  /* WIN32分支结束 */
            len = sizeof(struct sockaddr_in);  /* 入/出长度参数置初值 */
            if (getsockname(c->fd, (struct sockaddr *) &addr_in, &len) == -1) {  /* 查本端socket地址：确定客户端目的IP */
                ngx_connection_error(c, ngx_socket_errno,  /* 失败记错误日志 */
                                     "getsockname() failed");
                ngx_http_close_connection(c);  /* 关闭连接 */
                return;  /* 结束 */
            }  /* getsockname 失败分支结束 */

            r->in_addr = addr_in.sin_addr.s_addr;  /* 记下本端IP(网络字节序) */

#if (WIN32)  /* Windows条件分支结束 */
        }  /* WIN32 else 分支结束 */
#endif  /* WIN32条件编译结束 */

        /* the last in_port->addrs address is "*" */  /* 原注释：地址表最后一项约定是"*"通配 */

        for ( /* void */ ; i < in_port->addrs.nelts - 1; i++) {  /* 遍历具体地址项(跳过末尾通配项)：找与本端IP相等的项 */
            if (in_addr[i].addr == r->in_addr) {  /* 命中具体地址 */
                break;  /* 以该地址的虚拟主机配置为准 */
            }  /* 命中具体地址分支结束 */
        }  /* 地址表遍历结束 */

    } else {  /* 单地址(通配)监听：无需查询 */
        r->in_addr = in_addr[0].addr;  /* 直接取第0项地址 */
    }  /* 单地址监听分支结束 */

    r->virtual_names = &in_addr[i].names;  /* 挂上该IP上的虚拟主机名哈希表：等Host头再二级定位 */

    /* the default server configuration for the address:port */  /* 原注释：先取该地址的默认server配置(Host尚未解析) */
    cscf = in_addr[i].core_srv_conf;  /* 默认server：Host缺失或不匹配时兜底 */

    r->main_conf = cscf->ctx->main_conf;  /* 挂main级配置指针数组：请求此后可直接按模块索引取配置 */
    r->srv_conf = cscf->ctx->srv_conf;  /* 挂server级配置数组 */
    r->loc_conf = cscf->ctx->loc_conf;  /* 挂location级配置数组(后续按URI重定位) */

    rev->event_handler = ngx_http_process_request_line;  /* 读handler升级为请求行解析：下一次可读事件直接进解析循环 */

#if (NGX_HTTP_SSL)  /* 条件编译：SSL端口处理 */

    sscf = ngx_http_get_module_srv_conf(r, ngx_http_ssl_module);  /* 取SSL配置 */
    if (sscf->enable) {  /* 该server开启ssl：端口期望TLS流量 */

        if (c->ssl == NULL) {  /* 尚未建立SSL会话 */
            if (ngx_ssl_create_session(sscf->ssl_ctx, c, NGX_SSL_BUFFER)  /* 为连接创建SSL会话(缓冲模式写) */
                                                                  == NGX_ERROR)
            {
                ngx_http_close_connection(c);  /* SSL会话创建失败：关连接 */
                return;  /* 结束 */
            }  /* SSL会话创建失败分支结束 */

            /*
             * The majority of browsers do not send the "close notify" alert.
             * Among them are MSIE, Mozilla, Netscape 4, Konqueror, and Links.
             * And what is more MSIE ignores the server's alert.
             *
             * Opera always sends the alert.
             */  /* 原注释：多数浏览器不发close_notify告警，故关闭时不能等它 */

            c->ssl->no_rcv_shut = 1;  /* 关闭时不等待对端close_notify，直接拆除SSL层 */
            rev->event_handler = ngx_http_ssl_handshake;  /* 读handler换成SSL握手探测：先嗅探首字节判断协议 */
        }  /* SSL会话未建立分支结束 */

        r->filter_need_in_memory = 1;  /* 响应过滤链要求在内存中处理(SSL加密需要完整缓冲而非sendfile直发) */
    }  /* ssl 开启分支结束 */

#endif  /* NGX_HTTP_SSL条件编译结束 */

    server_name = cscf->server_names.elts;  /* 取默认server的第一个server_name */
    r->server_name = &server_name->name;  /* 记为当前主机名：Host头匹配成功后会覆盖 */

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取core模块location级配置(此时是默认server的) */
    c->log->file = clcf->err_log->file;  /* 日志文件按server配置切换：不同server可配不同error_log */
    if (!(c->log->log_level & NGX_LOG_DEBUG_CONNECTION)) {  /* 非连接级debug时 */
        c->log->log_level = clcf->err_log->log_level;  /* 日志级别整体切换为新server的配置 */
    }  /* 日志级别切换判断结束 */

    if (c->buffer == NULL) {  /* 连接级头缓冲尚未分配(keepalive复用时已有) */
        c->buffer = ngx_create_temp_buf(c->pool,  /* 创建默认大小的请求头接收缓冲 */
                                        cscf->client_header_buffer_size);
        if (c->buffer == NULL) {  /* 分配失败 */
            ngx_http_close_connection(c);  /* 关连接 */
            return;  /* 结束 */
        }  /* 缓冲分配失败分支结束 */
    }  /* 连接级缓冲分配判断结束 */

    if (r->header_in == NULL) {  /* 请求还没有专属头缓冲(pipeline复用时已指向busy[0]) */
        r->header_in = c->buffer;  /* 先用连接级默认缓冲：绝大多数请求一个缓冲就够 */
    }  /* 头缓冲指向判断结束 */

    if (!(r->pool = ngx_create_pool(cscf->request_pool_size, c->log))) {  /* 创建请求级内存池：本请求所有动态数据都从这里分配 */
        ngx_http_close_connection(c);  /* 创建失败关连接 */
        return;  /* 结束 */
    }  /* 请求池创建失败分支结束 */

    if (ngx_array_init(&r->cleanup, r->pool, 5, sizeof(ngx_http_cleanup_t))  /* 初始化清理链表：登记请求结束后须释放的资源(打开的文件等) */
                                                                  == NGX_ERROR)
    {  /* 初始化失败块开始 */
        ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 按请求方式收尾(销毁请求池) */
        ngx_http_close_connection(c);  /* 再关连接 */
        return;  /* 结束 */
    }  /* 清理链初始化失败分支结束 */


    if (ngx_list_init(&r->headers_out.headers, r->pool, 20,  /* 初始化响应头链表：预分配20条，均摊O(1)追加 */
                                         sizeof(ngx_table_elt_t)) == NGX_ERROR)
    {  /* 初始化失败块开始 */
        ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 请求按500收尾 */
        ngx_http_close_connection(c);  /* 关连接 */
        return;  /* 结束 */
    }  /* 响应头链表初始化失败分支结束 */


    r->ctx = ngx_pcalloc(r->pool, sizeof(void *) * ngx_http_max_module);  /* 各HTTP模块的请求级上下文指针数组(按模块序号索引) */
    if (r->ctx == NULL) {  /* 分配失败 */
        ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 请求按500收尾 */
        ngx_http_close_connection(c);  /* 关连接 */
        return;  /* 结束 */
    }  /* 上下文数组分配失败分支结束 */

    c->single_connection = 1;  /* 标记单连接模式：本请求独占该连接 */
    r->connection = c;  /* 请求回指连接：后续读写/关闭都经它 */

    r->file.fd = NGX_INVALID_FILE;  /* 请求涉及的静态文件fd初始化为无效：表示尚未打开 */

    r->headers_in.content_length_n = -1;  /* 请求体长度未知(-1)：等Content-Length头解析后回填 */
    r->headers_in.keep_alive_n = -1;  /* keepalive超时参数未知(-1) */
    r->headers_out.content_length_n = -1;  /* 响应体长度未知(-1)：由内容阶段或filter决定 */
    r->headers_out.last_modified_time = -1;  /* 响应Last-Modified时间未知(-1) */

    r->http_state = NGX_HTTP_READING_REQUEST_STATE;  /* 请求状态机置为"读请求中"：错误日志会据此描述 */

#if (NGX_STAT_STUB)  /* 统计桩 */
    (*ngx_stat_requests)++;  /* 已受理请求数+1 */
#endif  /* 统计桩结束 */

    rev->event_handler(rev);  /* 立即调用新handler(process_request_line)：内核缓冲区里可能已有数据，无需再等一轮epoll */
}  /* 函数结束 */


#if (NGX_HTTP_SSL)  /* 条件编译：SSL端口握手嗅探 */

/*
 * ngx_http_ssl_handshake()：同一端口同时识别https与明文http的协议嗅探。
 * TLS规定客户端先发ClientHello，其记录层首字节固定为0x16(SSLv3/TLSv1)或
 * 0x80(SSLv2)，而HTTP请求行首字节必是方法大写字母——协议首字节天然可区分。
 * 故用MSG_PEEK偷看1字节而不消费：是TLS就切换SSL收发函数完成握手；是明文则
 * 标记plain_http由上层报错。数据未到(EAGAIN)则挂起等下次可读事件。
 */
static void ngx_http_ssl_handshake(ngx_event_t *rev)
{
    int                  n;  /* recv返回值：窥视到的字节数或错误 */
    ngx_int_t            rc;  /* SSL握手结果码 */
    u_char               buf[1];  /* 1字节嗅探缓冲 */
    ngx_connection_t    *c;  /* 底层连接 */
    ngx_http_request_t  *r;  /* 请求对象 */

    c = rev->data;  /* 事件回指连接 */
    r = c->data;  /* 连接回指请求 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0,  /* debug日志 */
                   "http check ssl handshake");

    if (rev->timedout) {  /* 等待超时：客户端连上却迟迟不发ClientHello */
        ngx_http_client_error(r, 0, NGX_HTTP_REQUEST_TIME_OUT);  /* 按408记录并终结请求 */
        return;  /* 结束 */
    }  /* 握手等待超时分支结束 */

    n = recv(c->fd, buf, 1, MSG_PEEK);  /* 关键：MSG_PEEK只窥视不消费——若判为明文HTTP，请求行字节仍留在内核缓冲区可交给原解析器 */

    if (n == -1 && ngx_socket_errno == NGX_EAGAIN) {  /* 内核接收缓冲区还空(TCP数据未到达) */
        return;  /* 挂起等下一次可读事件：不另挂定时器，沿用post_accept超时 */
    }  /* 数据未到达挂起分支结束 */

    if (n == 1) {  /* 成功窥得1字节 */
        if (buf[0] == 0x80 /* SSLv2 */ || buf[0] == 0x16 /* SSLv3/TLSv1 */) {  /* 原注释+判断：首字节符合TLS/SSL记录格式 */
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, rev->log, 0,  /* debug记录协议版本字节 */
                           "https ssl handshake: 0x%X", buf[0]);

            c->recv = ngx_ssl_recv;  /* 连接收函数切换为SSL版：此后"读"实为对TLS记录的解密读 */
            c->send_chain = ngx_ssl_send_chain;  /* 发送同理切换为SSL加密写 */

            rc = ngx_ssl_handshake(c);  /* 启动SSL/TLS握手：非阻塞下可能立即完成或需等更多握手字节 */

            if (rc == NGX_ERROR) {  /* 握手直接失败(证书/协议版本错误等) */
                ngx_http_close_request(r, NGX_HTTP_BAD_REQUEST);  /* 请求按400收尾 */
                ngx_http_close_connection(r->connection);  /* 关闭TCP连接 */
                return;  /* 结束 */
            }  /* 握手直接失败分支结束 */

            if (rc != NGX_OK) {  /* 握手尚未完成 */
                return;  /* 挂起：SSL层已登记读写事件，握手完成后由此继续 */
            }  /* 握手未完成挂起分支结束 */

        } else {  /* 首字节非TLS格式：客户端向https端口发的是明文 */
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0,  /* debug日志 */
                           "plain http");

            r->plain_http = 1;  /* 标记明文：process_request_header将报"https端口收到明文"错误 */
        }  /* 明文HTTP标记分支结束 */
    }  /* 窥视字节判断结束 */

    rev->event_handler = ngx_http_process_request_line;  /* 协议判定完毕：读handler换回请求行解析 */
    ngx_http_process_request_line(rev);  /* 立即推进解析：MSG_PEEK未消费，缓冲区里数据仍在 */
}  /* ngx_http_ssl_handshake 函数结束 */

#endif  /* NGX_HTTP_SSL条件编译结束 */


/*
 * ngx_http_process_request_line()：读事件驱动的请求行解析循环。TCP是字节流
 * 协议，一次recv可能只读到半个请求行(拆包)，也可能一次捎来多个请求(粘包)。
 * 应对策略：循环"读字节→调ngx_http_parse_request_line状态机推进"；返回
 * NGX_AGAIN(数据不够)时把断点存于r->state并挂起等下一次可读事件，从断点
 * 无损续析——流式协议解析与事件驱动的经典配合。解析成功则把URI/参数/扩展名
 * 各段复制到请求池，再切换到头部解析；错误则经client_error映射400/501等。
 */
static void ngx_http_process_request_line(ngx_event_t *rev)
{
    u_char              *p;  /* 扫描指针：错误时找请求行末尾用 */
    ssize_t              n;  /* 本次recv读到的字节数 */
    ngx_int_t            rc, rv;  /* rc=解析状态机返回值; rv=换大缓冲结果 */
    ngx_connection_t    *c;  /* 底层TCP连接 */
    ngx_http_request_t  *r;  /* 请求对象：状态机断点r->state就存在它身上 */
    ngx_http_log_ctx_t  *ctx;  /* 日志上下文：切换"正在读头部"描述用 */

    c = rev->data;  /* 事件回指连接 */
    r = c->data;  /* 连接回指请求 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0,  /* debug日志 */
                   "http process request line");

    if (rev->timedout) {  /* 读请求超时：客户端发了一半停住(client_header_timeout到点) */
        ngx_http_client_error(r, 0, NGX_HTTP_REQUEST_TIME_OUT);  /* 按408记录并终结 */
        return;  /* 结束 */
    }  /* 读请求行超时分支结束 */

    rc = NGX_AGAIN;  /* 先置AGAIN迫使循环首轮先读数据 */

    for ( ;; ) {  /* 解析主循环："数据不够就读，读到就推进状态机"，直到解析完成或出错 */

        if (rc == NGX_AGAIN) {  /* 上一轮状态机说字节不够：需要再recv */
            n = ngx_http_read_request_header(r);  /* 从内核缓冲区读请求头字节进header_in */

            if (n == NGX_AGAIN || n == NGX_ERROR) {  /* AGAIN=内核读空挂起等事件(拆包)；ERROR=连接已关 */
                return;  /* 两者都已处理完毕，直接返回 */
            }  /* 挂起或出错分支结束 */
        }  /* 需再读数据分支结束 */

        rc = ngx_http_parse_request_line(r, r->header_in);  /* 调用状态机从r->state断点继续解析(状态机本体在ngx_http_parse.c) */

        if (rc == NGX_OK) {  /* 请求行解析完成 */

            /* the request line has been parsed successfully */  /* 原注释 */

            /* copy unparsed URI */  /* 原注释：先保存未经任何处理的原始URI */

            r->unparsed_uri.len = r->uri_end - r->uri_start;  /* 原始URI长度=状态机标记的首尾指针差 */
            r->unparsed_uri.data = ngx_palloc(r->pool, r->unparsed_uri.len + 1);  /* 从请求池分配(+1留给'\0')：header_in缓冲日后会被复用覆盖，必须拷出 */
            if (r->unparsed_uri.data == NULL) {  /* 分配失败 */
                ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 请求按500收尾 */
                ngx_http_close_connection(c);  /* 关连接 */
                return;  /* 结束 */
            }  /* 原始URI分配失败分支结束 */

            ngx_cpystrn(r->unparsed_uri.data, r->uri_start,  /* 拷贝原始URI并补'\0' */
                        r->unparsed_uri.len + 1);


            /* copy URI */  /* 原注释：再保存去掉参数部分的纯URI(路由匹配用) */

            if (r->args_start) {  /* URI带参数(有'?')：参数起点前1字节是'?' */
                r->uri.len = r->args_start - 1 - r->uri_start;  /* 纯URI长度不含'?'及其后参数 */
            } else {  /* 无参数 */
                r->uri.len = r->uri_end - r->uri_start;  /* 整段即URI */
            }  /* 无参数分支结束 */

            if (!(r->uri.data = ngx_palloc(r->pool, r->uri.len + 1))) {  /* 为纯URI分配空间 */
                ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 500收尾 */
                ngx_http_close_connection(c);  /* 关连接 */
                return;  /* 结束 */
            }  /* URI分配失败分支结束 */

            if (r->complex_uri) {  /* 复杂URI(含%XX转义/./../等)：需规范化 */
                rc = ngx_http_parse_complex_uri(r);  /* 做URI解码与路径归一化(消除../防目录穿越) */

                if (rc == NGX_HTTP_INTERNAL_SERVER_ERROR) {  /* 规范化内部错误(如分配失败) */
                    ngx_http_close_request(r, rc);  /* 按500收尾 */
                    ngx_http_close_connection(c);  /* 关连接 */
                    return;  /* 结束 */
                }  /* URI规范化内部错误分支结束 */

                if (rc != NGX_OK) {  /* 其余非OK值=URI语法错误 */
                    r->request_line.len = r->request_end - r->request_start;  /* 补齐请求行信息供错误日志 */
                    r->request_line.data = r->request_start;  /* 数据指针指向请求行起始 */

                    ngx_http_client_error(r, rc, NGX_HTTP_BAD_REQUEST);  /* 按400报错终结 */
                    return;  /* 结束 */
                }  /* URI语法错误分支结束 */

            } else {  /* 简单URI：无转义无需规范化 */
                ngx_cpystrn(r->uri.data, r->uri_start, r->uri.len + 1);  /* 原样拷贝即可 */
            }  /* 简单URI分支结束 */


            r->request_line.len = r->request_end - r->request_start;  /* 请求行整体(方法+URI+版本)长度 */
            r->request_line.data = r->request_start;  /* 请求行首址 */
            r->request_line.data[r->request_line.len] = '\0';  /* 补'\0'：日志可直接当字符串打印 */

            if (r->method == 0) {  /* HTTP/0.9请求：状态机没识别版本段、也没设method位图 */
                r->method_name.len = r->method_end - r->request_start + 1;  /* 用名字方式记方法(0.9只有方法+URI) */
                r->method_name.data = r->request_line.data;  /* 方法名数据指针=请求行首(方法位于行首) */
            }  /* HTTP/0.9 方法记录判断结束 */

            if (r->uri_ext) {  /* URI中有扩展名(出现过'.') */

                /* copy URI extention */  /* 原注释 */

                if (r->args_start) {  /* 扩展名止于'?'前 */
                    r->exten.len = r->args_start - 1 - r->uri_ext;  /* 长度算到'?'之前 */
                } else {  /* URI 无参数(无?)的 else 分支 */
                    r->exten.len = r->uri_end - r->uri_ext;  /* 无参数则算到URI尾 */
                }  /* 扩展名无参数分支结束 */

                if (!(r->exten.data = ngx_palloc(r->pool, r->exten.len + 1))) {  /* 分配扩展名空间 */
                    ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 500收尾 */
                    ngx_http_close_connection(c);  /* 关连接 */
                    return;  /* 结束 */
                }  /* 扩展名分配失败分支结束 */

                ngx_cpystrn(r->exten.data, r->uri_ext, r->exten.len + 1);  /* 拷贝扩展名(如html/cgi)：mime类型与SSI判定用 */
            }  /* 扩展名拷贝结束 */

            if (r->args_start && r->uri_end > r->args_start) {  /* 存在参数段 */

                /* copy URI arguments */  /* 原注释 */

                r->args.len = r->uri_end - r->args_start;  /* 参数串长度 */

                if (!(r->args.data = ngx_palloc(r->pool, r->args.len + 1))) {  /* 分配参数空间 */
                    ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 500收尾 */
                    ngx_http_close_connection(c);  /* 关连接 */
                    return;  /* 结束 */
                }  /* 参数分配失败分支结束 */

                ngx_cpystrn(r->args.data, r->args_start, r->args.len + 1);  /* 拷贝参数(?a=b&c=d部分) */
            }  /* 参数段拷贝结束 */

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug输出请求行 */
                           "http request line: \"%s\"", r->request_line.data);

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug输出规范化后URI */
                           "http uri: \"%s\"", r->uri.data);

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug输出参数 */
                           "http args: \"%s\"",
                           r->args.data ? r->args.data : (u_char *) "");

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug输出扩展名 */
                           "http exten: \"%s\"",
                           r->exten.data ? r->exten.data : (u_char *) "");

            if (r->http_version < NGX_HTTP_VERSION_10) {  /* HTTP/0.9：没有头部段、也没有Host */
                rev->event_handler = ngx_http_block_read;  /* 读handler挂"屏蔽读"：0.9无请求体，多余字节皆非法 */
                ngx_http_handler(r);  /* 直接进入HTTP处理阶段：跳过头部解析 */
                return;  /* 处理已移交 */
            }  /* HTTP/0.9 处理分支结束 */


            if (ngx_list_init(&r->headers_in.headers, r->pool, 20,  /* 初始化请求头链表：预分配20条头部槽位 */
                                         sizeof(ngx_table_elt_t)) == NGX_ERROR)
            {  /* 初始化失败块 */
                ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 500收尾 */
                ngx_http_close_connection(c);  /* 关连接 */
                return;  /* 结束 */
            }  /* 请求头链表初始化失败分支结束 */


            if (ngx_array_init(&r->headers_in.cookies, r->pool, 5,  /* 初始化Cookie指针数组：Cookie可重复出现，需单独收集全部 */
                                       sizeof(ngx_table_elt_t *)) == NGX_ERROR)
            {  /* 初始化失败块 */
                ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 500收尾 */
                ngx_http_close_connection(c);  /* 关连接 */
                return;  /* 结束 */
            }  /* Cookie 数组初始化失败分支结束 */


            ctx = c->log->data;  /* 取日志上下文 */
            ctx->action = "reading client request headers";  /* 动作描述切换：后续错误日志带"while reading ... headers" */
            ctx->url = r->unparsed_uri.data;  /* 日志上下文补上URL：此刻起错误日志能定位到具体请求 */

            rev->event_handler = ngx_http_process_request_headers;  /* 读handler升级为头部解析 */
            ngx_http_process_request_headers(rev);  /* 立即推进：缓冲区中头部字节可能已随请求行一起到达(TCP粘包) */

            return;  /* 移交完毕 */

        } else if (rc != NGX_AGAIN) {  /* 状态机返回错误码(非OK非AGAIN) */

            /* there was error while a request line parsing */  /* 原注释 */

            for (p = r->request_start; p < r->header_in->last; p++) {  /* 从请求行首扫描：找到第一个CR/LF定出请求行末尾 */
                if (*p == CR || *p == LF) {  /* 行结束符即请求行边界 */
                    break;  /* 停在这里 */
                }  /* 找到行结束符分支结束 */
            }  /* 请求行末尾扫描结束 */

            r->request_line.len = p - r->request_start;  /* 尽量还原残缺请求行：供错误日志展示客户端到底发了什么 */
            r->request_line.data = r->request_start;  /* 指向残缺请求行起始 */

            if (rc == NGX_HTTP_PARSE_INVALID_METHOD) {  /* 方法非法的情形 */
                r->http_version = NGX_HTTP_VERSION_10;  /* 版本置1.0：让响应带上完整状态行(501需要版本) */
            }  /* 非法方法版本修正判断结束 */

            ngx_http_client_error(r, rc,  /* 按错误码记日志并终结请求 */
                                  (rc == NGX_HTTP_PARSE_INVALID_METHOD) ?
                                         NGX_HTTP_NOT_IMPLEMENTED:
                                         NGX_HTTP_BAD_REQUEST);
            return;  /* 结束 */
        }  /* 请求行解析错误分支结束 */

        /* NGX_AGAIN: a request line parsing is still incomplete */  /* 原注释：请求行未解析完(TCP拆包，字节不够) */

        if (r->header_in->pos == r->header_in->end) {  /* 缓冲区已被状态机消费到尾：请求行超出了当前缓冲容量 */

            rv = ngx_http_alloc_large_header_buffer(r, 1);  /* 换large_client_header_buffers大缓冲并搬移解析断点 */

            if (rv == NGX_ERROR) {  /* 换缓冲失败(分配失败) */
                ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 500收尾 */
                ngx_http_close_connection(c);  /* 关连接 */
                return;  /* 结束 */
            }  /* 换大缓冲失败分支结束 */

            if (rv == NGX_DECLINED) {  /* 已达大缓冲数量/大小上限：URI超长 */
                ngx_http_client_error(r, NGX_HTTP_PARSE_TOO_LONG_URI,  /* 按414报错终结 */
                                      NGX_HTTP_REQUEST_URI_TOO_LARGE);
                return;  /* 结束 */
            }  /* URI超长分支结束 */
        }  /* 缓冲耗尽判断结束 */
    }  /* 请求行解析主循环结束 */
}  /* 函数结束：要么请求行完成进入头部解析，要么挂起等数据，要么报错终结 */


/*
 * ngx_http_process_request_headers()：读事件驱动的头部解析循环。逐行调用
 * ngx_http_parse_header_line状态机：每成功一行就登记进headers_in链表，并按
 * ngx_http_headers_in登记表把常用头直挂到结构体字段(O(1)访问)。遇到空行
 * (CRLF)即头段结束(NGX_HTTP_PARSE_HEADER_DONE)，做语义校验后进入处理阶段。
 * 数据不够(NGX_AGAIN)时同样挂起等下次可读事件，从r->state断点续析。
 */
static void ngx_http_process_request_headers(ngx_event_t *rev)
{
    ssize_t              n;  /* 本次读到的字节数 */
    ngx_int_t            rc, rv, i;  /* rc=头部状态机返回; rv=换缓冲结果; i=登记表下标 */
    ngx_table_elt_t     *h, **cookie;  /* h=新解析出的头部元素; cookie=Cookie数组槽位 */
    ngx_connection_t    *c;  /* 底层连接 */
    ngx_http_request_t  *r;  /* 请求对象 */

    c = rev->data;  /* 事件回指连接 */
    r = c->data;  /* 连接回指请求 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0,  /* debug日志 */
                   "http process request header line");

    if (rev->timedout) {  /* 头部接收超时：客户端发送头部途中停顿过久 */
        ngx_http_client_error(r, 0, NGX_HTTP_REQUEST_TIME_OUT);  /* 按408终结 */
        return;  /* 结束 */
    }  /* 头部接收超时分支结束 */

    rc = NGX_AGAIN;  /* 置AGAIN迫使首轮先读数据 */

    for ( ;; ) {  /* 头部解析主循环：直到头段收齐或出错 */

        if (rc == NGX_AGAIN) {  /* 状态机需要更多字节 */

            if (r->header_in->pos == r->header_in->end) {  /* 缓冲区已被消费光：单条头行超长 */

                rv = ngx_http_alloc_large_header_buffer(r, 0);  /* 换大缓冲(参数0=正在解析头部行而非请求行) */

                if (rv == NGX_ERROR) {  /* 分配失败 */
                    ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 500收尾 */
                    ngx_http_close_connection(c);  /* 关连接 */
                    return;  /* 结束 */
                }  /* 换大缓冲失败分支结束 */

                if (rv == NGX_DECLINED) {  /* 超出大缓冲上限：头部行超长 */
                    ngx_http_client_error(r, NGX_HTTP_PARSE_TOO_LONG_HEADER,  /* 按400报"头行过长"终结 */
                                          NGX_HTTP_BAD_REQUEST);
                    return;  /* 结束 */
                }  /* 头行超长分支结束 */
            }  /* 缓冲耗尽判断结束 */

            n = ngx_http_read_request_header(r);  /* recv读入更多头部字节 */

            if (n == NGX_AGAIN || n == NGX_ERROR) {  /* AGAIN=内核读空挂起；ERROR=连接已关 */
                return;  /* 已处理完毕 */
            }  /* 挂起或出错分支结束 */
        }  /* 需再读数据分支结束 */

        rc = ngx_http_parse_header_line(r, r->header_in);  /* 头部行状态机从r->state断点续析 */

        if (rc == NGX_OK) {  /* 成功解析出一条完整头部行 */

            /* a header line has been parsed successfully */  /* 原注释 */

            r->headers_n++;  /* 头部行计数+1 */

            if (!(h = ngx_list_push(&r->headers_in.headers))) {  /* 从头链表追加一个槽位存放这条头 */
                ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 失败按500收尾 */
                ngx_http_close_connection(c);  /* 关连接 */
                return;  /* 结束 */
            }  /* 头槽位分配失败分支结束 */

            h->key.len = r->header_name_end - r->header_name_start;  /* 头名长度=状态机标记的指针差 */
            h->key.data = r->header_name_start;  /* 头名首址 */
            h->key.data[h->key.len] = '\0';  /* 补'\0'：便于字符串比较与日志 */

            h->value.len = r->header_end - r->header_start;  /* 头值长度 */
            h->value.data = r->header_start;  /* 头值首址 */
            h->value.data[h->value.len] = '\0';  /* 补'\0' */

            if (h->key.len == sizeof("Cookie") - 1  /* Cookie头可出现多条(RFC6265允许)，需全数收集 */
                && ngx_strcasecmp(h->key.data, "Cookie") == 0)
            {  /* 是Cookie头 */
                if (!(cookie = ngx_array_push(&r->headers_in.cookies))) {  /* 在cookies数组中追加一个指针槽 */
                    ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 失败按500收尾 */
                    ngx_http_close_connection(c);  /* 关连接 */
                    return;  /* 结束 */
                }  /* Cookie槽位分配失败分支结束 */

                *cookie = h;  /* 槽位指向这条Cookie头：模块按数组遍历全部Cookie */

            } else {  /* 普通头：按登记表查是否需要挂到headers_in的专属字段 */

                for (i = 0; ngx_http_headers_in[i].name.len != 0; i++) {  /* 遍历登记表直到哨兵项 */
                    if (ngx_http_headers_in[i].name.len != h->key.len) {  /* 长度不同必不匹配：先比长度省一次逐字符比较 */
                        continue;  /* 看下一项 */
                    }  /* 长度不等跳过判断结束 */

                    if (ngx_strcasecmp(ngx_http_headers_in[i].name.data,  /* 不区分大小写比较头名 */
                                       h->key.data) == 0)
                    {  /* 头名命中登记表 */
                        *((ngx_table_elt_t **) ((char *) &r->headers_in  /* 按登记的成员偏移把指针写入headers_in对应字段 */
                                         + ngx_http_headers_in[i].offset)) = h;
                        break;  /* 登记完毕，无需再查 */
                    }  /* 头名命中登记分支结束 */
                }  /* 登记表遍历结束 */
            }  /* 普通头登记分支结束 */

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug输出这条头 */
                           "http header: \"%s: %s\"",
                           h->key.data, h->value.data);

            continue;  /* 继续解析下一条头行(粘包时缓冲里可能还有多行) */

        } else if (rc == NGX_HTTP_PARSE_HEADER_DONE) {  /* 收到头段结束的空行(CRLF)：整个头收齐 */

            /* a whole header has been parsed successfully */  /* 原注释 */

            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug日志 */
                           "http header done");

            r->http_state = NGX_HTTP_PROCESS_REQUEST_STATE;  /* 状态切到"处理请求"：读取阶段结束 */

            rc = ngx_http_process_request_header(r);  /* 头部语义校验：Host/Content-Length/Connection等(见下函数) */

            if (rc != NGX_OK) {  /* 校验发现客户端错误 */
                ngx_http_client_error(r, rc, NGX_HTTP_BAD_REQUEST);  /* 记日志并按400类终结 */
                return;  /* 结束 */
            }  /* 语义校验失败分支结束 */

            if (rev->timer_set) {  /* 读定时器还在(client_header_timeout) */
                ngx_del_timer(rev);  /* 头已收齐：摘除定时器，红黑树节点归还 */
            }  /* 摘读定时器判断结束 */

#if (NGX_STAT_STUB)  /* 统计桩 */
            (*ngx_stat_reading)--;  /* reading计数-1 */
            r->stat_reading = 0;  /* 清请求上的阅读标志 */
            (*ngx_stat_writing)++;  /* writing计数+1：进入写响应阶段 */
            r->stat_writing = 1;  /* 置请求上的写作标志 */
#endif  /* 统计桩结束 */

            rev->event_handler = ngx_http_block_read;  /* 读handler换为"屏蔽读"：处理/响应期间不接受新数据 */
            ngx_http_handler(r);  /* 进入HTTP处理阶段引擎：按location配置跑各phase handler */
            return;  /* 移交完毕 */

        } else if (rc != NGX_AGAIN) {  /* 状态机返回头部语法错误码 */

            /* there was error while a header line parsing */  /* 原注释 */

#if (NGX_DEBUG)  /* 仅debug构建：把非法头打出来便于定位 */
            if (rc == NGX_HTTP_PARSE_INVALID_HEADER  /* 错误确为非法头且开了HTTP debug日志 */
                && (rev->log->log_level & NGX_LOG_DEBUG_HTTP))
            {
                u_char *p;  /* 扫描指针 */
                for (p = r->header_name_start;  /* 从头名起点扫描到行尾LF */
                     p < r->header_in->last - 1;
                     p++)
                {
                    if (*p == LF) {  /* 找到行结束符 */
                        break;  /* 停下 */
                    }  /* 找到LF分支结束 */
                }  /* 非法头行尾扫描结束 */
                *p = '\0';  /* 截断成完整字符串 */
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, rev->log, 0,  /* debug输出非法头 */
                               "http invalid header: \"%s\"",
                               r->header_name_start);
            }  /* 非法头debug输出结束 */
#endif  /* NGX_DEBUG条件编译结束 */

            ngx_http_client_error(r, rc, NGX_HTTP_BAD_REQUEST);  /* 按400记录并终结请求 */
            return;  /* 结束 */
        }  /* 头部语法错误分支结束 */

        /* NGX_AGAIN: a header line parsing is still not complete */  /* 原注释：当前头行尚未收齐(TCP拆包)，回到循环头继续recv */

    }  /* 头部解析主循环结束 */
}  /* 函数结束 */


/*
 * ngx_http_read_request_header()：解析循环的"取数据"环节。优先消费header_in
 * 中尚未解析的存量字节；缓冲空且rev->ready(内核已有数据)才真正recv。recv返回
 * NGX_AGAIN=内核接收缓冲区被读空而TCP对端还有后续字节未达——这正是拆包场景，
 * 挂起等下一次可读事件并补挂client_header_timeout；返回0=对端发了FIN
 * (客户端过早关闭)，按400收尾。返回值=可供状态机消费的字节数。
 */
static ssize_t ngx_http_read_request_header(ngx_http_request_t *r)
{
    ssize_t                    n;  /* recv返回值 */
    ngx_event_t               *rev;  /* 读事件 */
    ngx_http_core_srv_conf_t  *cscf;  /* server配置：取client_header_timeout */

    rev = r->connection->read;  /* 取读事件 */

    n = r->header_in->last - r->header_in->pos;  /* 计算缓冲内未消费的存量字节 */

    if (n > 0) {  /* 还有上次recv来但未解析完的数据(TCP粘包) */
        return n;  /* 直接返回存量：能不进内核就不进内核，减少系统调用 */
    }  /* 存量数据分支结束 */

    if (!rev->ready) {  /* 事件标记未就绪：内核接收缓冲区当前无数据 */
        return NGX_AGAIN;  /* 通知上层挂起：等下一次可读事件再继续 */
    }  /* 内核无数据分支结束 */

    n = r->connection->recv(r->connection, r->header_in->last,  /* 真正recv：从last起写到缓冲尾(HTTPS时此函数已被换成SSL解密读) */
                            r->header_in->end - r->header_in->last);

    if (n == NGX_AGAIN) {  /* 内核缓冲区读空：剩余请求字节还在网络途中(拆包) */
        if (!r->header_timeout_set) {  /* 尚未挂过头接收定时器 */
            cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);  /* 取server配置 */
            ngx_add_timer(rev, cscf->client_header_timeout);  /* 挂client_header_timeout：等不到后续字节到点关连接 */
            r->header_timeout_set = 1;  /* 标记已挂：避免重复挂导致红黑树节点泄漏 */
        }  /* 挂头接收定时器判断结束 */

        if (ngx_handle_read_event(rev, 0) == NGX_ERROR) {  /* 确保读事件登记在epoll：等待对端后续数据 */
            ngx_http_close_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 注册失败按500收尾 */
            ngx_http_close_connection(r->connection);  /* 关连接 */
            return NGX_ERROR;  /* 报错 */
        }  /* 读事件登记失败分支结束 */

        return NGX_AGAIN;  /* 上层据此挂起，从r->state断点等待续析 */
    }  /* 拆包挂起分支结束 */

    if (n == 0) {  /* recv返回0：对端发送FIN(四次挥手第一步)，客户端过早关闭 */
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,  /* INFO级记录：属客户端行为非服务器故障 */
                      "client closed prematurely connection");
    }  /* 过早关闭记录判断结束 */

    if (n == 0 || n == NGX_ERROR) {  /* 对端关闭或读错误 */
        ngx_http_close_request(r, NGX_HTTP_BAD_REQUEST);  /* 请求按400收尾 */
        ngx_http_close_connection(r->connection);  /* 关连接 */
        return NGX_ERROR;  /* 报错 */
    }  /* 对端关闭或读错误分支结束 */

    r->header_in->last += n;  /* 推进写游标：新到的n字节纳入可解析范围 */

    return n;  /* 返回本次可消费字节数 */
}  /* ngx_http_read_request_header 函数结束 */


/*
 * ngx_http_alloc_large_header_buffer()：默认头缓冲装不下时换large_client_header_buffers
 * 大缓冲。精髓在于"断点搬移"：状态机的各定位指针(request_start/uri_start等)
 * 都是old缓冲内的绝对地址，缓冲一换全部失效——按"新址=新基址+(旧址-旧基址)"
 * 统一平移，使解析能从新缓冲精确续析，这是指针式流解析器换缓冲的标准手法。
 * request_line=1表示请求行未完成，否则是头部行未完成。返回DECLINED=超限报错。
 */
static ngx_int_t ngx_http_alloc_large_header_buffer(ngx_http_request_t *r,
                                                    ngx_uint_t request_line)  /* request_line=1：换缓冲时请求行尚未解析完 */
{
    u_char                    *old, *new;  /* old=旧缓冲中未完成行起点; new=新缓冲首址 */
    ngx_buf_t                 *b;  /* 新分配的大缓冲 */
    ngx_http_connection_t     *hc;  /* 连接级HTTP状态：大缓冲free/busy链挂在这里 */
    ngx_http_core_srv_conf_t  *cscf;  /* server配置：取大缓冲数量与大小 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug日志 */
                   "http alloc large header buffer");

    if (request_line && r->state == 0) {  /* 换缓冲因"请求行超界"触发，但状态机在起始态 */

        /* the client fills up the buffer with "\r\n" */  /* 原注释：缓冲里其实全是请求间的多余CRLF */

        r->header_in->pos = r->header_in->start;  /* 读游标拨回起点 */
        r->header_in->last = r->header_in->start;  /* 写游标拨回起点：清空缓冲重新接收即可，无需换大缓冲 */

        return NGX_OK;  /* 原地重置完成 */
    }  /* 多余CRLF重置分支结束 */

    old = request_line ? r->request_start : r->header_name_start;  /* 记下未完成行在旧缓冲的起点：搬移的基准点 */

    cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);  /* 取server配置 */

    if (r->state != 0  /* 状态机已深入(正在半行中间) */
        && (size_t) (r->header_in->pos - old)  /* 且已消费的部分就超过了单块大缓冲容量 */
                                     >= cscf->large_client_header_buffers.size)
    {
        return NGX_DECLINED;  /* 单行超长：上层报414/400，不给分配 */
    }  /* 单行超长判断结束 */

    hc = r->http_connection;  /* 取连接级大缓冲管理结构 */

    if (hc->nfree) {  /* free链有空闲大缓冲(前几个请求用剩的) */
        b = hc->free[--hc->nfree];  /* 弹出链尾一块：缓冲复用避免反复分配 */

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug输出复用缓冲信息 */
                       "http large header free: " PTR_FMT " " SIZE_T_FMT,
                       b->pos, b->end - b->last);

    } else if (hc->nbusy < cscf->large_client_header_buffers.num) {  /* free空但busy数未达上限：允许新分配 */

        if (hc->busy == NULL) {  /* busy指针数组还没建 */
            hc->busy = ngx_palloc(r->connection->pool,  /* 按上限数量分配指针数组(挂连接池：随连接释放) */
                  cscf->large_client_header_buffers.num * sizeof(ngx_buf_t *));
            if (hc->busy == NULL) {  /* 分配失败 */
                return NGX_ERROR;  /* 上层按500收尾 */
            }  /* busy数组分配失败分支结束 */
        }  /* busy数组未建判断结束 */

        b = ngx_create_temp_buf(r->connection->pool,  /* 新建一块大缓冲(挂连接池) */
                                cscf->large_client_header_buffers.size);
        if (b == NULL) {  /* 分配失败 */
            return NGX_ERROR;  /* 上层按500收尾 */
        }  /* 大缓冲分配失败分支结束 */

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug输出新缓冲信息 */
                       "http large header alloc: " PTR_FMT " " SIZE_T_FMT,
                       b->pos, b->end - b->last);

    } else {  /* 分配不到大缓冲的 else：交上层报 414 */
        return NGX_DECLINED;  /* busy也满了：整个头部超限，上层报414/400 */
    }  /* 大缓冲配额用尽分支结束 */

    hc->busy[hc->nbusy++] = b;  /* 新缓冲进busy链：本请求用完后归还free链 */

    if (r->state == 0) {  /* 状态机在行边界(上一行刚解析完) */
        /*
         * r->state == 0 means that a header line was parsed successfully
         * and we do not need to copy incomplete header line and
         * to relocate the parser header pointers
         */  /* 原注释：无需搬移任何指针 */

        r->header_in = b;  /* 直接切换接收缓冲为新大缓冲 */

        return NGX_OK;  /* 完成 */
    }  /* 行边界免搬移分支结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug输出需搬移的字节数 */
                   "http large header copy: %d", r->header_in->pos - old);

    new = b->start;  /* 新缓冲基址 */

    ngx_memcpy(new, old, r->header_in->pos - old);  /* 把旧缓冲中未解析完的半行搬到新缓冲开头 */

    b->pos = new + (r->header_in->pos - old);  /* 读游标同步平移：已搬字节视为已消费 */
    b->last = new + (r->header_in->pos - old);  /* 写游标同步：后续recv接着写在其后 */

    if (request_line) {  /* 请求行断在半路：平移请求行相关全部定位指针 */
        r->request_start = new;  /* 请求行起点=搬移基准=new缓冲头 */

        if (r->request_end) {  /* 请求行末指针若已设置 */
            r->request_end = new + (r->request_end - old);  /* 按偏移平移 */
        }  /* request_end 平移判断结束 */

        r->method_end = new + (r->method_end - old);  /* 方法末址平移 */

        r->uri_start = new + (r->uri_start - old);  /* URI起点平移 */
        r->uri_end = new + (r->uri_end - old);  /* URI终点平移 */

        if (r->schema_start) {  /* 绝对URI的scheme起点若已标记 */
            r->schema_start = new + (r->schema_start - old);  /* 平移 */
            r->schema_end = new + (r->schema_end - old);  /* 平移 */
        }  /* scheme 指针平移判断结束 */

        if (r->host_start) {  /* 主机名起点若已标记 */
            r->host_start = new + (r->host_start - old);  /* 平移 */
            r->host_end = new + (r->host_end - old);  /* 平移 */
        }  /* host 指针平移判断结束 */

        if (r->port_start) {  /* 端口起点若已标记 */
            r->port_start = new + (r->port_start - old);  /* 平移 */
            r->port_end = new + (r->port_end - old);  /* 平移 */
        }  /* port 指针平移判断结束 */

        if (r->uri_ext) {  /* 扩展名起点若已标记 */
            r->uri_ext = new + (r->uri_ext - old);  /* 平移 */
        }  /* uri_ext 平移判断结束 */

        if (r->args_start) {  /* 参数起点若已标记 */
            r->args_start = new + (r->args_start - old);  /* 平移 */
        }  /* args_start 平移判断结束 */

    } else {  /* 头部行断在半路：只需平移头名/头值四个指针 */
        r->header_name_start = new;  /* 头名起点=新缓冲头 */
        r->header_name_end = new + (r->header_name_end - old);  /* 头名终点平移 */
        r->header_start = new + (r->header_start - old);  /* 头值起点平移 */
        r->header_end = new + (r->header_end - old);  /* 头值终点平移 */
    }  /* 头部行指针平移分支结束 */

    r->header_in = b;  /* 接收缓冲切换为新大缓冲 */

    return NGX_OK;  /* 完成：解析循环将从新缓冲断点续析 */
}  /* ngx_http_alloc_large_header_buffer 函数结束 */


/*
 * ngx_http_process_request_header()：头部收齐后的"语义检查+二级路由"。语法正确
 * 不代表语义合法：此处校验Host(HTTP/1.1强制，虚拟主机按它完成server级定位)、
 * Content-Length(数字合法性、POST必须带)、Connection(close/keep-alive判定连接
 * 复用方式)，并顺带识别UA中的主流浏览器quirk。所有校验失败都以NGX_HTTP_PARSE_*
 * 错误码返回，由上层映射为400/411类响应。
 */
static ngx_int_t ngx_http_process_request_header(ngx_http_request_t *r)
{
    u_char                    *ua, *user_agent;  /* ua=命中子串位置; user_agent=UA串首址 */
    size_t                     len;  /* Host名长度(去端口) */
    ngx_uint_t                 i;  /* 虚拟主机表下标 */
    ngx_http_server_name_t    *name;  /* server_name表元素 */
    ngx_http_core_srv_conf_t  *cscf;  /* server配置 */
    ngx_http_core_loc_conf_t  *clcf;  /* location配置(切日志用) */

    if (r->headers_in.host) {  /* 客户端带了Host头：虚拟主机二级定位的依据 */
        for (len = 0; len < r->headers_in.host->value.len; len++) {  /* 扫描Host值找':' */
            if (r->headers_in.host->value.data[len] == ':') {  /* 遇到端口分隔符 */
                break;  /* 主机名部分到此为止 */
            }  /* 端口分隔符分支结束 */
        }  /* Host端口扫描结束 */
        r->headers_in.host_name_len = len;  /* 记纯主机名长度(host:port去掉:port)：后续error_page/绝对重定向用 */

        /* find the name based server configuration */  /* 原注释：按主机名定位server配置 */

        name = r->virtual_names->elts;  /* 取该监听地址上的虚拟主机名表 */
        for (i = 0; i < r->virtual_names->nelts; i++) {  /* 逐个候选server_name比较 */
            if (r->headers_in.host_name_len != name[i].name.len) {  /* 长度不等直接跳过：先比长度减少逐字符比较 */
                continue;  /* 看下一个 */
            }  /* 长度不等跳过判断结束 */

            if (ngx_strncasecmp(r->headers_in.host->value.data,  /* 不区分大小写比较主机名(域名大小写不敏感) */
                                name[i].name.data,
                                r->headers_in.host_name_len) == 0)
            {  /* Host命中某虚拟主机 */
                r->srv_conf = name[i].core_srv_conf->ctx->srv_conf;  /* 切换server级配置到命中server */
                r->loc_conf = name[i].core_srv_conf->ctx->loc_conf;  /* 切换location级配置 */
                r->server_name = &name[i].name;  /* 记下命中的server_name */

                clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取新server的core配置 */
                r->connection->log->file = clcf->err_log->file;  /* 错误日志文件随之切换 */
                if (!(r->connection->log->log_level & NGX_LOG_DEBUG_CONNECTION))  /* 非debug连接时 */
                {
                    r->connection->log->log_level = clcf->err_log->log_level;  /* 日志级别切换为新server配置 */
                }  /* 日志级别切换判断结束 */

                break;  /* 定位完成 */
            }  /* Host命中分支结束 */
        }  /* 虚拟主机名遍历结束 */

        if (i == r->virtual_names->nelts) {  /* 循环走完没命中：没有任何server_name与Host匹配 */
            cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);  /* 取默认server配置 */

            if (cscf->restrict_host_names != NGX_HTTP_RESTRICT_HOST_OFF) {  /* 且配置要求拒绝未知Host */
                return NGX_HTTP_PARSE_INVALID_HOST;  /* 报非法Host错误 */
            }  /* 拒绝未知Host判断结束 */
        }  /* Host未命中判断结束 */

    } else {  /* 没带Host头 */
        if (r->http_version > NGX_HTTP_VERSION_10) {  /* HTTP/1.1协议强制要求Host(RFC2616 14.23) */
            return NGX_HTTP_PARSE_NO_HOST_HEADER;  /* 报缺Host错误(400) */
        }  /* 1.1缺Host分支结束 */
        r->headers_in.host_name_len = 0;  /* 1.0/0.9允许无Host：长度记0 */
    }  /* 无Host分支结束 */

    if (r->headers_in.content_length) {  /* 带Content-Length头：说明有请求体 */
        r->headers_in.content_length_n =  /* 字符串转数值：请求体读取/丢弃都以此为准 */
                             ngx_atoi(r->headers_in.content_length->value.data,  /* 语句续行：字符串 Content-Length 转数值 */
                                      r->headers_in.content_length->value.len);

        if (r->headers_in.content_length_n == NGX_ERROR) {  /* 不是合法十进制数 */
            return NGX_HTTP_PARSE_INVALID_CL_HEADER;  /* 报非法Content-Length(400) */
        }  /* 非法Content-Length判断结束 */
    }  /* Content-Length 处理结束 */

    if (r->method == NGX_HTTP_POST && r->headers_in.content_length_n <= 0) {  /* POST但无有效长度声明 */
        return NGX_HTTP_PARSE_POST_WO_CL_HEADER;  /* 报错：无长度定界则请求体无法读取 */
    }  /* POST缺长度判断结束 */

    if (r->plain_http) {  /* 之前嗅探发现客户端向https端口发明文HTTP */
        return NGX_HTTP_PARSE_HTTP_TO_HTTPS;  /* 报协议错配(映射为495类) */
    }  /* 明文发https判断结束 */

    if (r->headers_in.connection) {  /* 带Connection头：决定TCP连接后续处置 */
        if (r->headers_in.connection->value.len == 5  /* 值为"close"：客户端要求响应完就挥手 */
            && ngx_strcasecmp(r->headers_in.connection->value.data, "close")
                                                                          == 0)
        {
            r->headers_in.connection_type = NGX_HTTP_CONNECTION_CLOSE;  /* 标记非持久连接 */

        } else if (r->headers_in.connection->value.len == 10  /* 值为"keep-alive"：要求连接复用 */
                   && ngx_strcasecmp(r->headers_in.connection->value.data,
                                                            "keep-alive") == 0)
        {
            r->headers_in.connection_type = NGX_HTTP_CONNECTION_KEEP_ALIVE;  /* 标记持久连接 */

            if (r->headers_in.keep_alive) {  /* 还带了Keep-Alive参数头 */
                r->headers_in.keep_alive_n =  /* 解析timeout参数(客户端建议的空闲时长) */
                                 ngx_atoi(r->headers_in.keep_alive->value.data,  /* 语句续行：Keep-Alive timeout 参数转数值 */
                                          r->headers_in.keep_alive->value.len);
            }  /* Keep-Alive参数解析结束 */
        }  /* keep-alive 分支结束 */
    }  /* Connection 头处理结束 */

    if (r->headers_in.user_agent) {  /* 带User-Agent：识别浏览器quirk */

        /*
         * check some widespread browsers while the headers are still
         * in CPU cache
         */  /* 原注释：趁头部字节还在CPU缓存里赶紧比对，提高缓存命中 */

        user_agent = r->headers_in.user_agent->value.data;  /* UA串首址 */

        ua = (u_char *) ngx_strstr(user_agent, "MSIE");  /* 查找MSIE子串 */

        if (ua && ua + 8 < user_agent + r->headers_in.user_agent->value.len) {  /* 命中且后续还有足够字节可安全读 */

            r->headers_in.msie = 1;  /* 标记IE：IE有若干HTTP实现quirk需兼容 */

            if (ua[4] == ' ' && ua[5] == '4' && ua[6] == '.') {  /* 形如"MSIE 4."：特指IE4 */
                r->headers_in.msie4 = 1;  /* 单独标记IE4(quirk更多) */
            }  /* IE4识别判断结束 */

#if 0  /* 禁用代码 */
            /* MSIE ignores the SSL "close notify" alert */  /* 原注释 */

            ngx_ssl_set_nosendshut(r->connection->ssl);  /* 对IE不发SSL关闭告警(禁用) */
#endif  /* 禁用块结束 */
        }  /* MSIE处理结束 */

        if (ngx_strstr(user_agent, "Opera")) {  /* Opera的UA常伪装成MSIE，需再查Opera字样 */
            r->headers_in.opera = 1;  /* 标记Opera */
            r->headers_in.msie = 0;  /* 撤销MSIE标记 */
            r->headers_in.msie4 = 0;  /* 撤销MSIE4标记 */
        }  /* Opera识别结束 */

        if (!r->headers_in.msie && !r->headers_in.opera) {  /* 非IE非Opera：再识别两类 */

            if (ngx_strstr(user_agent, "Gecko/")) {  /* Gecko内核(Firefox等) */
                r->headers_in.gecko = 1;  /* 标记Gecko */

            } else if (ngx_strstr(user_agent, "Konqueror")) {  /* KDE的Konqueror */
                r->headers_in.konqueror = 1;  /* 标记Konqueror */
            }  /* Konqueror分支结束 */
        }  /* Gecko/Konqueror识别结束 */
    }  /* User-Agent识别结束 */

    return NGX_OK;  /* 全部校验通过：请求可以进入处理阶段 */
}  /* ngx_http_process_request_header 函数结束 */


/*
 * ngx_http_finalize_request()：请求生命周期各阶段handler的统一收口。按返回码
 * 分流：>=500等特殊响应码走ngx_http_special_response生成错误页后重入本函数；
 * NGX_ERROR直接关连接；NGX_AGAIN说明响应未发完转写事件；正常完成则按配置
 * 选择keepalive复用(免重新三次握手)、lingering关闭(读净残留)或立即关闭。
 * 是"一条TCP连接上HTTP请求生命周期"的换轨点。
 */
void ngx_http_finalize_request(ngx_http_request_t *r, int rc)
{
    ngx_http_core_loc_conf_t  *clcf;  /* location配置：keepalive/lingering参数 */

    /* r can be already destroyed when rc == NGX_DONE */  /* 原注释 */

    if (rc == NGX_DONE || r->main) {  /* DONE=异步操作(如subrequest)仍在持有请求；r->main=子请求由主请求统一终结 */
        return;  /* 都不由这里终结：引用计数模型下"谁最后用完谁关闭" */
    }  /* DONE/子请求分支结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug记录终结码 */
                   "http finalize request: %d", rc);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {  /* 特殊响应(400/404/500等错误码区间) */

        if (r->connection->read->timer_set) {  /* 读事件定时器还在 */
            ngx_del_timer(r->connection->read);  /* 摘除：即将出错误页，不再等读 */
        }  /* 摘读定时器判断结束 */

        if (r->connection->write->timer_set) {  /* 写事件定时器还在 */
            ngx_del_timer(r->connection->write);  /* 摘除 */
        }  /* 摘写定时器判断结束 */

        if (rc == NGX_HTTP_CLIENT_CLOSED_REQUEST || r->closed) {  /* 499(客户端已关)或底层连接已坏：发不了错误页 */
            ngx_http_close_request(r, 0);  /* 直接终结请求 */
            ngx_http_close_connection(r->connection);  /* 并关闭连接 */
            return;  /* 结束 */
        }  /* 客户端已关闭分支结束 */

        ngx_http_finalize_request(r, ngx_http_special_response_handler(r, rc));  /* 生成错误响应页后重入本函数继续分流(递归深度1) */

        return;  /* 结束 */

    } else if (rc == NGX_ERROR) {  /* 处理过程发生错误 */
        ngx_http_close_request(r, 0);  /* 终结请求 */
        ngx_http_close_connection(r->connection);  /* 直接关连接：错误状态下无响应可言 */
        return;  /* 结束 */

    } else if (rc == NGX_AGAIN) {  /* 响应链还有数据没发出去(写缓冲满) */
        ngx_http_set_write_handler(r);  /* 挂到写事件上：等EPOLLOUT继续发 */
        return;  /* 结束 */
    }  /* NGX_AGAIN 分支结束 */

    if (r->connection->read->timer_set) {  /* 响应完成：读定时器(若有) */
        ngx_del_timer(r->connection->read);  /* 摘除 */
    }  /* 摘读定时器判断结束 */

    if (r->connection->write->timer_set) {  /* 写定时器(若有) */
        r->connection->write->delayed = 0;  /* 清限速延迟标志 */
        ngx_del_timer(r->connection->write);  /* 摘除 */
    }  /* 摘写定时器判断结束 */

    if (r->connection->read->pending_eof) {  /* kqueue/EPOLLRDHUP已报对端关闭：客户端不会再来请求 */
#if (NGX_KQUEUE)  /* kqueue特有的pending_eof提前感知 */
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log,  /* debug记录对端错误码 */
                       r->connection->read->kq_errno,
                       "kevent() reported about an closed connection");
#endif  /* NGX_KQUEUE条件编译结束 */
        ngx_http_close_request(r, 0);  /* 终结请求 */
        ngx_http_close_connection(r->connection);  /* 关连接：复用无意义 */
        return;  /* 结束 */
    }  /* 对端已关闭分支结束 */

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取location配置 */

    if (!ngx_terminate  /* 非master强制退出 */
         && !ngx_exiting  /* 非优雅退出中 */
         && r->keepalive != 0  /* 请求允许keepalive(客户端没要求close且协议支持) */
         && clcf->keepalive_timeout > 0)  /* 且配置了keepalive超时 */
    {
        ngx_http_set_keepalive(r);  /* 转入keepalive：TCP连接不关，等下一请求(省三次握手与慢启动爬坡) */
        return;  /* 结束 */

    } else if (r->lingering_close && clcf->lingering_timeout > 0) {  /* 需要lingering且配置允许 */
        ngx_http_set_lingering_close(r);  /* 转入lingering：先读净客户端残留数据再挥手，避免对端RST */
        return;  /* 结束 */
    }  /* lingering 分支结束 */

    ngx_http_close_request(r, 0);  /* 其余情况：终结请求 */
    ngx_http_close_connection(r->connection);  /* 并立即关闭连接 */
}  /* ngx_http_finalize_request 函数结束 */


/*
 * ngx_http_set_write_handler()：把连接"交接"给写事件。响应一次writev发不完
 * (TCP发送缓冲区满=对端来不及收/链路拥塞)时，注册ngx_http_writer为写回调并
 * 挂send_timeout定时器。此后每收到EPOLLOUT(发送缓冲区有空位)就续发，
 * send_timeout内一次都没发出去则判定对端异常，超时关闭。
 */
static void ngx_http_set_write_handler(ngx_http_request_t *r)
{
    ngx_event_t               *wev;  /* 写事件 */
    ngx_http_core_loc_conf_t  *clcf;  /* location配置：取send_timeout/send_lowat */

    wev = r->connection->write;  /* 取写事件 */
    wev->event_handler = ngx_http_writer;  /* 写回调注册为writer：EPOLLOUT到来即续发响应 */

    r->http_state = NGX_HTTP_WRITING_REQUEST_STATE;  /* 请求状态切到"写响应中" */

    if (wev->ready && wev->delayed) {  /* 写就绪但处于限速延迟：暂不登记，等延迟定时器到期再驱动 */
        return;  /* 直接返回 */
    }  /* 限速延迟判断结束 */

    clcf = ngx_http_get_module_loc_conf(r->main ? r->main : r,  /* 子请求场景一律取主请求的配置 */
                                        ngx_http_core_module);
    if (!wev->delayed) {  /* 非限速延迟状态 */
        ngx_add_timer(wev, clcf->send_timeout);  /* 挂send_timeout：持续发不出去(对端不收)到点强关 */
    }  /* 挂send_timeout判断结束 */

    wev->available = clcf->send_lowat;  /* 设置发送低水位：缓冲区空出到该字节数才通知(减少无效唤醒) */
    if (ngx_handle_write_event(wev, NGX_LOWAT_EVENT) == NGX_ERROR) {  /* 登记写事件到epoll等待EPOLLOUT */
        ngx_http_close_request(r, 0);  /* 登记失败：终结请求 */
        ngx_http_close_connection(r->connection);  /* 关连接 */
    }  /* 写事件登记失败分支结束 */

    return;  /* 结束 */
}  /* ngx_http_set_write_handler 函数结束 */


/*
 * ngx_http_writer()：写事件handler——响应"部分发送"后的续发引擎。每次
 * EPOLLOUT(内核发送缓冲区有空位，即对端TCP窗口又有余量)触发一次output_filter
 * 尝试把剩余响应推下去：仍发不完(NGX_AGAIN)则继续等EPOLLOUT并续挂
 * send_timeout；发完则finalize。timedout分支处理限速延迟恢复：delayed状态下
 * 超时不是错误，只是限速窗口到期，重新登记继续即可。
 */
void ngx_http_writer(ngx_event_t *wev)
{
    int                        rc;  /* output_filter返回码 */
    ngx_connection_t          *c;  /* 底层连接 */
    ngx_http_request_t        *r;  /* 请求对象 */
    ngx_http_core_loc_conf_t  *clcf;  /* location配置：send_timeout/send_lowat */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, wev->log, 0, "http writer handler");  /* debug日志 */

    c = wev->data;  /* 事件回指连接 */
    r = c->data;  /* 连接回指请求 */

    if (wev->timedout) {  /* 定时器到点：send_timeout或限速延迟到期 */
        if (!wev->delayed) {  /* 非延迟状态=真正的send_timeout：整个超时期间一次都没写出去 */
            ngx_http_client_error(r, 0, NGX_HTTP_REQUEST_TIME_OUT);  /* 按408终结：对端持续不收(TCP发送缓冲长期满) */
            return;  /* 结束 */
        }  /* send_timeout分支结束 */

        wev->timedout = 0;  /* 清超时标志：限速延迟到期不算错误 */
        wev->delayed = 0;  /* 解除延迟状态 */

        if (!wev->ready) {  /* 写事件仍不就绪：发送缓冲区还没空出来 */
            clcf = ngx_http_get_module_loc_conf(r->main ? r->main : r,  /* 取主请求配置 */
                                                ngx_http_core_module);
            ngx_add_timer(wev, clcf->send_timeout);  /* 重新挂send_timeout继续等 */

            wev->available = clcf->send_lowat;  /* 恢复低水位设置 */

            if (ngx_handle_write_event(wev, NGX_LOWAT_EVENT) == NGX_ERROR) {  /* 重新登记写事件 */
                ngx_http_close_request(r, 0);  /* 失败终结请求 */
                ngx_http_close_connection(r->connection);  /* 关连接 */
            }  /* 写事件登记失败分支结束 */

            return;  /* 等待EPOLLOUT */
        }  /* 写不就绪分支结束 */

    } else {  /* 写事件已就绪的 else：执行发送 */
        if (wev->delayed) {  /* 未超时但处于限速延迟：本次EPOLLOUT无效，不许发 */
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, wev->log, 0,  /* debug日志 */
                           "http writer delayed");

            clcf = ngx_http_get_module_loc_conf(r->main ? r->main : r,  /* 取主请求配置 */
                                                ngx_http_core_module);
            wev->available = clcf->send_lowat;  /* 维持低水位 */

            if (ngx_handle_write_event(wev, NGX_LOWAT_EVENT) == NGX_ERROR) {  /* 重新登记：继续等延迟定时器 */
                ngx_http_close_request(r, 0);  /* 失败终结 */
                ngx_http_close_connection(r->connection);  /* 关连接 */
            }  /* 写事件登记失败分支结束 */

            return;  /* 不发送 */
        }  /* 限速延迟分支结束 */
    }  /* 非超时分支结束 */

    rc = ngx_http_output_filter(r, NULL);  /* 推动输出过滤链续发剩余响应(NULL=只flush不发新数据) */

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug记录过滤链结果 */
                  "http writer output filter: %d", rc);

    if (rc == NGX_AGAIN) {  /* 仍未发完：发送缓冲区又满了 */
        clcf = ngx_http_get_module_loc_conf(r->main ? r->main : r,  /* 取主请求配置 */
                                            ngx_http_core_module);
        if (!wev->ready && !wev->delayed) {  /* 写不就绪且未限速：需要挂超时兜底 */
            ngx_add_timer(wev, clcf->send_timeout);  /* 续挂send_timeout：对端一直不收则到点关连接 */
        }  /* 挂send_timeout判断结束 */

        wev->available = clcf->send_lowat;  /* 设置低水位 */

        if (ngx_handle_write_event(wev, NGX_LOWAT_EVENT) == NGX_ERROR) {  /* 重新登记写事件等下一轮EPOLLOUT */
            ngx_http_close_request(r, 0);  /* 失败终结 */
            ngx_http_close_connection(r->connection);  /* 关连接 */
        }  /* 写事件登记失败分支结束 */

        return;  /* 挂起：写事件驱动下一轮续发 */
    }  /* 未发完分支结束 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "http writer done");  /* debug：响应发送完毕 */

    ngx_http_finalize_request(r, rc);  /* 发完：交回finalize分流(keepalive/lingering/关闭) */
}  /* ngx_http_writer 函数结束 */


/*
 * ngx_http_block_read()："屏蔽读"handler。请求头收齐进入处理/发送阶段后，
 * 不希望客户端此间发来的任何字节(如提前抢跑的请求体)触发读回调扰乱状态机。
 * 水平触发(level)模式下若事件仍在epoll注册表里会持续报告，故直接把读事件
 * 从epoll摘除；边缘触发模式摘除后自然静默，无需动作。
 */
static void ngx_http_block_read(ngx_event_t *rev)
{
    ngx_connection_t          *c;  /* 连接(仅错误时取用) */
    ngx_http_request_t        *r;  /* 请求(仅错误时取用) */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0, "http read blocked");  /* debug日志 */

    /* aio does not call this handler */  /* 原注释：aio模式下不会进到这里 */

    if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && rev->active) {  /* 水平触发且读事件还挂在事件机制上 */
        if (ngx_del_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {  /* 从epoll摘除读事件：处理期间静默 */
            c = rev->data;  /* 摘除失败才取上下文 */
            r = c->data;  /* 由事件反取连接、再取请求对象 */
            ngx_http_close_request(r, 0);  /* 终结请求 */
            ngx_http_close_connection(c);  /* 关连接 */
        }  /* 摘读事件失败分支结束 */
    }  /* 水平触发判断结束 */
}  /* ngx_http_block_read 函数结束 */


/*
 * ngx_http_discard_body()：模块声明不关心请求体时，把体字节"读走扔掉"。
 * 若不读走就直接响应，这些字节会被客户端当作TCP流上未确认的数据不断重传，
 * 甚至污染下一请求(keepalive时)。先就地消化header_in里的存量，不够再挂
 * 读事件循环丢弃。
 */
ngx_int_t ngx_http_discard_body(ngx_http_request_t *r)
{
    ssize_t       size;  /* 头缓冲中存量字节数 */
    ngx_event_t  *rev;  /* 读事件 */

    rev = r->connection->read;  /* 取读事件 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0, "http set discard body");  /* debug日志 */

    if (rev->timer_set) {  /* 读定时器还挂着 */
        ngx_del_timer(rev);  /* 摘除：丢弃阶段按丢弃逻辑自行管理 */
    }  /* 摘读定时器判断结束 */

    if (r->headers_in.content_length_n <= 0) {  /* 无请求体(长度<=0) */
        return NGX_OK;  /* 无事可做 */
    }  /* 无请求体分支结束 */

    size = r->header_in->last - r->header_in->pos;  /* 头缓冲中尚未消费的存量 */

    if (size) {  /* 有存量：其中可能已捎带请求体前缀(TCP粘包) */
        if (r->headers_in.content_length_n > size) {  /* 存量还不够抵扣全部体长 */
            r->headers_in.content_length_n -= size;  /* 存量字节全部视为体前缀作废，剩余体长相应减少 */

        } else {  /* 存量足以覆盖全部请求体 */
            r->header_in->pos += r->headers_in.content_length_n;  /* 读游标直接越过体部分 */
            r->headers_in.content_length_n = 0;  /* 体长清零 */
            return NGX_OK;  /* 丢弃完成，无需再读socket */
        }  /* 存量足够分支结束 */
    }  /* 存量消化判断结束 */

    rev->event_handler = ngx_http_read_discarded_body_event;  /* 读回调换成丢弃事件处理 */

    if (ngx_handle_level_read_event(rev) == NGX_ERROR) {  /* 以水平触发登记读事件：体字节逐批到来 */
        return NGX_HTTP_INTERNAL_SERVER_ERROR;  /* 登记失败报500 */
    }  /* 读事件登记失败分支结束 */

    return ngx_http_read_discarded_body(r);  /* 立即尝试丢弃一轮：缓冲里可能已有数据 */
}  /* ngx_http_discard_body 函数结束 */


/*
 * ngx_http_read_discarded_body_event()：丢弃阶段的读事件回调。每来一批体
 * 字节读走扔掉一批；内核读空(NGX_AGAIN)则重新登记等下一批，直到按
 * Content-Length计数丢弃完毕。
 */
static void ngx_http_read_discarded_body_event(ngx_event_t *rev)
{
    ngx_int_t            rc;  /* 丢弃结果 */
    ngx_connection_t    *c;  /* 连接 */
    ngx_http_request_t  *r;  /* 请求 */

    c = rev->data;  /* 事件回指连接 */
    r = c->data;  /* 连接回指请求 */

    rc = ngx_http_read_discarded_body(r);  /* 读走并扔掉一批体字节 */

    if (rc == NGX_AGAIN) {  /* 内核读空：本轮到此 */
        if (ngx_handle_level_read_event(rev) == NGX_ERROR) {  /* 重新登记读事件等下一批 */
            ngx_http_close_request(r, rc);  /* 失败终结请求 */
            ngx_http_close_connection(c);  /* 关连接 */
            return;  /* 结束 */
        }  /* 读事件登记失败分支结束 */
    }  /* 内核读空分支结束 */

    if (rc != NGX_OK) {  /* 非AGAIN且非OK=出错或丢弃完毕后的错误路径 */
        ngx_http_close_request(r, rc);  /* 终结请求 */
        ngx_http_close_connection(c);  /* 关连接 */
    }  /* 出错收尾判断结束 */
}  /* ngx_http_read_discarded_body_event 函数结束 */


/*
 * ngx_http_read_discarded_body()：单轮丢弃执行体。用栈上小缓冲recv进数据后
 * 原地扔掉(不进内存池)，按Content-Length递减计数；读完(计数为0)返回OK，
 * 内核读空返回AGAIN。注意recv出错时请求已带响应码，属客户端问题，忽略之。
 */
static ngx_int_t ngx_http_read_discarded_body(ngx_http_request_t *r)
{
    ssize_t  size, n;  /* size=本轮欲读字节数; n=实际读到 */
    u_char   buffer[NGX_HTTP_DISCARD_BUFFER_SIZE];  /* 栈上丢弃缓冲：数据读进来即扔，不占堆 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* debug日志 */
                   "http read discarded body");

    if (r->headers_in.content_length_n == 0) {  /* 计数到0：请求体已全部丢弃 */
        return NGX_OK;  /* 完成 */
    }  /* 丢弃完成判断结束 */


    size = r->headers_in.content_length_n;  /* 本轮想读的量=剩余体长 */

    if (size > NGX_HTTP_DISCARD_BUFFER_SIZE) {  /* 超过丢弃缓冲容量 */
        size = NGX_HTTP_DISCARD_BUFFER_SIZE;  /* 截断为缓冲大小：丢弃无需贪多 */
    }  /* 截断为缓冲大小判断结束 */

    n = r->connection->recv(r->connection, buffer, size);  /* recv读入：数据将直接作废 */

    if (n == NGX_ERROR) {  /* 读出错(如对端RST) */

        r->closed = 1;  /* 标记连接已坏：后续finalize不再尝试发数据 */

        /*
         * if a client request body is discarded then we already set
         * some HTTP response code for client and we can ignore the error
         */  /* 原注释：响应码已定，客户端又跑了，错误无需上报 */

        return NGX_OK;  /* 视为丢弃完成 */
    }  /* 读出错分支结束 */

    if (n == NGX_AGAIN) {  /* 内核缓冲区读空：剩余体字节尚未到达(拆包) */
        return NGX_AGAIN;  /* 上层重新登记读事件等待 */
    }  /* 拆包挂起分支结束 */

    r->headers_in.content_length_n -= n;  /* 递减剩余体长：n字节已被读走扔掉 */

    return NGX_OK;  /* 本轮成功 */
}  /* ngx_http_read_discarded_body 函数结束 */


/*
 * ngx_http_set_keepalive()：响应发完且客户端要求keepalive时，让TCP连接"退役
 * 待命"而非关闭。要点：1) 连接复用省去下次请求的TCP三次握手与慢启动爬坡；
 * 2) 头缓冲中未消费的字节=客户端已抢跑的下一请求(pipeline)，须原样保留；
 * 3) 请求级内存池此时即可销毁归还，c->buffer/hc等连接级资源跨请求复用；
 * 4) 挂keepalive_timeout定时器，空闲超时主动挥手。此后读事件交给
 * ngx_http_keepalive_handler等下一请求。
 */
static void ngx_http_set_keepalive(ngx_http_request_t *r)
{
    ngx_int_t                  i;  /* 循环下标 */
    ngx_buf_t                 *b, *f;  /* b=当前头缓冲; f=归还free链的单块大缓冲 */
    ngx_event_t               *rev, *wev;  /* 读/写事件 */
    ngx_connection_t          *c;  /* 底层连接 */
    ngx_http_connection_t     *hc;  /* 连接级HTTP状态 */
    ngx_http_log_ctx_t        *ctx;  /* 日志上下文 */
    ngx_http_core_srv_conf_t  *cscf;  /* server配置：大缓冲参数 */
    ngx_http_core_loc_conf_t  *clcf;  /* location配置：keepalive_timeout */

    c = r->connection;  /* 取连接 */
    rev = c->read;  /* 取读事件 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "set http keepalive handler");  /* debug日志 */

    ctx = (ngx_http_log_ctx_t *) c->log->data;  /* 取日志上下文 */
    ctx->action = "closing request";  /* 动作描述切换：本函数内出错时日志可定位 */

    hc = r->http_connection;  /* 取连接级HTTP状态 */
    b = r->header_in;  /* 当前请求头缓冲 */

    if (b->pos < b->last) {  /* 头缓冲中还有未消费字节：下一请求已抢跑(pipeline)或残留 */

        /* the pipelined request */  /* 原注释：这是管线化请求 */

        if (b != c->buffer) {  /* 用的不是连接默认缓冲=动用了大缓冲 */

            /* move the large header buffers to the free list */  /* 原注释：大缓冲归还free链 */

            cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);  /* 取server配置(大缓冲数量) */

            if (hc->free == NULL) {  /* free链指针数组还没建 */
                hc->free = ngx_palloc(c->pool,  /* 按上限数量建指针数组(挂连接池) */
                  cscf->large_client_header_buffers.num * sizeof(ngx_buf_t *));

                if (hc->free == NULL) {  /* 分配失败 */
                    ngx_http_close_connection(c);  /* 关连接 */
                    return;  /* 结束 */
                }  /* free链分配失败分支结束 */
            }  /* free链未建判断结束 */

            for (i = 0; i < hc->nbusy - 1; i++) {  /* 遍历busy链(最后一块存着pipeline数据，不还) */
                f = hc->busy[i];  /* 取出一块用完的大缓冲 */
                hc->free[hc->nfree++] = f;  /* 放回free链 */
                f->pos = f->start;  /* 读游标复位 */
                f->last = f->start;  /* 写游标复位：缓冲变回全空 */
            }  /* 大缓冲归还循环结束 */

            hc->busy[0] = b;  /* 含pipeline数据的缓冲留在busy[0]：下一请求直接以它开头 */
            hc->nbusy = 1;  /* busy链只剩这一块 */
        }  /* 大缓冲归还分支结束 */
    }  /* pipeline数据判断结束 */

    ngx_http_close_request(r, 0);  /* 终结本请求：销毁请求级内存池，请求对象占的内存归还连接池 */
    c->data = hc;  /* 连接的data改回hc：keepalive阶段连接的上下文是hc而非请求 */

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取location配置(注意r->pool刚销毁，这里只用其配置指针) */
    ngx_add_timer(rev, clcf->keepalive_timeout);  /* 挂keepalive定时器：空闲超过它就挥手关连接，防连接空占 */

    if (ngx_handle_level_read_event(rev) == NGX_ERROR) {  /* 以水平触发登记读事件：下一请求字节一到即唤醒 */
        ngx_http_close_connection(c);  /* 登记失败关连接 */
        return;  /* 结束 */
    }  /* 读事件登记失败分支结束 */

    wev = c->write;  /* 取写事件 */
    wev->event_handler = ngx_http_empty_handler;  /* 写回调挂空handler：keepalive期间不应有写兴趣 */

    if (b->pos < b->last) {  /* 存在pipeline数据：立刻处理下一请求 */

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "pipelined request");  /* debug日志 */

        hc->pipeline = 1;  /* 标记管线模式：下一请求是抢跑来的 */
        ctx->action = "reading client pipelined request line";  /* 动作描述切换 */
        ngx_http_init_request(rev);  /* 直接进入请求初始化：复用hc与busy[0]里的字节，无需等新事件 */
        return;  /* 移交完毕 */
    }  /* 立即处理管线请求分支结束 */

    hc->pipeline = 0;  /* 无管线：清标志 */

    if (ngx_pfree(c->pool, r) == NGX_OK) {  /* 尝试把请求结构占的池块提前归还(内存池free链复用) */
        hc->request = NULL;  /* 归还成功：下一请求全新分配 */
    }  /* 请求块归还包括判断结束 */

    b = c->buffer;  /* 转向连接级默认头缓冲 */

    if (ngx_pfree(c->pool, b->start) == NGX_OK) {  /* 尝试把缓冲占的池块也归还 */
        b->pos = NULL;  /* 成功：标记缓冲未分配，keepalive_handler届时重新分配 */

    } else {  /* 缓冲归还不成功的 else：原地清空复用 */
        b->pos = b->start;  /* 归还不成功(池块未被free链吸收)：原地复用 */
        b->last = b->start;  /* 清空缓冲 */
    }  /* 缓冲原地复用分支结束 */

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0, "hc free: " PTR_FMT " %d",  /* debug输出free链状态 */
                   hc->free, hc->nfree);

    if (hc->free) {  /* free链还有大缓冲 */
        for (i = 0; i < hc->nfree; i++) {  /* 逐块尝试归还连接池 */
            ngx_pfree(c->pool, hc->free[i]);  /* 归还(池的free链可吸收则真释放) */
            hc->free[i] = NULL;  /* 清槽位 */
        }  /* free链归还循环结束 */

        hc->nfree = 0;  /* free链清空 */
    }  /* free链清空判断结束 */

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0, "hc busy: " PTR_FMT " %d",  /* debug输出busy链状态 */
                   hc->busy, hc->nbusy);

    if (hc->busy) {  /* busy链还有缓冲(无pipeline时为空，但保险起见) */
        for (i = 0; i < hc->nbusy; i++) {  /* 逐块归还 */
            ngx_pfree(c->pool, hc->busy[i]);  /* 归还连接池 */
            hc->busy[i] = NULL;  /* 清槽位 */
        }  /* busy链归还循环结束 */

        hc->nbusy = 0;  /* busy链清空 */
    }  /* busy链清空判断结束 */

    rev->event_handler = ngx_http_keepalive_handler;  /* 读回调换成keepalive处理：等下一个请求的到来 */

    if (wev->active) {  /* 写事件还挂在事件机制上 */
        if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {  /* kqueue：写事件要显式禁用 */
            if (ngx_del_event(wev, NGX_WRITE_EVENT, NGX_DISABLE_EVENT)  /* kqueue 摘写事件(禁用而非删除)：语句起行 */
                                                                  == NGX_ERROR)
            {
                ngx_http_close_connection(c);  /* 摘除失败关连接 */
                return;  /* 结束 */
            }  /* kqueue摘写事件失败分支结束 */

        } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {  /* 水平触发(epoll LT)：写事件须摘除，否则EPOLLOUT持续误报 */
            if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {  /* 水平触发(epoll LT)摘写事件 */
                ngx_http_close_connection(c);  /* 摘除失败关连接 */
                return;  /* 结束 */
            }  /* 水平触发摘写事件失败分支结束 */
        }  /* 水平触发分支结束 */
    }  /* 写事件挂载判断结束 */

    ctx->action = "keepalive";  /* 动作描述：空闲等待复用中 */

    if (c->tcp_nopush == NGX_TCP_NOPUSH_SET) {  /* 响应期间设置了TCP_NOPUSH(攒大包发送) */
        if (ngx_tcp_push(c->fd) == NGX_ERROR) {  /* 发送TCP_PUSH清掉nopush：把缓冲余量立即推出去 */
            ngx_connection_error(c, ngx_socket_errno, ngx_tcp_push_n " failed");  /* 失败记错误 */
            ngx_http_close_connection(c);  /* 关连接 */
            return;  /* 结束 */
        }  /* TCP_PUSH失败分支结束 */
        c->tcp_nopush = NGX_TCP_NOPUSH_UNSET;  /* 恢复未设置状态：下一请求重新按需设置 */
    }  /* nopush清除判断结束 */

#if 0  /* 禁用代码 */
    /* if "keepalive_buffers off" then we need some other place */  /* 原注释 */
    r->http_state = NGX_HTTP_KEEPALIVE_STATE;  /* (禁用)状态切keepalive——r即将销毁故无处安放 */
#endif  /* 禁用块结束 */

    if (rev->ready) {  /* 读事件已就绪：下一请求字节恰好已经到达 */
        ngx_http_keepalive_handler(rev);  /* 直接处理，省一轮epoll往返 */
    }  /* 立即处理判断结束 */
}  /* ngx_http_set_keepalive 函数结束 */


/*
 * ngx_http_keepalive_handler()：keepalive空闲连接上的读事件。三种结局：
 * 1) timedout：keepalive_timeout到点，连接空闲过久，主动挥手关闭(TCP层正常
 *    四次挥手)；2) recv返回0：客户端先发了FIN(四次挥手)，对等优雅关闭；
 * 3) recv读到字节：下一请求开始了，交回ngx_http_init_request重走解析流程。
 * MSIE惯用RST断连，故ECONNRESET降级忽略，不刷错误日志。
 */
static void ngx_http_keepalive_handler(ngx_event_t *rev)
{
    size_t                  size;  /* 头缓冲容量 */
    ssize_t                 n;  /* recv返回值 */
    ngx_buf_t              *b;  /* 连接级头缓冲 */
    ngx_connection_t       *c;  /* 底层连接 */
    ngx_http_log_ctx_t     *ctx;  /* 日志上下文 */
    ngx_http_connection_t  *hc;  /* 连接级HTTP状态 */

    c = rev->data;  /* 事件回指连接 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "http keepalive handler");  /* debug日志 */

    if (rev->timedout) {  /* keepalive_timeout到点：空闲连接该退场了 */
        ngx_http_close_connection(c);  /* 主动关闭：close()触发四次挥手 */
        return;  /* 结束 */
    }  /* keepalive超时分支结束 */

    ctx = (ngx_http_log_ctx_t *) rev->log->data;  /* 取日志上下文 */

#if (HAVE_KQUEUE)  /* kqueue特有：EOF可与数据一起批量到达 */

    if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {  /* kqueue事件机制下 */
        if (rev->pending_eof) {  /* 本次事件同时报告对端已发FIN */
            ngx_log_error(NGX_LOG_INFO, c->log, rev->kq_errno,  /* INFO级记录客户端关闭 */
                          "kevent() reported that client %s closed "
                          "keepalive connection", ctx->client);
            ngx_http_close_connection(c);  /* 优雅关闭连接 */
            return;  /* 结束 */
        }  /* 对端关闭分支结束 */
    }  /* kqueue分支结束 */

#endif  /* HAVE_KQUEUE条件编译结束 */

    hc = c->data;  /* 取连接级HTTP状态 */
    b = c->buffer;  /* 取连接级头缓冲 */
    size = b->end - b->start;  /* 缓冲总容量 */

    if (b->pos == NULL) {  /* keepalive时曾把缓冲归还池free链：需重新分配 */
        if (!(b->pos = ngx_palloc(c->pool, size))) {  /* 从连接池重新分配数据区 */
            ngx_http_close_connection(c);  /* 失败关连接 */
            return;  /* 结束 */
        }  /* 缓冲重分配失败分支结束 */

        b->start = b->pos;  /* 重设缓冲四指针 */
        b->last = b->pos;  /* 写指针同步到新起始 */
        b->end = b->pos + size;  /* 末指针=新起始+容量 */
    }  /* 缓冲重分配判断结束 */

    /*
     * MSIE closes a keepalive connection with RST flag
     * so we ignore ECONNRESET here.
     */  /* 原注释：MSIE关keepalive用RST，ECONNRESET要忽略 */

    c->log_error = NGX_ERROR_IGNORE_ECONNRESET;  /* 本次recv若遇ECONNRESET则完全静默 */
    ngx_set_socket_errno(0);  /* 清errno：避免把上一次的errno误认为本次错误 */

    n = c->recv(c, b->last, size);  /* recv探测：要么读到下一请求首包字节，要么读到FIN(0) */
    c->log_error = NGX_ERROR_INFO;  /* 恢复常规错误级别 */

    if (n == NGX_AGAIN) {  /* 假唤醒(如kqueue与数据同到又被取走)：内核无数据 */
        return;  /* 继续空闲：定时器仍守着，无需重挂 */
    }  /* 假唤醒分支结束 */

    if (n == NGX_ERROR) {  /* 读错误(排除已降级的ECONNRESET)：连接异常 */
        ngx_http_close_connection(c);  /* 关闭 */
        return;  /* 结束 */
    }  /* 读错误分支结束 */

    rev->log->handler = NULL;  /* 暂摘日志回调：下面这条关闭日志只需客户端地址，不用URL(此时无请求) */

    if (n == 0) {  /* recv返回0：对端发送FIN(四次挥手第一步)，客户端正常关闭连接 */
        ngx_log_error(NGX_LOG_INFO, c->log, ngx_socket_errno,  /* INFO记录：属正常生命周期事件 */
                      "client %s closed keepalive connection", ctx->client);
        ngx_http_close_connection(c);  /* 对等关闭：本端随后close完成挥手 */
        return;  /* 结束 */
    }  /* 客户端正常关闭分支结束 */

    b->last += n;  /* 推进写游标：n字节请求前缀纳入解析 */
    rev->log->handler = ngx_http_log_error;  /* 恢复日志回调：从此有请求上下文 */
    ctx->action = "reading client request line";  /* 动作描述切回读请求行 */

    ngx_http_init_request(rev);  /* 交回请求初始化：为这条连接重建请求对象并开始解析下一个请求 */
}  /* ngx_http_keepalive_handler 函数结束 */


/*
 * ngx_http_set_lingering_close()：lingering(从容)关闭。响应已发完但客户端
 * 可能还有未发完的请求体；若立即close，内核会连未被确认的数据一起以RST拆链，
 * 客户端就读不到响应了。做法：shutdown(WRITE_SHUTDOWN)发本端FIN(进入
 * CLOSE_WAIT半关闭，仍可读)，再挂读事件把客户端余下字节读净(丢弃)，读完或
 * 超时才close——用"先收完再挥手"换取客户端必然收到完整响应。
 */
static void ngx_http_set_lingering_close(ngx_http_request_t *r)
{   
    ngx_event_t               *rev, *wev;  /* 读/写事件 */
    ngx_connection_t          *c;  /* 底层连接 */
    ngx_http_core_loc_conf_t  *clcf;  /* location配置：lingering_time/timeout */

    c = r->connection;  /* 取连接 */

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取配置 */

    rev = c->read;  /* 取读事件 */
    rev->event_handler = ngx_http_lingering_close_handler;  /* 读回调换成lingering处理：读净残留 */

    r->lingering_time = ngx_time() + clcf->lingering_time / 1000;  /* 计算lingering绝对截止时刻：总共容忍这么久 */
    ngx_add_timer(rev, clcf->lingering_timeout);  /* 挂单轮lingering_timeout：每次读空后歇这么久再看 */

    if (ngx_handle_level_read_event(rev) == NGX_ERROR) {  /* 水平触发登记读事件：残留字节一到即读 */
        ngx_http_close_request(r, 0);  /* 失败终结请求 */
        ngx_http_close_connection(c);  /* 关连接 */
        return;  /* 结束 */
    }  /* 读事件登记失败分支结束 */

    wev = c->write;  /* 取写事件 */
    wev->event_handler = ngx_http_empty_handler;  /* 写回调挂空handler：本阶段不再写 */

    if (wev->active) {  /* 写事件还挂着 */
        if (ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) {  /* kqueue：显式禁用写事件 */
            if (ngx_del_event(wev, NGX_WRITE_EVENT, NGX_DISABLE_EVENT)  /* kqueue 摘写事件(禁用而非删除)：语句起行 */
                                                                  == NGX_ERROR)
            {
                ngx_http_close_connection(c);  /* 失败关连接 */
                return;  /* 结束 */
            }  /* kqueue摘写事件失败分支结束 */

        } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {  /* 水平触发：摘写事件防EPOLLOUT误报 */
            if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {  /* 水平触发摘写事件 */
                ngx_http_close_connection(c);  /* 失败关连接 */
                return;  /* 结束 */
            }  /* 水平触发摘写事件失败分支结束 */
        }  /* 水平触发分支结束 */
    }  /* 写事件挂载判断结束 */

    if (ngx_shutdown_socket(c->fd, NGX_WRITE_SHUTDOWN) == -1) {  /* 只关写方向：发出本端FIN，进入半关闭仍可读 */
        ngx_connection_error(c, ngx_socket_errno,  /* shutdown失败记错误 */
                             ngx_shutdown_socket_n " failed");
        ngx_http_close_request(r, 0);  /* 终结请求 */
        ngx_http_close_connection(c);  /* 直接关连接 */
        return;  /* 结束 */
    }  /* shutdown失败分支结束 */

    if (rev->ready) {  /* 残留数据已经在内核缓冲区 */
        ngx_http_lingering_close_handler(rev);  /* 立即读一轮 */
    }  /* 立即读一轮判断结束 */
}  /* ngx_http_set_lingering_close 函数结束 */


/*
 * ngx_http_lingering_close_handler()：lingering阶段的读事件回调。循环读走
 * 客户端残留字节并丢弃；读到EOF(0)或错误说明对端已收尾，直接关；内核读空
 * 则歇一小会儿(lingering_timeout)再看，总时长受lingering_time硬顶约束——
 * 防御恶意客户端无限发送。所有退出路径最终都收敛到close完成挥手。
 */
static void ngx_http_lingering_close_handler(ngx_event_t *rev)
{
    ssize_t                    n;  /* 单轮recv返回值 */
    ngx_msec_t                 timer;  /* 下一次检查的等待毫秒数 */
    ngx_connection_t          *c;  /* 底层连接 */
    ngx_http_request_t        *r;  /* 请求对象 */
    ngx_http_core_loc_conf_t  *clcf;  /* location配置 */
    u_char                     buffer[NGX_HTTP_LINGERING_BUFFER_SIZE];  /* 栈上丢弃缓冲：读进即扔 */

    c = rev->data;  /* 事件回指连接 */
    r = c->data;  /* 连接回指请求 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug日志 */
                   "http lingering close handler");

    if (rev->timedout) {  /* 单轮lingering_timeout到点：客户端没再发数据，收尾 */
        ngx_http_close_request(r, 0);  /* 终结请求 */
        ngx_http_close_connection(c);  /* 关闭连接 */
        return;  /* 结束 */
    }  /* lingering单轮超时分支结束 */

    timer = r->lingering_time - ngx_time();  /* 距lingering总截止时刻还剩几秒 */
    if (timer <= 0) {  /* 总容忍期已到：不再陪客户端耗下去 */
        ngx_http_close_request(r, 0);  /* 终结请求 */
        ngx_http_close_connection(c);  /* 关闭连接 */
        return;  /* 结束 */
    }  /* 总容忍期到点分支结束 */

    do {  /* do-while：读净客户端残留数据(lingering) */
        n = c->recv(c, buffer, NGX_HTTP_LINGERING_BUFFER_SIZE);  /* 读一批残留字节(内容直接作废) */

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0, "lingering read: %d", n);  /* debug记录读取量 */

        if (n == NGX_ERROR || n == 0) {  /* 读错误或对端FIN(0)：客户端侧已收尾 */
            ngx_http_close_request(r, 0);  /* 终结请求 */
            ngx_http_close_connection(c);  /* 关闭连接完成挥手 */
            return;  /* 结束 */
        }  /* 对端已收尾分支结束 */

    } while (rev->ready);  /* 内核还有数据就继续循环读：一次读空减少epoll往返 */

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取配置 */

    timer *= 1000;  /* 剩余总时间换算为毫秒 */

    if (timer > clcf->lingering_timeout) {  /* 剩余总时间还很长 */
        timer = clcf->lingering_timeout;  /* 本轮只歇lingering_timeout：分批检查 */
    }  /* 截断为单轮超时判断结束 */

    ngx_add_timer(rev, timer);  /* 重挂定时器：到点再来看一眼是否还有残留 */

    return;  /* 挂起等待 */
}  /* ngx_http_lingering_close_handler 函数结束 */


/*
 * ngx_http_empty_handler()：空事件处理器。挂在"暂不关心"的事件上(如keepalive
 * 期间的写事件)，吞掉事件触发但不做任何事——比反复摘挂事件代价更低，
 * 也不会像未注册handler那样引发空指针。
 */
void ngx_http_empty_handler(ngx_event_t *wev)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, wev->log, 0, "http empty handler");  /* debug日志：便于确认事件被吞 */

    return;  /* 什么都不做 */
}  /* ngx_http_empty_handler 函数结束 */


/*
 * ngx_http_send_last()：发送响应末尾。HTTP响应必须有个"结束"标记，过滤链以
 * last_buf标志识别响应体终点，从而触发flush与收尾(此后才能finalize/keepalive)。
 */
ngx_int_t ngx_http_send_last(ngx_http_request_t *r)
{
    ngx_buf_t    *b;  /* 空的结束缓冲 */
    ngx_chain_t   out;  /* 单节点链：把结束缓冲挂进过滤链 */

    if (!(b = ngx_calloc_buf(r->pool))) {  /* 从请求池分配一个清零的buf结构 */
        return NGX_ERROR;  /* 失败报错 */
    }  /* buf分配失败分支结束 */

    b->last_buf = 1;  /* 关键标志：这是响应的最后一个buf(过滤链据此收尾) */
    out.buf = b;  /* 链节点指向结束buf */
    out.next = NULL;  /* 无后继：单节点链 */

    return ngx_http_output_filter(r, &out);  /* 送入输出过滤链：各filter处理后经writev发出 */
}  /* ngx_http_send_last 函数结束 */


/*
 * ngx_http_close_request()：请求级终结。逐一释放请求持有的外部资源(打开的
 * 文件fd、缓存锁、请求体临时文件)，写访问日志，最后ngx_destroy_pool把请求级
 * 内存池一次性整体回收——内存池理论的直接应用：请求生命周期内成千上万次
 * 小分配，销毁时只做一次free，无泄漏且O(1)。注意本函数只终结"请求"，TCP
 * 连接的关闭由ngx_http_close_connection负责，二者解耦以支撑keepalive。
 */
void ngx_http_close_request(ngx_http_request_t *r, int error)
{
    ngx_uint_t                 i;  /* 清理链下标 */
    ngx_log_t                 *log;  /* 连接日志对象 */
    ngx_http_log_ctx_t        *ctx;  /* 日志上下文 */
    ngx_http_cleanup_t        *cleanup;  /* 清理链元素：登记的待释放资源 */
    ngx_http_core_loc_conf_t  *clcf;  /* location配置 */
    struct linger              l;  /* SO_LINGER选项结构 */

    log = r->connection->log;  /* 取日志：后面多处要用 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0, "http close request");  /* debug日志 */

    if (r->pool == NULL) {  /* 请求池已销毁=请求已关闭：重复进入是逻辑bug */
        ngx_log_error(NGX_LOG_ALERT, log, 0,  /* ALERT级大声报警 */
                      "http request already closed");
        return;  /* 不能再碰已释放的池，直接返回自保 */
    }  /* 重复关闭防护分支结束 */

#if (NGX_STAT_STUB)  /* 统计桩 */
    if (r->stat_reading) {  /* 请求此前处于reading计数中 */
        (*ngx_stat_reading)--;  /* 对称递减：计数与标志必须成对维护 */
    }  /* reading计数递减判断结束 */

    if (r->stat_writing) {  /* 请求此前处于writing计数中 */
        (*ngx_stat_writing)--;  /* 对称递减 */
    }  /* writing计数递减判断结束 */
#endif  /* 统计桩结束 */

    if (error && r->headers_out.status == 0) {  /* 带错误终结且响应状态还没定 */
        r->headers_out.status = error;  /* 以错误码作为响应状态记入日志 */
    }  /* 状态码兜底判断结束 */

    ngx_http_log_handler(r);  /* 写访问日志：请求生命周期到此定格(记录状态/字节数等) */

    cleanup = r->cleanup.elts;  /* 取清理链数组 */
    for (i = 0; i < r->cleanup.nelts; i++) {  /* 遍历全部已登记的清理项 */
        if (!cleanup[i].valid) {  /* 该项已失效(资源早被释放) */
            continue;  /* 跳过 */
        }  /* 失效项跳过判断结束 */

#if (NGX_HTTP_CACHE)  /* 条件编译：缓存清理项 */

        if (cleanup[i].cache) {  /* 该项是缓存节点锁 */
            ngx_http_cache_unlock(cleanup[i].data.cache.hash,  /* 解锁缓存节点：别的请求可以用了 */
                                  cleanup[i].data.cache.cache, log);
            continue;  /* 下一项 */
        }  /* 缓存节点解锁分支结束 */

#endif  /* NGX_HTTP_CACHE条件编译结束 */

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0, "http cleanup fd: %d",  /* debug记录待关fd */
                       cleanup[i].data.file.fd);

        if (ngx_close_file(cleanup[i].data.file.fd) == NGX_FILE_ERROR) {  /* 关闭该清理项登记的文件fd(如打开的静态文件/sendfile源) */
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,  /* 关闭失败ALERT报警(fd泄漏前兆) */
                          ngx_close_file_n " \"%s\" failed",
                          cleanup[i].data.file.name);
        }  /* 关fd失败分支结束 */
    }  /* 清理链遍历结束 */

    /* STUB */  /* 原注释：临时的兜底处理 */
    if (r->file.fd != NGX_INVALID_FILE) {  /* 请求主文件还开着 */
        if (ngx_close_file(r->file.fd) == NGX_FILE_ERROR) {  /* 关闭它 */
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,  /* 失败报警 */
                          ngx_close_file_n " \"%s\" failed", r->file.name.data);
        }  /* 关主文件失败分支结束 */
    }  /* 主文件关闭判断结束 */

    if (r->request_body  /* 请求体被转存成了临时文件(体太大不宜驻内存) */
        && r->request_body->temp_file
        && r->request_body->temp_file->file.fd != NGX_INVALID_FILE)
    {  /* 临时文件还开着 */
        if (ngx_close_file(r->request_body->temp_file->file.fd)  /* 关闭请求体临时文件 */
                                                             == NGX_FILE_ERROR)
        {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,  /* 失败报警 */
                          ngx_close_file_n " deleted file \"%s\" failed",
                          r->request_body->temp_file->file.name.data);
        }  /* 关临时文件失败分支结束 */
    }  /* 临时文件关闭判断结束 */

    if (r->connection->timedout) {  /* 连接是因超时终结的 */
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取配置 */

        if (clcf->reset_timedout_connection) {  /* 配置要求超时连接用RST而非FIN */
            l.l_onoff = 1;  /* 打开SO_LINGER */
            l.l_linger = 0;  /* linger 0秒：close即发RST，跳过TIME_WAIT */

            if (setsockopt(r->connection->fd, SOL_SOCKET, SO_LINGER,  /* 套用选项：RST可立即回收对端状态，防超时连接堆积 */
                           (const void *) &l, sizeof(struct linger)) == -1)
            {
                ngx_log_error(NGX_LOG_ALERT, log, ngx_socket_errno,  /* 失败报警 */
                              "setsockopt(SO_LINGER) failed");
            }  /* SO_LINGER设置失败分支结束 */
        }  /* RST选项判断结束 */
    }  /* 超时连接处理结束 */

    /* ctx->url was allocated from r->pool */  /* 原注释：url来自请求池 */
    ctx = log->data;  /* 取日志上下文 */
    ctx->url = NULL;  /* 摘掉url指针：它随请求池一起销毁了，留着就是悬垂指针 */

    r->request_line.len = 0;  /* 同理清掉指向请求池的请求行 */

    ngx_destroy_pool(r->pool);  /* 核心一步：销毁请求级内存池，本请求全部动态内存一次性归还 */

    return;  /* 请求彻底终结(TCP连接的去留另由finalize决定) */
}  /* ngx_http_close_request 函数结束 */


#if (NGX_HTTP_SSL)  /* 条件编译：SSL关闭处理 */

/*
 * ngx_ssl_close_handler()：SSL连接的关闭事件回调。TLS拆除需发close_notify
 * 告警，非阻塞下未必一次发完；AGAIN时挂起等下次事件，完成后才真正关TCP。
 */
void ngx_ssl_close_handler(ngx_event_t *ev)
{
    ngx_connection_t  *c;  /* 底层连接 */

    c = ev->data;  /* 事件回指连接 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0, "http ssl close handler");  /* debug日志 */

    if (ngx_ssl_shutdown(c) == NGX_AGAIN) {  /* SSL关闭未完成(告警还在发送队列里) */
        return;  /* 挂起：SSL层登记了读写事件，完成后再次进入 */
    }  /* SSL关闭未完成分支结束 */

    ngx_http_close_connection(c);  /* SSL层拆除完毕：关闭TCP连接 */
}  /* ngx_ssl_close_handler 函数结束 */

#endif  /* NGX_HTTP_SSL条件编译结束 */


/*
 * ngx_http_close_connection()：连接级终结入口。减活动连接计数后交由事件层
 * 的ngx_close_connection完成实际工作：摘epoll、摘定时器红黑树节点、close(fd)
 * 触发TCP四次挥手、归还连接池。请求/连接两级收尾由此彻底分离。
 */
void ngx_http_close_connection(ngx_connection_t *c)
{
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* debug记录被关fd */
                   "close http connection: %d", c->fd);

#if (NGX_STAT_STUB)  /* 统计桩 */
    (*ngx_stat_active)--;  /* 活动连接计数-1 */
#endif  /* 统计桩结束 */

    ngx_close_connection(c);  /* 事件层收尾：摘事件/摘定时器/close(fd)/归还连接对象 */
}  /* ngx_http_close_connection 函数结束 */


/*
 * ngx_http_client_error()：客户端侧解析类错误的统一处理。两类入参：超时类
 * 直接关连接收场；语法/语义类则按错误码查client_header_errors文案表记日志，
 * 并把NGX_HTTP_PARSE_*错误码翻译成响应状态码(400/414/501等)，再经finalize
 * 走ngx_http_special_response给客户端回错误页。客户端错误与服务器故障严格
 * 区分日志级别：前者ERR且不惊动运维，后者才是服务器问题。
 */
static void ngx_http_client_error(ngx_http_request_t *r,
                                  int client_error, int error)  /* client_error=解析错误码; error=响应状态码 */
{
    ngx_http_log_ctx_t        *ctx;  /* 日志上下文 */
    ngx_http_core_srv_conf_t  *cscf;  /* server配置：restrict_host_names */

    ctx = r->connection->log->data;  /* 取日志上下文 */

    if (error == NGX_HTTP_REQUEST_TIME_OUT) {  /* 超时类错误(408)：请求头迟迟收不齐 */
        ngx_log_error(NGX_LOG_INFO, r->connection->log, NGX_ETIMEDOUT,  /* INFO级：慢客户端不算事故 */
                      "client timed out");
        r->connection->timedout = 1;  /* 标记连接超时：close_request会据此决定SO_LINGER RST */
        ngx_http_close_request(r, error);  /* 终结请求 */
        ngx_http_close_connection(r->connection);  /* 关闭连接 */
        return;  /* 结束 */
    }  /* 408超时分支结束 */

    r->connection->log->handler = NULL;  /* 暂摘日志回调：下面要手工定制日志格式(含错误文案) */

    if (ctx->url) {  /* URL已知(请求行已解析出)：日志可带完整URL */
        switch (client_error) {  /* 按解析错误码分派日志文案 */

        case NGX_HTTP_PARSE_INVALID_HOST:  /* Host头与所有server_name不匹配 */
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,  /* ERR记录：文案取自错误表(带host/url两个参数) */
                    client_header_errors[client_error - NGX_HTTP_CLIENT_ERROR],
                    ctx->client, r->headers_in.host->value.data, ctx->url);

            error = NGX_HTTP_INVALID_HOST;  /* 状态码改为444/INVALID_HOST语义 */

            cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);  /* 取server配置 */

            if (cscf->restrict_host_names == NGX_HTTP_RESTRICT_HOST_CLOSE) {  /* 配置为对未知Host直接断链 */
                ngx_http_close_request(r, error);  /* 终结请求：连错误页都不给 */
                ngx_http_close_connection(r->connection);  /* 直接关闭连接 */
                return;  /* 结束 */
            }  /* 未知Host断链分支结束 */

            break;  /* 否则继续走通用路径回错误页 */

        case NGX_HTTP_PARSE_HTTP_TO_HTTPS:  /* 明文HTTP发到SSL端口 */
            error = NGX_HTTP_TO_HTTPS;  /* 状态码改为TO_HTTPS(495类) */

            /* fall through */  /* 原注释：故意贯穿到default共用日志逻辑 */

        default:  /* 其余解析错误：按错误码查文案表记日志 */
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,  /* 记 ERR 日志：错误表文案+客户端+URL */
                    client_header_errors[client_error - NGX_HTTP_CLIENT_ERROR],
                    ctx->client, ctx->url);
        }  /* 解析错误码分派结束 */

    } else {  /* URL未知(请求行都没解析出来)：日志只能记到客户端地址 */
        if (error == NGX_HTTP_REQUEST_URI_TOO_LARGE) {  /* URI超长(414) */
            r->request_line.len = r->header_in->end - r->request_start;  /* 尽量还原超长请求行供日志 */
            r->request_line.data = r->request_start;  /* 指向超长请求行起始 */
        }  /* URI超长还原判断结束 */

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,  /* ERR记录(文案只有一个参数) */
                    client_header_errors[client_error - NGX_HTTP_CLIENT_ERROR],
                    ctx->client);
    }  /* URL未知分支结束 */

    r->connection->log->handler = ngx_http_log_error;  /* 恢复标准日志回调 */

    ngx_http_finalize_request(r, error);  /* 以错误状态码终结：finalize将调special_response回错误页 */
}  /* ngx_http_client_error 函数结束 */


/*
 * ngx_http_log_error()：连接日志的上下文拼装回调。nginx错误日志="基础消息+
 * 本回调输出"，基础消息由各出错点提供，本回调补充"当时正在做什么(action)、
 * 客户端是谁、访问什么URL"——正是排障时最需要的三元组。
 */
static size_t ngx_http_log_error(void *data, char *buf, size_t len)  /* 返回实际拼装长度 */
{
    ngx_http_log_ctx_t *ctx = data;  /* 取日志上下文 */

    if (ctx->action && ctx->url) {  /* 动作与URL都有：完整三元组 */
        return ngx_snprintf(buf, len, " while %s, client: %s, URL: %s",  /* 拼完整日志前缀：动作+客户端+URL */
                            ctx->action, ctx->client, ctx->url);

    } else if (ctx->action == NULL && ctx->url) {  /* 无动作描述(keepalive空闲期)但有URL残留 */
        return ngx_snprintf(buf, len, ", client: %s, URL: %s",  /* 拼：客户端+URL */
                            ctx->client, ctx->url);

    } else {  /* 无URL(请求行尚未解析)：只报动作与客户端 */
        return ngx_snprintf(buf, len, " while %s, client: %s",  /* 拼：动作+客户端 */
                            ctx->action, ctx->client);
    }  /* 无URL分支结束 */
}  /* ngx_http_log_error 函数结束 */
