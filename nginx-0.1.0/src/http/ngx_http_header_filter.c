
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置总头：探测宏与平台类型映射 */
#include <ngx_core.h>    /* 核心头：ngx_str_t/内存池/日志等基础设施 */
#include <ngx_http.h>    /* HTTP框架：请求结构、headers_out、过滤器链指针 */
#include <nginx.h>       /* 版本头：NGINX_VER宏(用于Server响应头) */


static ngx_int_t ngx_http_header_filter_init(ngx_cycle_t *cycle);  /* 前置声明：模块初始化钩子 */
static ngx_int_t ngx_http_header_filter(ngx_http_request_t *r);  /* 前置声明：响应头过滤器本体 */


static ngx_http_module_t  ngx_http_header_filter_module_ctx = {  /* HTTP模块上下文：头过滤器不解析指令，钩子全为NULL */
    NULL,                                  /* pre conf */  /* 解析配置前钩子：不需要 */

    NULL,                                  /* create main configuration */  /* 不需要main级配置结构 */
    NULL,                                  /* init main configuration */  /* 不需要main级初始化 */

    NULL,                                  /* create server configuration */  /* 不需要server级配置结构 */
    NULL,                                  /* merge server configuration */  /* 不需要server级合并 */

    NULL,                                  /* create location configuration */  /* 不需要location级配置结构 */
    NULL,                                  /* merge location configuration */  /* 不需要location级合并 */
};  /* 模块上下文定义结束 */


ngx_module_t  ngx_http_header_filter_module = {  /* 模块描述结构 */
    NGX_MODULE,  /* 模块版本/签名宏 */
    &ngx_http_header_filter_module_ctx,    /* module context */  /* 指向上面的HTTP模块上下文 */
    NULL,                                  /* module directives */  /* 无指令表 */
    NGX_HTTP_MODULE,                       /* module type */  /* 模块类型：HTTP */
    ngx_http_header_filter_init,           /* init module */  /* 模块初始化钩子：注册头过滤链顶端 */
    NULL                                   /* init child */  /* 无进程级初始化 */
};  /* 模块描述结束 */


static char server_string[] = "Server: " NGINX_VER CRLF;  /* 预生成的Server头(含版本与CRLF)：编译期拼好，运行时整串memcpy零拼装开销 */


static ngx_str_t http_codes[] = {  /* 状态码文本表：状态行按下标O(1)取用，下标=状态码换算(见过滤函数) */

    ngx_string("200 OK"),  /* [0] 2XX段基址(status-200)：最常见响应置于首位 */
    ngx_null_string,  /* "201 Created" */  /* [1] 0.1.0不使用201 */
    ngx_null_string,  /* "202 Accepted" */  /* [2] 0.1.0不使用202 */
    ngx_null_string,  /* "203 Non-Authoritative Information" */  /* [3] 0.1.0不使用203 */
    ngx_null_string,  /* "204 No Content" */  /* [4] 0.1.0不使用204 */
    ngx_null_string,  /* "205 Reset Content" */  /* [5] 0.1.0不使用205 */
    ngx_string("206 Partial Content"),  /* [6] 206部分内容：Range断点续传/分块下载 */
    ngx_null_string,  /* "207 Multi-Status" */  /* [7] 0.1.0不使用207 */

#if 0  /* 条件编译关闭：300暂不支持，此表项留空以保持下标连续 */
    ngx_null_string,  /* "300 Multiple Choices" */  /* 300多选：0.1.0不使用 */
#endif  /* 条件编译结束 */

    ngx_string("301 Moved Permanently"),  /* [8] 3XX段基址(status-301+8)：永久重定向(可被客户端缓存) */
#if 0  /* 条件编译：选用302的另一种文本 */
    ngx_string("302 Moved Temporarily"),  /* [9] 302旧称：临时移动 */
#else  /* 采用RFC2616规范名 */
    ngx_string("302 Found"),  /* [9] 302规范名：临时重定向(不让客户端永久缓存) */
#endif  /* 302文本选择结束 */
    ngx_null_string,  /* "303 See Other" */  /* [10] 0.1.0不使用303 */
    ngx_string("304 Not Modified"),  /* [11] 协商缓存命中：无响应体，仅靠头部校验If-Modified-Since */

    ngx_string("400 Bad Request"),  /* [12] 4XX段基址(status-400+12)：请求语法错误 */
    ngx_string("401 Unauthorized"),  /* [13] 未认证：需携带WWW-Authenticate挑战 */
    ngx_null_string,  /* "402 Payment Required" */  /* [14] 预留码：0.1.0不使用 */
    ngx_string("403 Forbidden"),  /* [15] 禁止访问：服务器拒绝且无认证可言 */
    ngx_string("404 Not Found"),  /* [16] 资源不存在：最经典的错误码 */
    ngx_string("405 Not Allowed"),  /* [17] 方法不被允许(如对静态文件发POST) */
    ngx_null_string,  /* "406 Not Acceptable" */  /* [18] 0.1.0不使用406 */
    ngx_null_string,  /* "407 Proxy Authentication Required" */  /* [19] 代理认证：0.1.0不使用 */
    ngx_string("408 Request Time-out"),  /* [20] 请求超时：客户端发请求过慢，服务器关闭连接 */
    ngx_null_string,  /* "409 Conflict" */  /* [21] 0.1.0不使用409 */
    ngx_null_string,  /* "410 Gone" */  /* [22] 0.1.0不使用410 */
    ngx_string("411 Length Required"),  /* [23] 缺少Content-Length：拒绝无长度界定的请求体 */
    ngx_null_string,  /* "412 Precondition Failed" */  /* [24] 0.1.0不使用412 */
    ngx_string("413 Request Entity Too Large"),  /* [25] 请求体超限(如超client_max_body_size) */
    /* [26] 414恒不发送：0.1.0把超长URI请求按HTTP/0.9处理(只发body不发头) */
    ngx_null_string,  /* "414 Request-URI Too Large" but we never send it
                       * because we treat such requests as the HTTP/0.9
                       * requests and send only a body without a header
                       */
    ngx_null_string,  /* "415 Unsupported Media Type" */  /* [27] 0.1.0不使用415 */
    ngx_string("416 Requested Range Not Satisfiable"),  /* [28] Range越界：续传起点超出文件长度 */

    ngx_string("500 Internal Server Error"),  /* [29] 5XX段基址(status-500+29)：服务器内部错误 */
    ngx_string("501 Method Not Implemented"),  /* [30] 方法未实现 */
    ngx_string("502 Bad Gateway"),  /* [31] 网关错误：上游返回非法/中断的响应 */
    ngx_string("503 Service Temporarily Unavailable"),  /* [32] 服务暂不可用：过载或维护(可配Retry-After) */
    ngx_string("504 Gateway Time-out")  /* [33] 网关超时：上游在时限内未应答 */
};  /* 状态码表结束 */


ngx_http_header_t  ngx_http_headers_out[] = {  /* 内置头名称表：其他模块可按头名经offsetof统一读写headers_out字段 */
    { ngx_string("Server"), offsetof(ngx_http_headers_out_t, server) },  /* offsetof求出值域在headers_out内的字节偏移 */
    { ngx_string("Date"), offsetof(ngx_http_headers_out_t, date) },  /* Date头：响应生成时间(RFC1123 GMT格式) */
    { ngx_string("Content-Type"),  /* Content-Type头：响应体MIME类型 */
                             offsetof(ngx_http_headers_out_t, content_type) },  /* Content-Type头：MIME类型(由types配置映射) */
    { ngx_string("Content-Length"),  /* Content-Length头：定长响应体长 */
                           offsetof(ngx_http_headers_out_t, content_length) },  /* Content-Length头：定长响应的体长 */
    { ngx_string("Content-Encoding"),  /* Content-Encoding头：压缩编码 */
                         offsetof(ngx_http_headers_out_t, content_encoding) },  /* Content-Encoding头：内容编码(gzip等) */
    { ngx_string("Location"), offsetof(ngx_http_headers_out_t, location) },  /* Location头：重定向目标URL */
    { ngx_string("Last-Modified"),  /* Last-Modified头：缓存校验用修改时间 */
                            offsetof(ngx_http_headers_out_t, last_modified) },  /* Last-Modified头：协商缓存(Cache校验)依据 */
    { ngx_string("Accept-Ranges"),  /* Accept-Ranges头：声明支持Range续传 */
                            offsetof(ngx_http_headers_out_t, accept_ranges) },  /* Accept-Ranges头：声明支持Range断点续传 */
    { ngx_string("Expires"), offsetof(ngx_http_headers_out_t, expires) },  /* Expires头：绝对过期时间(HTTP/1.0缓存) */
    { ngx_string("Cache-Control"),  /* Cache-Control头：HTTP/1.1缓存策略 */
                            offsetof(ngx_http_headers_out_t, cache_control) },  /* Cache-Control头：HTTP/1.1缓存策略 */
    { ngx_string("ETag"), offsetof(ngx_http_headers_out_t, etag) },  /* ETag头：实体标签(强缓存校验器) */

    { ngx_null_string, 0 }  /* 哨兵项：数组遍历结束标志 */
};  /* 内置头表结束 */


/*
 * ngx_http_header_filter()：响应头过滤器——头过滤链的顶端(每请求只执行一次)。
 * 把status/headers_out拼装成HTTP/1.x协议文本：状态行、Server、Date(GMT)、
 * Content-Type、Content-Length、Connection(keep-alive/close连接协商)、
 * Last-Modified等；头部作为一个buf经写过滤器发出，与首段正文可聚合进
 * 同一TCP段(对应TCP_CORK攒包思路)，减少小包与往返次数。
 */
static ngx_int_t ngx_http_header_filter(ngx_http_request_t *r)  /* r=当前请求 */
{  /* 函数体开始 */
    u_char                    *p;  /* 临时指针：记录写入起点以便回填值域长度 */
    size_t                     len;  /* 响应头总字节数(两次遍历：先统计后写入) */
    ngx_uint_t                 status, i;  /* status=http_codes下标; i=链表段内遍历下标 */
    ngx_buf_t                 *b;  /* 承载响应头文本的临时buf */
    ngx_chain_t               *ln;  /* 包裹头buf的链节点 */
    ngx_list_part_t           *part;  /* 通用头链表的当前段 */
    ngx_table_elt_t           *header;  /* 段内头条目数组 */
    ngx_http_core_loc_conf_t  *clcf;  /* core模块location配置(keepalive设置) */

    if (r->http_version < NGX_HTTP_VERSION_10) {  /* HTTP/0.9协议：无响应头概念 */
        return NGX_OK;  /* 直接放行到body过滤，只发正文(协议如此规定) */
    }  /* 协议版本判断结束 */

    if (r->method == NGX_HTTP_HEAD) {  /* HEAD请求：只要元数据不要正文 */
        r->header_only = 1;  /* 置只发头标志：头部buf将被标记为last_buf */
    }  /* HEAD方法判断结束 */

    if (r->headers_out.last_modified_time != -1) {  /* 已设置Last-Modified时间戳(Unix秒, -1表示未设) */
        if (r->headers_out.status != NGX_HTTP_OK  /* 仅200/304/206允许携带Last-Modified */
            && r->headers_out.status != NGX_HTTP_NOT_MODIFIED  /* 304凭它做缓存校验 */
            && r->headers_out.status != NGX_HTTP_PARTIAL_CONTENT)  /* 206凭它定位Range区间 */
        {  /* 其余状态码 */
            r->headers_out.last_modified_time = -1;  /* 撤销时间戳：语义上无效 */
            r->headers_out.last_modified = NULL;  /* 同时清除已构造的头条目 */
        }  /* 状态码合法性判断结束 */
    }  /* Last-Modified存在判断结束 */

    /* 2 is for trailing "\r\n" and 2 is for "\r\n" in the end of header */
    len = sizeof("HTTP/1.x ") - 1 + 2 + 2;  /* 总长基线：状态行前缀9字节+2+2(结尾CRLF与头终止空行CRLF)；先粗估后精确累加，避免二次扫描 */

    /* status line */
    if (r->headers_out.status_line.len) {  /* 模块/上游已显式给出完整状态行文本 */
        len += r->headers_out.status_line.len;  /* 按给定文本长度累加 */
#if (NGX_SUPPRESS_WARN)  /* 某些编译器会报status未初始化，此宏用于压警告 */
        status = NGX_INVALID_ARRAY_INDEX;  /* 置无效下标(不会被使用) */
#endif  /* 压警告条件编译结束 */

    } else {  /* 只有数值状态码：换算成http_codes表下标 */

        if (r->headers_out.status < NGX_HTTP_MOVED_PERMANENTLY) {  /* 小于301即2XX段(200..207) */
            /* 2XX */
            status = r->headers_out.status - NGX_HTTP_OK;  /* 下标=status-200：落进数组[0..7] */

        } else if (r->headers_out.status < NGX_HTTP_BAD_REQUEST) {  /* 3XX段(301..304等) */
            /* 3XX */
            status = r->headers_out.status - NGX_HTTP_MOVED_PERMANENTLY + 8;  /* 下标=(status-301)+8：3XX段从[8]起 */

            if (r->headers_out.status == NGX_HTTP_NOT_MODIFIED) {  /* 304缓存命中无实体 */
                r->header_only = 1;  /* 同HEAD：只发响应头，正文长度为0 */
            }  /* 304判断结束 */

        } else if (r->headers_out.status < NGX_HTTP_INTERNAL_SERVER_ERROR) {  /* 4XX段(400..416) */
            /* 4XX */
            status = r->headers_out.status - NGX_HTTP_BAD_REQUEST + 8 + 4;  /* 下标=(status-400)+12：4XX段从[12]起 */

        } else {  /* 其余为5XX段 */
            /* 5XX */
            status = r->headers_out.status  /* 下标=(status-500)+29：5XX段从[29]起 */
                                 - NGX_HTTP_INTERNAL_SERVER_ERROR + 8 + 4 + 17;  /* 续上行：偏移累计29，即5XX段基址 */
        }  /* 状态码分档结束 */

        len += http_codes[status].len;  /* 累加状态文本(如"404 Not Found")长度 */
    }  /* 状态行长度分支结束 */

    if (r->headers_out.server && r->headers_out.server->key.len) {  /* 上游/模块已设置自定义Server头 */
        len += r->headers_out.server->key.len  /* 键+值+2字节(": "与"\r\n"分隔) */
               + r->headers_out.server->value.len + 2;  /* 续上行：值长+2字节分隔 */
    } else {  /* 未自定义 */
        len += sizeof(server_string) - 1;  /* 用预置的"Server: nginx/版本"串长度 */
    }  /* Server头长度分支结束 */

    if (r->headers_out.date && r->headers_out.date->key.len) {  /* 已显式设置Date头 */
        len += r->headers_out.date->key.len  /* 键+值+2字节分隔 */
               + r->headers_out.date->value.len + 2;  /* 续上行：值长+CRLF两字节 */
    } else {  /* 未设置则由本过滤器补 */
        len += sizeof("Date: Mon, 28 Sep 1970 06:00:00 GMT" CRLF) - 1;  /* RFC1123固定29字符GMT格式，长度编译期可知 */
    }  /* Date头长度分支结束 */

    if (r->headers_out.content_length == NULL) {  /* 尚未设置Content-Length头条目 */
        if (r->headers_out.content_length_n >= 0) {  /* 体长已知(>=0)：定长响应 */
            len += sizeof("Content-Length: ") - 1 + NGX_OFF_T_LEN + 2;  /* 按off_t最大十进制位数预留，写入时按实际数字 */
        }  /* 定长判断结束 */
    }  /* Content-Length判断结束 */

    if (r->headers_out.content_type && r->headers_out.content_type->value.len) {  /* 已设置Content-Type */
        r->headers_out.content_type->key.len = 0;  /* 清空键长作标记：此头由本过滤器统一输出，通用头扫描阶段跳过 */
        len += sizeof("Content-Type: ") - 1  /* 头名+值+2字节分隔 */
               + r->headers_out.content_type->value.len + 2;  /* 续上行：MIME值长+CRLF */

        if (r->headers_out.charset.len) {  /* 配置了charset(如charset utf-8) */
            len += sizeof("; charset=") - 1 + r->headers_out.charset.len;  /* 追加"; charset=xxx"长度 */
        }  /* charset判断结束 */
    }  /* Content-Type长度分支结束 */

    if (r->headers_out.location  /* 相对Location(以/开头)需补全为绝对URL */
        && r->headers_out.location->value.len  /* 且值非空 */
        && r->headers_out.location->value.data[0] == '/')  /* 且以'/'开头(相对URL) */
    {  /* 条件成立 */
        r->headers_out.location->key.len = 0;  /* 清键作标记：由本过滤器统一输出 */
        len += sizeof("Location: http://") - 1  /* "http://"+域名+原路径+分隔 */
               + r->server_name->len + r->headers_out.location->value.len + 2;  /* 续上行：域名+路径长+CRLF */

        if (r->port != 80) {  /* 非HTTP默认端口需带":port" */
            len += r->port_text->len;  /* 端口文本长度(预格式化好的":8080"串) */
        }  /* 端口判断结束 */
    }  /* Location补全分支结束 */

    if (r->headers_out.last_modified && r->headers_out.last_modified->key.len) {  /* 已有现成的Last-Modified头文本 */
        len += r->headers_out.last_modified->key.len  /* 键+值+2字节分隔 */
               + r->headers_out.last_modified->value.len + 2;  /* 续上行：值长+CRLF */

    } else if (r->headers_out.last_modified_time != -1) {  /* 只有Unix时间戳：由本过滤器格式化 */
        len += sizeof("Last-Modified: Mon, 28 Sep 1970 06:00:00 GMT" CRLF) - 1;  /* 按固定GMT日期格式长度预留 */
    }  /* Last-Modified长度分支结束 */

    if (r->chunked) {  /* 分块传输编码(体长未知且为HTTP/1.1) */
        len += sizeof("Transfer-Encoding: chunked" CRLF) - 1;  /* chunked以长度自描述，替代Content-Length */
    }  /* chunked判断结束 */

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取location配置(keepalive_timeout/keepalive_header) */

    if (r->keepalive) {  /* 协商结果为保持连接：TCP连接复用，响应后不挥手 */
        len += sizeof("Connection: keep-alive" CRLF) - 1;  /* 明示keep-alive避免依赖HTTP/1.1隐式语义 */

        /*
         * MSIE and Opera ignore the "Keep-Alive: timeout=<N>" header.
         * MSIE keeps the connection alive for about 60-65 seconds.
         * Opera keeps the connection alive very long.
         * Mozilla keeps the connection alive for N plus about 1-10 seconds.
         * Konqueror keeps the connection alive for about N seconds.
         */

        if (clcf->keepalive_header  /* 配置允许发送Keep-Alive头 */
            && (r->headers_in.gecko || r->headers_in.konqueror))  /* 且仅Firefox/Konqueror会读它(见上方浏览器差异注释) */
        {  /* 条件成立 */
            len += sizeof("Keep-Alive: timeout=") - 1 + TIME_T_LEN + 2;  /* "Keep-Alive: timeout=N\r\n"长度 */
        }  /* Keep-Alive头判断结束 */

    } else {  /* 不保持连接 */
        len += sizeof("Connection: closed" CRLF) - 1;  /* 明示close：响应发完即四次挥手释放fd */
    }  /* Connection头长度分支结束 */

    part = &r->headers_out.headers.part;  /* 通用头链表第一段 */
    header = part->elts;  /* 段内元素数组首地址 */

    for (i = 0; /* void */; i++) {  /* 遍历分段数组(每段定容，满则换下一段) */

        if (i >= part->nelts) {  /* 本段元素已用尽 */
            if (part->next == NULL) {  /* 且没有下一段 */
                break;  /* 遍历结束 */
            }  /* 尾段判断结束 */

            part = part->next;  /* 切换到下一段 */
            header = part->elts;  /* 取新段元素数组 */
            i = 0;  /* 下标归零重新计数 */
        }  /* 换段判断结束 */

        if (header[i].key.len == 0) {  /* 键被清空的头(content_type/location)已由本过滤器单独输出 */
            continue;  /* 跳过避免重复 */
        }  /* 空键判断结束 */

        /* 2 is for ": " and 2 is for "\r\n" */
        len += header[i].key.len + 2 + header[i].value.len + 2;  /* 累加"键: 值\r\n"长度 */
    }  /* 通用头统计循环结束 */

    if (!(b = ngx_create_temp_buf(r->pool, len))) {  /* 按统计总长一次性分配头缓冲(内存池随请求统一释放) */
        return NGX_ERROR;  /* 分配失败 */
    }  /* 分配判断结束 */

    /* "HTTP/1.x " */
    b->last = ngx_cpymem(b->last, "HTTP/1.1 ", sizeof("HTTP/1.x ") - 1);  /* 写协议版本：ngx_cpymem=memcpy并返回新末址，链式推进写指针 */

    /* status line */
    if (r->headers_out.status_line.len) {  /* 有显式状态行文本 */
        b->last = ngx_cpymem(b->last, r->headers_out.status_line.data,  /* 原样拷入 */
                             r->headers_out.status_line.len);  /* 续上行：显式状态行文本长度 */

    } else {  /* 查表取状态文本 */
        b->last = ngx_cpymem(b->last, http_codes[status].data,  /* 按换算下标O(1)取"200 OK"之类文本 */
                             http_codes[status].len);  /* 续上行：状态文本长度 */
    }  /* 状态行输出结束 */
    *(b->last++) = CR; *(b->last++) = LF;  /* 状态行以CRLF结尾：HTTP规定行结束符是\r\n而非\n */

    if (!(r->headers_out.server && r->headers_out.server->key.len)) {  /* 未自定义Server头 */
        b->last = ngx_cpymem(b->last, server_string, sizeof(server_string) - 1);  /* 整串拷入预置Server头(已含CRLF) */
    }  /* Server头输出判断结束 */

    if (!(r->headers_out.date && r->headers_out.date->key.len)) {  /* 未自定义Date头 */
        b->last = ngx_cpymem(b->last, "Date: ", sizeof("Date: ") - 1);  /* 先写头名 */
        b->last = ngx_cpymem(b->last, ngx_cached_http_time.data,  /* 取nginx自缓存的GMT时间串：时钟线程每秒刷新， */
                             ngx_cached_http_time.len);  /* 避免每请求调用gmtime/strftime的系统调用与格式化开销 */

        *(b->last++) = CR; *(b->last++) = LF;  /* 行尾CRLF */
    }  /* Date头输出判断结束 */

    if (r->headers_out.content_length == NULL) {  /* 尚无Content-Length头条目 */
        if (r->headers_out.content_length_n >= 0) {  /* 定长响应 */
            b->last += ngx_snprintf((char *) b->last,  /* 数字格式化为十进制文本(不拷整串，直接推进写指针) */
                                sizeof("Content-Length: ") + NGX_OFF_T_LEN + 2,  /* 续上行：缓冲区上限 */
                                "Content-Length: " OFF_T_FMT CRLF,  /* 续上行：格式串 */
                                r->headers_out.content_length_n);  /* 续上行：实际体长 */
        }  /* 定长输出判断结束 */
    }  /* Content-Length输出判断结束 */

    if (r->headers_out.content_type && r->headers_out.content_type->value.len) {  /* 需要输出Content-Type */
        b->last = ngx_cpymem(b->last, "Content-Type: ",  /* 写"Content-Type: "头名 */
                             sizeof("Content-Type: ") - 1);  /* 续上行：头名长度 */
        p = b->last;  /* 记录值域起点：回填charset后总长用 */
        b->last = ngx_cpymem(b->last, r->headers_out.content_type->value.data,  /* 拷入MIME类型 */
                             r->headers_out.content_type->value.len);  /* 续上行：拷入MIME类型 */

        if (r->headers_out.charset.len) {  /* 有charset配置 */
            b->last = ngx_cpymem(b->last, "; charset=",  /* 追加"; charset="分隔 */
                                 sizeof("; charset=") - 1);  /* 续上行：分隔串长度 */
            b->last = ngx_cpymem(b->last, r->headers_out.charset.data,  /* 追加字符集名 */
                                 r->headers_out.charset.len);  /* 续上行：字符集名 */

            r->headers_out.content_type->value.len = b->last - p;  /* 回填值域实际总长(含charset) */
            r->headers_out.content_type->value.data = p;  /* 值域指向头缓冲内：日志/再统计可直接引用 */
        }  /* charset输出判断结束 */

        *(b->last++) = CR; *(b->last++) = LF;  /* 行尾CRLF */
    }  /* Content-Type输出判断结束 */

    if (r->headers_out.location  /* 相对Location需补全 */
        && r->headers_out.location->value.len  /* 且值非空 */
        && r->headers_out.location->value.data[0] == '/')  /* 且以'/'开头 */
    {  /* 条件成立 */
        p = b->last + sizeof("Location: ") - 1;  /* 记录值域起点(跳过头名) */
        b->last = ngx_cpymem(b->last, "Location: http://",  /* 补协议前缀 */
                             sizeof("Location: http://") - 1);  /* 续上行：协议前缀长度 */
        b->last = ngx_cpymem(b->last, r->server_name->data,  /* 补服务器域名 */
                             r->server_name->len);  /* 续上行：拷入服务器域名 */
        if (r->port != 80) {  /* 非默认端口 */
            b->last = ngx_cpymem(b->last, r->port_text->data,  /* 补":port"文本 */
                                 r->port_text->len);  /* 续上行：端口文本 */
        }  /* 端口输出判断结束 */

        b->last = ngx_cpymem(b->last, r->headers_out.location->value.data,  /* 接上原相对路径 */
                             r->headers_out.location->value.len);  /* 续上行：拷入原路径 */

        r->headers_out.location->value.len = b->last - p;  /* 回填补全后的URL长度 */
        r->headers_out.location->value.data = p;  /* 值域指向头缓冲内(绝对URL) */

        *(b->last++) = CR; *(b->last++) = LF;  /* 行尾CRLF */
    }  /* Location输出判断结束 */

    if (!(r->headers_out.last_modified && r->headers_out.last_modified->key.len)  /* 未自定义Last-Modified且 */
        && r->headers_out.last_modified_time != -1)  /* 且时间戳有效 */
    {  /* 有时间戳可用 */
        b->last = ngx_cpymem(b->last, "Last-Modified: ",  /* 写"Last-Modified: "头名 */
                             sizeof("Last-Modified: ") - 1);  /* 续上行：头名长度 */
        b->last += ngx_http_time(b->last, r->headers_out.last_modified_time);  /* Unix秒转"Mon, 28 Sep 1970 06:00:00 GMT"格式 */

        *(b->last++) = CR; *(b->last++) = LF;  /* 行尾CRLF */
    }  /* Last-Modified输出判断结束 */

    if (r->chunked) {  /* 分块传输 */
        b->last = ngx_cpymem(b->last, "Transfer-Encoding: chunked" CRLF,  /* 写chunked传输编码头(已含CRLF) */
                             sizeof("Transfer-Encoding: chunked" CRLF) - 1);  /* 续上行：整行长度 */
    }  /* chunked输出判断结束 */

    if (r->keepalive) {  /* 保持连接 */
        b->last = ngx_cpymem(b->last, "Connection: keep-alive" CRLF,  /* 写keep-alive连接头(已含CRLF) */
                             sizeof("Connection: keep-alive" CRLF) - 1);  /* 续上行：整行长度 */

        if (clcf->keepalive_header  /* 配置允许且浏览器支持(见前) */
            && (r->headers_in.gecko || r->headers_in.konqueror))  /* 且该浏览器会解析此头 */
        {  /* 条件成立 */
            b->last += ngx_snprintf((char *) b->last,  /* 写Keep-Alive: timeout=N：告知浏览器保活秒数 */
                            sizeof("Keep-Alive: timeout=") + TIME_T_LEN + 2,  /* 续上行：缓冲区上限 */
                            "Keep-Alive: timeout=" TIME_T_FMT CRLF,  /* 续上行：格式串 */
                            clcf->keepalive_header);  /* 续上行：超时秒数 */
        }  /* Keep-Alive输出判断结束 */

    } else {  /* 关闭连接 */
        b->last = ngx_cpymem(b->last, "Connection: close" CRLF,  /* 写close连接头(已含CRLF) */
                             sizeof("Connection: close" CRLF) - 1);  /* 续上行：整行长度 */
    }  /* Connection输出判断结束 */

    part = &r->headers_out.headers.part;  /* 通用头链表第一段 */
    header = part->elts;  /* 段内元素数组 */

    for (i = 0; /* void */; i++) {  /* 逐段遍历剩余通用头 */

        if (i >= part->nelts) {  /* 本段用尽 */
            if (part->next == NULL) {  /* 无下一段 */
                break;  /* 遍历结束 */
            }  /* 尾段判断结束 */

            part = part->next;  /* 切段 */
            header = part->elts;  /* 新段数组 */
            i = 0;  /* 下标归零 */
        }  /* 换段判断结束 */

        if (header[i].key.len == 0) {  /* 键已清空(已单独输出)的头 */
            continue;  /* 跳过防止重复发送 */
        }  /* 空键判断结束 */

        b->last = ngx_cpymem(b->last, header[i].key.data, header[i].key.len);  /* 写头名 */
        *(b->last++) = ':' ; *(b->last++) = ' ' ;  /* "名字: 值"的分隔冒号与空格 */

        b->last = ngx_cpymem(b->last, header[i].value.data,  /* 写头值 */
                             header[i].value.len);  /* 续上行：拷入头值 */
        *(b->last++) = CR; *(b->last++) = LF;  /* 行尾CRLF */
    }  /* 通用头输出循环结束 */

#if (NGX_DEBUG)  /* 仅调试构建编译此段 */
    *(b->last) = '\0';  /* 临时补'\0'便于按%s打印(随后会被结尾CRLF覆盖，不出现在响应里) */
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s\n", b->pos);  /* 完整打印响应头文本 */
#endif  /* 调试段条件编译结束 */

    /* the end of HTTP header */
    *(b->last++) = CR; *(b->last++) = LF;  /* 头终止空行：HTTP报文以CRLFCRLF分隔头与正文 */

    r->header_size = b->last - b->pos;  /* 记录头部长度(供统计与日志模块使用) */

    if (r->header_only) {  /* HEAD/304等无正文响应 */
        b->last_buf = 1;  /* 头buf兼作结束buf：写过滤器见last_buf即完成响应(并按需挥手) */
    }  /* 仅头判断结束 */

    if (!(ln = ngx_alloc_chain_link(r->pool))) {  /* 分配链节点承载头buf(内存池随请求释放) */
        return NGX_ERROR;  /* 分配失败 */
    }  /* 分配判断结束 */

    ln->buf = b;  /* 挂上响应头buf */
    ln->next = NULL;  /* 单节点链，以NULL收尾 */

    return ngx_http_write_filter(r, ln);  /* 交给写过滤链末端：头与已就绪的首段正文聚合进同一writev/TCP段发出 */
}  /* ngx_http_header_filter()结束 */


static ngx_int_t ngx_http_header_filter_init(ngx_cycle_t *cycle)  /* 模块初始化 */
{  /* 函数体开始 */
    ngx_http_top_header_filter = ngx_http_header_filter;  /* 注册为头过滤链顶端：响应产生后第一个被调用(且每请求仅一次) */

    return NGX_OK;  /* 初始化成功 */
}  /* ngx_http_header_filter_init()结束 */
