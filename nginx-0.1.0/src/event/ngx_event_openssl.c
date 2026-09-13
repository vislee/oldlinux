
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置总头：基础类型与编译期特性 */
#include <ngx_core.h>  /* 核心设施：连接、缓冲区、日志 */
#include <ngx_event.h>  /* 事件框架：读/写事件与定时器(SSL异步化依赖它) */


/* 前置声明：内部写函数，send_chain与关闭路径共用 */
static ngx_int_t ngx_ssl_write(ngx_connection_t *c, u_char *data, size_t size);


/*
 * ngx_ssl_init()：初始化OpenSSL库。
 * 在任何SSL操作前须调用一次：注册算法表并装载错误码描述串；
 * 证书/密钥等共享配置在SSL_CTX中创建，每个连接再实例化SSL对象。
 */
ngx_int_t ngx_ssl_init(ngx_log_t *log)
{
    SSL_library_init();  /* 注册全部密码算法(对称/摘要/非对称)：SSL算法子系统自举 */
    SSL_load_error_strings();  /* 装载错误码→可读串映射表：供ngx_ssl_error输出详情 */

    return NGX_OK;  /* 初始化完成 */
}  /* ngx_ssl_init()结束 */


/*
 * ngx_ssl_create_session()：为TCP连接创建SSL会话。
 * TLS握手在TCP三次握手完成之后进行(ClientHello/ServerHello→证书→
 * 密钥交换→Finished)；本函数只做绑定准备：分配SSL对象、关联socket、
 * 置服务器accept角色，真正的握手在首次读写时惰性驱动。
 */
ngx_int_t ngx_ssl_create_session(ngx_ssl_ctx_t *ssl_ctx, ngx_connection_t *c,
                                 ngx_uint_t flags)
{
    ngx_ssl_t  *ssl;  /* nginx侧SSL包装结构 */

    if (!(ssl = ngx_pcalloc(c->pool, sizeof(ngx_ssl_t)))) {  /* 从连接池分配并清零 */
        return NGX_ERROR;  /* 失败：连接无法启用TLS */
    }  /* 错误检查结束 */

    if (!(ssl->buf = ngx_create_temp_buf(c->pool, NGX_SSL_BUFSIZE))) {  /* 16K聚合缓冲：对齐TLS记录最大载荷 */
        return NGX_ERROR;  /* 失败 */
    }  /* 错误检查结束 */

    if (flags & NGX_SSL_BUFFER) {  /* 位与测试BUFFER标志 */
        ssl->buffer = 1;  /* 启用输出聚合：多条小数据合并为一个TLS记录，摊薄加密与分包开销 */
    }  /* 标志处理结束 */

    ssl->ssl = SSL_new(ssl_ctx);  /* 从SSL_CTX(证书/密钥/参数等共享配置)实例化每连接的SSL对象 */

    if (ssl->ssl == NULL) {  /* SSL_new失败 */
        ngx_ssl_error(NGX_LOG_ALERT, c->log, 0, "SSL_new() failed");  /* 输出OpenSSL错误栈 */
        return NGX_ERROR;  /* 失败 */
    }  /* 错误检查结束 */

    if (SSL_set_fd(ssl->ssl, c->fd) == 0) {  /* 把SSL层绑定到该连接的socket描述符 */
        ngx_ssl_error(NGX_LOG_ALERT, c->log, 0, "SSL_set_fd() failed");  /* 输出错误栈 */
        return NGX_ERROR;  /* 失败 */
    }  /* 错误检查结束 */

    SSL_set_accept_state(ssl->ssl);  /* 置服务器角色：握手状态机就绪，等待ClientHello */

    c->ssl = ssl;  /* 挂到连接上：此后该连接收发走SSL通道 */

    return NGX_OK;  /* 成功 */
}  /* ngx_ssl_create_session()结束 */


/*
 * ngx_ssl_recv()：连接的接收函数(可注册为c->recv)。
 * 从TLS层解密读数据；把OpenSSL在非阻塞fd上的WANT_READ/WANT_WRITE
 * 映射为nginx的NGX_AGAIN，从而融入epoll事件驱动模型。
 */
ngx_int_t ngx_ssl_recv(ngx_connection_t *c, u_char *buf, size_t size)
{
    int         n, sslerr;  /* n:读到的字节数 sslerr:SSL错误类别 */
    ngx_err_t   err;  /* 底层系统调用errno */
    char       *handshake;  /* 是否处于握手中的标注串 */

    n = SSL_read(c->ssl->ssl, buf, size);  /* 解密读：先收TLS记录再解密入buf(必要时内部续跑握手) */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL_read: %d", n);  /* 调试日志：SSL_read返回值 */

    if (n > 0) {  /* 读到明文数据 */
        return n;  /* 语义与recv(2)一致：调用方感觉不到加密层的存在 */
    }  /* 成功分支结束 */

    sslerr = SSL_get_error(c->ssl->ssl, n);  /* n≤0须查错误类别：SSL失败原因不止一种 */

    err = (sslerr == SSL_ERROR_SYSCALL) ? ngx_errno : 0;  /* 仅SYSCALL类错误携带errno(底层socket/IO出错) */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL_get_error: %d", sslerr);  /* 调试日志：错误类别 */

    if (sslerr == SSL_ERROR_WANT_READ) {  /* 需更多数据才能继续(等对端TLS记录) */
        return NGX_AGAIN;  /* 等价非阻塞recv的EAGAIN：挂起等可读事件 */
    }  /* WANT_READ分支结束 */

    if (sslerr == SSL_ERROR_WANT_WRITE) {  /* 读操作反而要写(握手期发ServerHello等) */
        ngx_log_error(NGX_LOG_ALERT, c->log, err,  /* 告警日志：附errno */
                      "SSL wants to write%s", handshake);  /* 日志正文 */
        return NGX_ERROR;  /* 本版未处理读中要写的情形：按错误处理 */
#if 0  /* 禁用的备选实现：返回AGAIN等可写事件 */
        return NGX_AGAIN;  /* (未启用)挂起等可写事件后重读 */
#endif  /* 禁用块结束 */
    }  /* WANT_WRITE分支结束 */

    if (!SSL_is_init_finished(c->ssl->ssl)) {  /* 握手未完成(还在协商/证书/密钥交换阶段) */
        handshake = "in SSL handshake";  /* 标注：错误发生在握手中 */

    } else {  /* 握手已完成 */
        handshake = "";  /* 标注留空 */
    }  /* 握手状态判断结束 */

    c->ssl->no_rcv_shut = 1;  /* 未见对端close_notify即断：记下以便shutdown时不再等它 */

    if (sslerr == SSL_ERROR_ZERO_RETURN || ERR_peek_error() == 0) {  /* 对端正常关闭(收到close_notify)或无库错误 */
        ngx_log_error(NGX_LOG_INFO, c->log, err,  /* info级：正常断开不算错误 */
                      "client closed connection%s", handshake);  /* 日志正文 */

        return NGX_ERROR;  /* 连接结束：通知上层走关闭流程 */
    }  /* 正常关闭分支结束 */

    ngx_ssl_error(NGX_LOG_ALERT, c->log, err,  /* 真正的协议/库错误：输出错误栈 */
                  "SSL_read() failed%s", handshake);  /* 日志正文 */

    return NGX_ERROR;  /* 出错 */
}  /* ngx_ssl_recv()结束 */


/*
 * OpenSSL has no SSL_writev() so we copy several bufs into our 16K buffer
 * before SSL_write() call to decrease a SSL overhead.
 *
 * Besides for protocols such as HTTP it is possible to always buffer
 * the output to decrease a SSL overhead some more.
 */

/*
 * ngx_ssl_send_chain()：连接的链式发送函数(注册为c->send_chain)。
 * OpenSSL没有SSL_writev，故把多个小buf聚进16K缓冲再一次SSL_write：
 * 每条TLS记录都有固定开销(记录头+MAC+填充+加密)，聚合可显著摊薄；
 * 单个大buf且无需聚合时走免拷贝直写快路径。
 */
ngx_chain_t *ngx_ssl_send_chain(ngx_connection_t *c, ngx_chain_t *in,
                                off_t limit)
{
    int          n;  /* 单次SSL_write的字节数 */
    ngx_uint_t   flush;  /* 是否立即冲刷缓冲 */
    ssize_t      send, size;  /* send:本次累计发送 size:本轮拷贝/发送量 */
    ngx_buf_t   *buf;  /* 16K输出聚合缓冲 */

    buf = c->ssl->buf;  /* 取连接专属的聚合缓冲 */

    if (in && in->next == NULL && !c->buffered && !c->ssl->buffer) {  /* 单buf、缓冲空、未开聚合：满足直写条件 */

        /*
         * we avoid a buffer copy if the incoming buf is a single,
         * our buffer is empty, and we do not need to buffer the output
         */

        n = ngx_ssl_write(c, in->buf->pos, in->buf->last - in->buf->pos);  /* 直接加密写出：省一次memcpy */

        if (n == NGX_ERROR) {  /* 写失败 */
            return NGX_CHAIN_ERROR;  /* 链错误标记：上层终止输出 */
        }  /* 错误检查结束 */

        if (n < 0) {  /* AGAIN映射为负值：本轮实际写出0字节 */
            n = 0;  /* 归一为0：指针不动，留待事件重试 */
        }  /* 负值归一结束 */

        in->buf->pos += n;  /* 读指针前移已发送部分：部分写时剩余留待下次 */

        return in;  /* 返回剩余链：非NULL表示尚有未发完数据 */
    }  /* 直写快路径结束 */

    send = 0;  /* 本次调用累计发送量清零 */
    flush = (in == NULL) ? 1 : 0;  /* in为空是冲刷请求：必须把缓冲存量发出 */

    for ( ;; ) {  /* 拷贝-发送交替的主循环 */

        while (in && buf->last < buf->end) {  /* 聚合阶段：向16K缓冲灌数据，灌满或链空为止 */
            if (in->buf->last_buf) {  /* 该buf带last_buf标记：响应的最后一块 */
                flush = 1;  /* 立即冲刷：让客户端尽早看到响应收尾 */
            }  /* last标记处理结束 */

            if (ngx_buf_special(in->buf)) {  /* 特殊buf(无数据，如flush/sync标记) */
                in = in->next;  /* 跳过：无字节可拷 */
                continue;  /* 处理下一节点 */
            }  /* 特殊buf跳过结束 */

            size = in->buf->last - in->buf->pos;  /* 当前buf未发送的数据量 */

            if (size > buf->end - buf->last) {  /* 超过聚合缓冲的剩余空间 */
                size = buf->end - buf->last;  /* 截断为本轮能装下的量(生产者-消费者节流) */
            }  /* 截断结束 */

            /*
             * TODO: the taking in->buf->flush into account can be
             *       implemented using the limit on the higher level
             */

            if (send + size > limit) {  /* 会超过本次调用的流量上限(限流) */
                size = limit - send;  /* 削减到剩余配额 */
                flush = 1;  /* 配额用尽前把缓冲冲出去 */
            }  /* 限流处理结束 */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志 */
                           "SSL buf copy: %d", size);  /* 打印本轮拷贝量 */

            ngx_memcpy(buf->last, in->buf->pos, size);  /* 用户态拷贝：数据进入聚合缓冲等待合并加密 */

            buf->last += size;  /* 缓冲写指针前移 */

            in->buf->pos += size;  /* 源buf读指针前移 */
            if (in->buf->pos == in->buf->last) {  /* 该buf已全部拷完 */
                in = in->next;  /* 切到链上下一个节点 */
            }  /* buf耗尽判断结束 */
        }  /* 聚合循环结束 */

        size = buf->last - buf->pos;  /* 聚合缓冲中待发送的数据量 */

        if (!flush && buf->last < buf->end && c->ssl->buffer) {  /* 无需冲刷、缓冲未满且开了聚合 */
            break;  /* 先攒着：凑更大的TLS记录以降低固定开销(Nagle式聚合思想) */
        }  /* 攒包判断结束 */

        n = ngx_ssl_write(c, buf->pos, size);  /* 把聚合数据加密发出 */

        if (n == NGX_ERROR) {  /* 写失败 */
            return NGX_CHAIN_ERROR;  /* 上层终止 */
        }  /* 错误检查结束 */

        if (n < 0) {  /* AGAIN按0字节处理 */
            n = 0;  /* 等可写事件重试 */
        }  /* 归一结束 */

        buf->pos += n;  /* 缓冲读指针前移(可能只发了一部分) */
        send += n;  /* 累计本次调用发送量 */
        c->sent += n;  /* 连接级发送计数：供流量统计/限速 */

        if (n < size) {  /* 部分写：socket发送缓冲已满(对应TCP拥塞/接收窗口填满) */
            break;  /* 剩余留待可写事件续发 */
        }  /* 部分写判断结束 */

        if (buf->pos == buf->last) {  /* 聚合缓冲已发空 */
            buf->pos = buf->start;  /* 复位读指针 */
            buf->last = buf->start;  /* 复位写指针：整块缓冲复用 */
        }  /* 缓冲复用结束 */

        if (in == NULL || send == limit) {  /* 链发完或配额用尽 */
            break;  /* 本次调用结束 */
        }  /* 结束判断结束 */
    }  /* 主循环结束 */

    c->buffered = (buf->pos < buf->last) ? 1 : 0;  /* 缓冲有存量则置buffered：事件循环据此继续监听可写 */

    return in;  /* 返回未发完的链(NULL表示全部发出) */
}  /* ngx_ssl_send_chain()结束 */


/*
 * ngx_ssl_write()：内部写封装。
 * 把OpenSSL的WANT_WRITE/WANT_READ映射为nginx的NGX_AGAIN语义，
 * 与非阻塞socket的EAGAIN及事件驱动模型衔接。
 */
static ngx_int_t ngx_ssl_write(ngx_connection_t *c, u_char *data, size_t size)
{
    int        n, sslerr;  /* n:写出字节数 sslerr:错误类别 */
    ngx_err_t  err;  /* 底层errno */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL to write: %d", size);  /* 调试日志：待写字节数 */

    n = SSL_write(c->ssl->ssl, data, size);  /* 加密写：明文封装成TLS记录后经socket发出 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL_write: %d", n);  /* 调试日志：返回值 */

    if (n > 0) {  /* 写成功 */
        return n;  /* 与send(2)语义一致 */
    }  /* 成功分支结束 */

    sslerr = SSL_get_error(c->ssl->ssl, n);  /* 查错误类别 */

    err = (sslerr == SSL_ERROR_SYSCALL) ? ngx_errno : 0;  /* 仅SYSCALL类携带errno */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL_get_error: %d", sslerr);  /* 调试日志：错误类别 */

    if (sslerr == SSL_ERROR_WANT_WRITE) {  /* socket发送缓冲满：对应EAGAIN */
        c->write->ready = 0;  /* 清就绪标志：等下一次可写事件再试 */
        return NGX_AGAIN;  /* 挂起，剩余数据续传 */
    }  /* WANT_WRITE分支结束 */

    if (sslerr == SSL_ERROR_WANT_READ) {  /* 写中要读(如重协商等罕见情形) */
        ngx_log_error(NGX_LOG_ALERT, c->log, err,  /* 告警日志：附errno */
                      "SSL wants to read%s", handshake);  /* 日志正文 */
        return NGX_ERROR;  /* 本版按错误处理 */
#if 0  /* 禁用的备选实现：返回AGAIN等可读事件 */
        return NGX_AGAIN;  /* (未启用)挂起等可读事件后续写 */
    }  /* (未启用)分支闭合 */
#endif  /* 禁用块结束 */

    c->ssl->no_rcv_shut = 1;  /* 异常路径：后续shutdown不再等对端close_notify */

    ngx_ssl_error(NGX_LOG_ALERT, c->log, err, "SSL_write() failed");  /* 输出OpenSSL错误栈 */

    return NGX_ERROR;  /* 出错 */
}  /* ngx_ssl_write()结束 */


/*
 * ngx_ssl_shutdown()：TLS层关闭(交换close_notify)。
 * TLS有独立的关闭规程：close_notify防止截断攻击(Truncation Attack)；
 * 该过程往往需多次事件驱动才能完成，如同TCP四次挥手是异步的。
 */
ngx_int_t ngx_ssl_shutdown(ngx_connection_t *c)
{
    int         n, sslerr;  /* n:shutdown返回值 sslerr:错误类别 */
    ngx_uint_t  again;  /* 是否还需再来一轮 */

    if (c->timedout) {  /* 连接已超时：跳过协商直接按双向关闭处理 */
        SSL_set_shutdown(c->ssl->ssl, SSL_RECEIVED_SHUTDOWN|SSL_SENT_SHUTDOWN);  /* 同时置收/发已关闭标志 */

    } else {  /* 正常关闭路径 */
        if (c->ssl->no_rcv_shut) {  /* 此前未收到对端close_notify */
            SSL_set_shutdown(c->ssl->ssl, SSL_RECEIVED_SHUTDOWN);  /* 补记"已收"：状态机不再等待 */
        }  /* 收向标志处理结束 */

        if (c->ssl->no_send_shut) {  /* 此前未发出close_notify */
            SSL_set_shutdown(c->ssl->ssl, SSL_SENT_SHUTDOWN);  /* 补记"已发" */
        }  /* 发向标志处理结束 */
    }  /* 超时分支结束 */

    again = 0;  /* 初始无需重试 */

    for ( ;; ) {  /* 关闭状态机循环 */
        n = SSL_shutdown(c->ssl->ssl);  /* 发/等close_notify：1=双向完成 0=已发待回 负=出错 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "SSL_shutdown: %d", n);  /* 调试日志：返回值 */

        if (n == 0) {  /* 第一阶段完成：close_notify已发出，等对方回执 */
            again = 1;  /* 标记需继续第二阶段 */
            break;  /* 跳出登记事件 */
        }  /* 阶段一结束 */

        if (n == 1) {  /* 双向关闭完成(收到对方close_notify) */
            SSL_free(c->ssl->ssl);  /* 释放SSL对象(会话按SSL_CTX缓存策略可复用) */
            c->ssl = NULL;  /* 摘除SSL：连接回归纯TCP语义 */
            return NGX_OK;  /* 完成 */
        }  /* 完成分支结束 */

        break;  /* 负数：出错，跳出查错误类别 */
    }  /* 状态机循环结束 */

    if (!again) {  /* 出错路径才需要取sslerr */
        sslerr = SSL_get_error(c->ssl->ssl, n);  /* 获取错误类别 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,  /* 调试日志 */
                       "SSL_get_error: %d", sslerr);  /* 打印错误类别 */
    }  /* 错误获取结束 */

    if (again || sslerr == SSL_ERROR_WANT_READ) {  /* 等对方close_notify(读方向) */

        ngx_add_timer(c->read, 10000);  /* 10秒定时器：对方不回即强制收尾(防资源悬挂) */

        if (ngx_handle_read_event(c->read, 0) == NGX_ERROR) {  /* 登记读事件：close_notify到达时回调 */
            return NGX_ERROR;  /* 登记失败 */
        }  /* 错误检查结束 */

        return NGX_AGAIN;  /* 异步等待第二阶段 */
    }  /* 等读分支结束 */

    if (sslerr == SSL_ERROR_WANT_WRITE) {  /* close_notify尚未发出(写缓冲满) */

        if (ngx_handle_write_event(c->write, 0) == NGX_ERROR) {  /* 登记写事件：可写时续发 */
            return NGX_ERROR;  /* 登记失败 */
        }  /* 错误检查结束 */

        return NGX_AGAIN;  /* 等可写续发 */
    }  /* 等写分支结束 */

    ngx_ssl_error(NGX_LOG_ALERT, c->log, 0, "SSL_shutdown() failed");  /* 输出OpenSSL错误栈 */

    return NGX_ERROR;  /* 关闭失败 */
}  /* ngx_ssl_shutdown()结束 */


/*
 * ngx_ssl_error()：把OpenSSL错误栈的可读描述拼进日志。
 * OpenSSL采用错误队列模型：失败后用ERR_get_error逐条取出错误码再转文字。
 */
void ngx_ssl_error(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
                   char *fmt, ...)
{
    int        len;  /* errstr当前已写长度 */
    char       errstr[NGX_MAX_CONF_ERRSTR];  /* 栈上消息缓冲 */
    va_list    args;  /* 可变参数指针 */

    va_start(args, fmt);  /* 开始遍历可变参数 */
    len = ngx_vsnprintf(errstr, sizeof(errstr) - 1, fmt, args);  /* 先格式化用户消息(预留1字节防溢出) */
    va_end(args);  /* 结束遍历 */

    errstr[len++] = ' ';  /* 追加分隔空格 */
    errstr[len++] = '(';  /* 开始OpenSSL错误段 */
    errstr[len++] = 'S';  /* 逐字符拼"SSL: "前缀 */
    errstr[len++] = 'S';  /* (不用字符串拷贝，直接写字节) */
    errstr[len++] = 'L';  /* 前缀第三字符 */
    errstr[len++] = ':';  /* 前缀冒号 */
    errstr[len++] = ' ';  /* 前缀尾空格 */

    ERR_error_string_n(ERR_get_error(), errstr + len, sizeof(errstr) - len - 1);  /* 取错误队列首条并转为可读串(预留结尾空间) */

    ngx_log_error(level, log, err, "%s)", errstr);  /* 补上闭括号输出最终日志 */
}  /* ngx_ssl_error()结束 */
