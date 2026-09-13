
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置总头：编译期特性探测结果与基础类型 */
#include <ngx_core.h>  /* Nginx核心设施：内存池、buf、临时文件读写 */
#include <ngx_event.h>  /* 事件驱动框架：读事件与定时器——读体靠可读事件异步推进 */
#include <ngx_http.h>  /* HTTP框架接口：请求结构r、核心loc配置等 */


/* 前置声明：读事件回调，内核收到新TCP段(可读就绪)时由事件循环调用 */
static void ngx_http_read_client_request_body_handler(ngx_event_t *rev);
/* 前置声明：读取循环实现，被入口函数与事件回调共同复用 */
static ngx_int_t ngx_http_do_read_client_request_body(ngx_http_request_t *r);


/*
 * ngx_http_read_client_request_body()：启动读取客户端请求体。
 * TCP是无消息边界的字节流，HTTP报文又没有帧定界，请求体的长度完全靠
 * 头部字段Content-Length声明——应用层用"长度字段"弥补传输层的定界缺失。
 * 解析头部时一次recv可能多读进了体的前缀(滞留在header_in中)，这里先把
 * 这部分"预读体"搬进请求体链，再按剩余量分配client_body_buffer_size
 * 的接收缓冲；数据已在内核缓冲则立即读，未到(NGX_AGAIN)则挂起等下一次
 * 可读事件，进程绝不阻塞在recv上。
 */
ngx_int_t ngx_http_read_client_request_body(ngx_http_request_t *r)  /* 入口：读体的启动函数 */
{
    ssize_t                    size;  /* 已预读的体字节数/后续读写长度 */
    ngx_buf_t                 *b;  /* 包装预读体数据的缓冲区 */
    ngx_chain_t               *cl;  /* 缓冲链节点：把buf串进请求体链 */
    ngx_http_core_loc_conf_t  *clcf;  /* 核心location配置：缓冲大小与读超时 */

    size = r->header_in->last - r->header_in->pos;  /* 头部缓冲中pos之后未消费的字节数=被预读进来的体前缀 */

    if (size) {  /* 有预读体数据：一次recv多带出来的部分，HTTP流水线的常见现象 */

        /* there is the pre-read part of the request body */

        /* 为预读体分配buf结构，失败返回500(内存耗尽的防御) */
        ngx_test_null(b, ngx_calloc_buf(r->pool),
                      NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 宏第二行：出错返回值 */

        b->temporary = 1;  /* 内存可写数据标记：下游filter(如gzip)可原地改写 */
        b->start = b->pos = r->header_in->pos;  /* 起点与读指针：指向header_in中预读体开头 */
        b->end = b->last = r->header_in->last;  /* 终点与写指针：对齐header_in尾部，界定预读体区间 */

        ngx_alloc_link_and_set_buf(r->request_body->bufs, b, r->pool,  /* 分配链节点，预读buf挂为请求体第一块 */
                                    NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 失败返回500 */

        if (size >= r->headers_in.content_length_n) {  /* 预读量≥声明长度：体已全部随头部到达 */

            /* the whole request body was pre-read */

            r->header_in->pos += r->headers_in.content_length_n;  /* 读指针越过属于体的字节，保留流水线中下一请求的数据 */

            r->request_body->handler(r->request_body->data);  /* 体读齐：回调注册的处理函数(如proxy转发) */

            return NGX_OK;  /* 同步完成，无需等待网络事件 */
        }  /* 整体预读分支结束 */

        r->header_in->pos = r->header_in->last;  /* 预读部分全部划归体：把头部缓冲消费干净 */
    }  /* 预读处理结束 */


    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取当前location的核心配置 */

    r->request_body->rest = r->headers_in.content_length_n - size;  /* 剩余待读=Content-Length−已预读(按长度定界) */

    /* 剩余量<缓冲上限的1.25倍：多留25%余量，尽量避免体溢出到磁盘临时文件 */
    if (r->request_body->rest
                             < clcf->client_body_buffer_size  /* 与配置的内存缓冲上限比较 */
                                        + (clcf->client_body_buffer_size >> 2))  /* 上限+25%(>>2即四分之一) */
    {
        size = r->request_body->rest;  /* 装得下：按实际剩余分配，不浪费内存 */

    } else {  /* 剩余量超出一次性缓冲能力 */
        size = clcf->client_body_buffer_size;  /* 只分配配置上限，超出部分将落盘临时文件 */
    }  /* 缓冲大小决策结束 */

    ngx_test_null(r->request_body->buf, ngx_create_temp_buf(r->pool, size),  /* 从池分配接收缓冲(temp型,filter可改写) */
                  NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 分配失败返回500 */

    ngx_alloc_link_and_set_buf(cl, r->request_body->buf, r->pool,  /* 分配链节点，把接收缓冲挂上请求体链 */
                               NGX_HTTP_INTERNAL_SERVER_ERROR);  /* 失败返回500 */

    if (r->request_body->bufs) {  /* 链上已有预读buf：接收缓冲接到它后面 */
        r->request_body->bufs->next = cl;  /* 链表接续：预读块在前，接收块在后 */

    } else {  /* 没有预读数据 */
        r->request_body->bufs = cl;  /* 接收缓冲就是请求体链的链头 */
    }  /* 链挂接结束 */

    /* 注册读事件回调：此后体数据全靠可读事件驱动送达(注册-通知模型) */
    r->connection->read->event_handler =
                                     ngx_http_read_client_request_body_handler;  /* 即上方声明的handler */

    return ngx_http_do_read_client_request_body(r);  /* 先同步读一轮：内核接收缓冲里可能已有数据 */
}  /* ngx_http_read_client_request_body()结束 */


/*
 * 读事件回调：事件循环发现连接可读(内核收到新TCP数据段)时被调用，
 * 是请求体异步读取的"续点"；读超时则终止请求。
 */
static void ngx_http_read_client_request_body_handler(ngx_event_t *rev)
{
    ngx_int_t            rc;  /* 读取循环返回的状态码 */
    ngx_connection_t    *c;  /* TCP连接对象 */
    ngx_http_request_t  *r;  /* HTTP请求对象 */

    c = rev->data;  /* 事件回指连接：nginx用data指针串起事件→连接→请求上下文链 */
    r = c->data;  /* 连接再回指请求：拿到HTTP层上下文 */

    if (rev->timedout) {  /* client_body_timeout到期：对端迟迟不发数据 */
        ngx_http_finalize_request(r, NGX_HTTP_REQUEST_TIME_OUT);  /* 以408终止：防异常/恶意对端无限占用连接 */
        return;  /* 请求已结束 */
    }  /* 超时分支结束 */

    rc = ngx_http_do_read_client_request_body(r);  /* 事件就绪：继续读取循环，吃完内核缓冲 */

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {  /* 返回值达到4xx/5xx错误门槛 */
        ngx_http_finalize_request(r, rc);  /* 终止请求并生成错误响应 */
    }  /* 错误处理结束 */
}  /* 读事件回调结束 */


/*
 * ngx_http_do_read_client_request_body()：读取循环核心。
 * 反复以非阻塞recv把内核socket接收缓冲的数据拷入r->request_body->buf；
 * 缓冲写满则整块落盘到临时文件(底层pwrite按偏移写)后复用缓冲；直到收满
 * Content-Length字节。返回NGX_AGAIN表示本轮无数据可读，挂起等事件。
 */
static ngx_int_t ngx_http_do_read_client_request_body(ngx_http_request_t *r)
{
    size_t                     size;  /* 本轮最多可读的字节数 */
    ssize_t                    n;  /* 单次系统调用的实际字节数(正数/0/AGAIN) */
    ngx_buf_t                 *b;  /* 落盘后描述文件区间的buf */
    ngx_connection_t          *c;  /* 当前TCP连接 */
    ngx_http_core_loc_conf_t  *clcf;  /* 核心配置：读超时 */

    c = r->connection;  /* 缓存连接指针：循环内高频访问 */

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* 调试日志(仅debug编译生效) */
                   "http read client request body");  /* 日志正文 */

    for ( ;; ) {  /* 一次吃完内核缓冲：减少epoll唤醒次数(中断/上下文切换开销) */
        if (r->request_body->buf->last == r->request_body->buf->end) {  /* 内存缓冲写满：先腾空才能继续收 */
            n = ngx_write_chain_to_temp_file(r->request_body->temp_file,  /* 内存链批量落盘(内部pwrite按offset写) */
                     r->request_body->bufs->next ? r->request_body->bufs->next:  /* 三目：取接收缓冲所在链节点 */
                                                   r->request_body->bufs);  /* 无next时就是链头(预读块始终留内存) */

            /* TODO: n == 0 or not complete and level event */

            if (n == NGX_ERROR) {  /* 写文件失败：磁盘满/IO错误等 */
                return NGX_HTTP_INTERNAL_SERVER_ERROR;  /* 500：服务端自身故障 */
            }  /* 错误检查结束 */

            r->request_body->temp_file->offset += n;  /* 文件偏移前移：记录已落盘字节数，供追加写与回读定位 */

            r->request_body->buf->pos = r->request_body->buf->start;  /* 复位读指针 */
            r->request_body->buf->last = r->request_body->buf->start;  /* 复位写指针：整块缓冲重新可用 */
        }  /* 落盘腾空结束 */

        size = r->request_body->buf->end - r->request_body->buf->last;  /* 缓冲当前空闲空间 */

        if (size > r->request_body->rest) {  /* 空闲比剩余应读量大：只读rest */
            size = r->request_body->rest;  /* 以Content-Length定界，绝不越界吃进流水线下一请求 */
        }  /* 读长度裁剪结束 */

        n = c->recv(c, r->request_body->buf->last, size);  /* 非阻塞recv：内核态→用户态拷贝；无数据返回NGX_AGAIN(≈EAGAIN) */

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* 调试日志 */
                       "http client request body recv " SIZE_T_FMT, n);  /* 打印本次recv结果 */

        if (n == NGX_AGAIN) {  /* 内核接收缓冲已空：数据未到，本轮结束 */
            clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);  /* 取配置：读超时 */
            ngx_add_timer(c->read, clcf->client_body_timeout);  /* 挂读定时器：对端停发超时即408，防连接悬挂 */

            if (ngx_handle_read_event(c->read, 0) == NGX_ERROR) {  /* 向epoll/select登记读兴趣，就绪后回调注册的handler */
                return NGX_HTTP_INTERNAL_SERVER_ERROR;  /* 登记失败：500 */
            }  /* 错误检查结束 */

            return NGX_AGAIN;  /* 让出控制权回事件循环：进程绝不在recv上阻塞 */
        }  /* AGAIN分支结束 */

        if (n == 0) {  /* recv返回0：对端发FIN，体传完前客户端就关闭了连接 */
            ngx_log_error(NGX_LOG_INFO, c->log, 0,  /* info级：提前断开属常见现象 */
                          "client closed prematurely connection");  /* 日志正文 */
        }  /* 提前关闭日志结束 */

        if (n == 0 || n == NGX_ERROR) {  /* 对端提前关闭或读出错：体不完整，请求无效 */
            r->closed = 1;  /* 标记连接已坏：keep-alive作废，等待回收 */
            return NGX_HTTP_BAD_REQUEST;  /* 400：请求体不完整 */
        }  /* 错误分支结束 */

        r->request_body->buf->last += n;  /* 写指针前移：n字节已拷入用户态缓冲 */
        r->request_body->rest -= n;  /* 剩余量扣减：逐步逼近Content-Length边界 */

        if (r->request_body->rest == 0) {  /* 收满声明长度：体读取完成 */
            break;  /* 跳出循环 */
        }  /* 完成判断结束 */

        if (r->request_body->buf->last < r->request_body->buf->end) {  /* 缓冲未满：内核缓冲也已被读空 */
            break;  /* 返回等下一次可读事件，避免忙等空转 */
        }  /* 继续读判断结束 */
    }  /* 读取循环结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,  /* 调试日志 */
                   "http client request body rest " SIZE_T_FMT,  /* 打印剩余量 */
                   r->request_body->rest);  /* 参数：rest */

    if (r->request_body->rest) {  /* 因缓冲/内核空而退出但体未读完 */
        return NGX_AGAIN;  /* 挂起等待下一次可读事件 */
    }  /* 未读完分支结束 */

    if (r->request_body->temp_file->file.fd != NGX_INVALID_FILE) {  /* 曾发生落盘：体主体已在临时文件中 */

        /* save the last part */
        n = ngx_write_chain_to_temp_file(r->request_body->temp_file,  /* 把内存中最后一段体追加写入临时文件 */
                     r->request_body->bufs->next ? r->request_body->bufs->next:  /* 三目：取接收缓冲链节点 */
                                                   r->request_body->bufs);  /* 同样跳过预读块，只落盘接收数据 */

        /* TODO: n == 0 or not complete and level event */

        if (n == NGX_ERROR) {  /* 写文件失败 */
            return NGX_HTTP_INTERNAL_SERVER_ERROR;  /* 500 */  /* 写临时文件失败：向上层报 500 内部服务器错误 */
        }  /* 错误检查结束 */

        if (!(b = ngx_calloc_buf(r->pool))) {  /* 分配描述文件区间的buf */
            return NGX_HTTP_INTERNAL_SERVER_ERROR;  /* 失败：500 */
        }  /* 错误检查结束 */

        b->in_file = 1;  /* 数据在文件而非内存：下游按文件区间读取 */
        b->file_pos = 0;  /* 区间起点：体从临时文件头开始 */
        b->file_last = r->request_body->temp_file->file.offset;  /* 区间终点=已写入的总偏移 */
        b->file = &r->request_body->temp_file->file;  /* 关联文件对象：下游可sendfile/pread回读 */

        if (r->request_body->bufs->next) {  /* 有预读块：文件buf替换第二块 */
            r->request_body->bufs->next->buf = b;  /* 已落盘的接收缓冲被文件buf取代 */

        } else {  /* 没有预读块 */
            r->request_body->bufs->buf = b;  /* 文件buf就是链头 */
        }  /* 链替换结束 */
    }  /* 落盘收尾结束 */

    r->request_body->handler(r->request_body->data);  /* 体读取完毕：触发注册回调进入下一阶段(如转发upstream) */

    return NGX_OK;  /* 成功完成 */
}  /* ngx_http_do_read_client_request_body()结束 */
