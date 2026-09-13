
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置头：编译期探测宏与平台相关类型定义 */
#include <ngx_core.h>    /* 核心头：buf/字符串/日志等基础设施声明 */
#include <ngx_http.h>    /* HTTP框架头：请求结构与NGX_HTTP_PARSE_*解析错误码声明 */


/*
 * ngx_http_parse_request_line()：请求行逐字节状态机——TCP粘包/拆包问题的
 * 教科书级解法。TCP是字节流协议，不保留应用层消息边界：一次recv可能只读到
 * 半个请求行，也可能一次捎来1.5个请求。应用层协议必须自带边界语法
 * （HTTP以CRLF结束请求行），解析器则必须做到：
 * 1) 逐字节消费，用state记住"当前解析到哪个语法成分"；
 * 2) 数据不够时把state外置到r->state并返回NGX_AGAIN，挂起等下一次可读
 *    事件，再从断点无损续析（这就是事件驱动与流式协议解析的配合）；
 * 3) 协议错误返回NGX_HTTP_PARSE_INVALID_*，由上层映射为400等响应。
 * 请求行语法：METHOD SP URI [SP HTTP/x.y] CRLF（HTTP/0.9无版本段）。
 */
ngx_int_t ngx_http_parse_request_line(ngx_http_request_t *r, ngx_buf_t *b)  /* r=请求(保存断点与各段定位) b=本次事件读到的字节缓冲 */
{  /* 函数体开始 */
    u_char  ch, *p, *m;  /* ch=当前消费的字符; p=读游标; m=方法名首址(方法识别用) */
    enum {  /* 请求行状态机的全部状态：状态外置=解析进度可保存、跨包可恢复 */
        sw_start = 0,  /* 起始态：等请求行真正的首字节(完成后也复位到此态) */
        sw_method,  /* 方法名中段：继续收大写字母，直到空格分隔符 */
        sw_space_after_method,  /* 方法后的单个空格态(0.1.0状态图中无迁入，保留) */
        sw_spaces_before_uri,  /* URI前空格段：兼容"GET  /"多空格的宽松写法 */
        sw_schema,  /* 绝对URI的scheme段：如"http" */
        sw_schema_slash,  /* scheme后的第一个'/' */
        sw_schema_slash_slash,  /* scheme后的第二个'/'：其后即主机名 */
        sw_host,  /* 主机名段：字母数字与.-，遇':'(端口)或'/'(路径)结束 */
        sw_port,  /* 端口段：十进制数字，遇'/'转入路径 */
        sw_after_slash_in_uri,  /* 刚越过'/'：警惕/./、//、%XX等待规范化序列 */
        sw_check_uri,  /* URI普通字符段：顺带记录文件扩展名起点 */
        sw_uri,  /* 已判明为复杂URI(含%XX或参数)：无需再查'/'后特殊序列 */
        sw_http_09,  /* URI后空白段：若等不到版本号就按HTTP/0.9请求收尾 */
        sw_http_H,  /* 版本串已收'H' */
        sw_http_HT,  /* 版本串已收"HT" */
        sw_http_HTT,  /* 版本串已收"HTT" */
        sw_http_HTTP,  /* 版本串已收"HTTP"：期待'/' */
        sw_first_major_digit,  /* 主版本号首位：RFC不允许前导0 */
        sw_major_digit,  /* 主版本号后续数字或小数点 */
        sw_first_minor_digit,  /* 次版本号首位 */
        sw_minor_digit,  /* 次版本号后续数字或行结束符 */
        sw_almost_done,  /* 已见CR只差LF：应对拆包恰好断在CR与LF之间的情形 */
        sw_done  /* 请求行完成：while循环的终止哨兵 */
    } state;  /* 局部状态变量：退出前回存r->state，实现跨事件续析 */

    state = r->state;  /* 恢复上次断点：本次读到的很可能只是上一包的后半截 */
    p = b->pos;  /* 读游标从buf未消费处起：pos/last正是本次recv到的字节区间 */

    while (p < b->last && state < sw_done) {  /* 尚有未消费字节且未达终态：在"剩余字节×当前状态"平面上推进 */
        ch = *p++;  /* 逐字节取出并前移游标：流式解析只依赖当前字符+状态，不要求整行在buf中连续 */

        /* gcc 2.95.2 and msvc 6.0 compile this switch as an jump table */

        switch (state) {  /* 按当前状态分派字符：编译为跳转表，单次迁移O(1) */

        /* HTTP methods: GET, HEAD, POST */
        case sw_start:  /* 起始态：确定请求行从哪个字节真正开始 */
            r->request_start = p - 1;  /* 记录请求行首址(p已前移故回退1)：供后续按指针切分各段 */

            if (ch == CR || ch == LF) {  /* 空行(CR/LF)：跳过请求间的多余CRLF——字节流中行的边界只由这两个字符界定 */
                break;  /* 停留sw_start继续等下一字节：来什么消化什么 */
            }  /* 空行分支结束 */

            if (ch < 'A' || ch > 'Z') {  /* 方法必须是全大写字母(RFC2616：method区分大小写且约定大写) */
                return NGX_HTTP_PARSE_INVALID_METHOD;  /* 非法方法：上层映射为501 Not Implemented */
            }  /* 首字符校验结束 */

            state = sw_method;  /* 迁入方法名状态：后续字节按方法名语法消化 */
            break;  /* 本字节处理完毕，回while取下一字节 */

        case sw_method:  /* 方法名状态：逐字收集，靠空格定界 */
            if (ch == ' ') {  /* 空格=方法名结束符：字节流没有消息边界，语法成分只能靠分隔符切分 */
                r->method_end = p - 1;  /* 记方法名末址(p已越过空格故回退1) */
                m = r->request_start;  /* m指向方法名首字节：开始整串比对 */

                if (r->method_end - m == 3) {  /* 长度为3才可能是GET：先比长度，省掉逐字符比较 */

                    if (m[0] == 'G' && m[1] == 'E' && m[2] == 'T') {  /* 精确匹配"GET" */
                        r->method = NGX_HTTP_GET;  /* 记为GET：幂等只读方法，可被缓存 */
                    }  /* GET匹配结束 */

                } else if (r->method_end - m == 4) {  /* 长度为4：候选是POST/HEAD */

                    if (m[0] == 'P' && m[1] == 'O'  /* 比对"POST"前两字符：注意此0.1.0源码接着比的是'T','T'(笔误) */
                        && m[2] == 'T' && m[3] == 'T')  /* 后两字符比'TT'而非"ST"，故POST实际永远匹配不上(0.1.0已知笔误，原样保留) */
                    {  /* POST匹配块开始 */
                        r->method = NGX_HTTP_POST;  /* 记为POST：携带请求体的非幂等方法 */

                    } else if (m[0] == 'H' && m[1] == 'E'  /* 比对"HEAD"前两字符 */
                               && m[2] == 'A' && m[3] == 'D')  /* 比对"HEAD"后两字符 */
                    {  /* HEAD匹配块开始 */
                        r->method = NGX_HTTP_HEAD;  /* 记为HEAD：只要响应头不要正文 */
                    }  /* HEAD匹配块结束 */
                }  /* 长度4方法比对链结束 */

                state = sw_spaces_before_uri;  /* 方法识别完毕，转入URI前空格状态 */
                break;  /* 该空格字节处理完 */
            }  /* 空格定界分支结束 */

            if (ch < 'A' || ch > 'Z') {  /* 方法名中途混入非大写字母 */
                return NGX_HTTP_PARSE_INVALID_METHOD;  /* 仍是非法方法：报501类错误 */
            }  /* 方法名中途校验结束 */

            break;  /* 继续收方法名的后续字节 */

        /* single space after method */
        case sw_space_after_method:  /* 方法后单空格态(本版无迁入路径，语义见下) */
            switch (ch) {  /* 对当前字符分派 */
            case ' ':  /* 恰一个空格：唯一合法分隔 */
                state = sw_spaces_before_uri;  /* 进入URI前空格状态 */
                break;  /* 字符处理完 */
            default:  /* 其余任何字符 */
                return NGX_HTTP_PARSE_INVALID_METHOD;  /* 都属方法区语法错误 */
            }  /* 方法后分隔符分派结束 */
            break;  /* 状态处理完毕 */

        /* space* before URI */
        case sw_spaces_before_uri:  /* URI前空格段：区分相对URI("/x")与绝对URI("http://x") */
            switch (ch) {  /* 对当前字符分派 */
            case '/':  /* 以'/'开头：相对URI，服务器按Host头补全主机 */
                r->uri_start = p - 1;  /* 记URI起点(p-1：'/'本身属于URI) */
                state = sw_after_slash_in_uri;  /* 刚越过'/'，转入警惕特殊序列的状态 */
                break;  /* 字符处理完 */
            case ' ':  /* 又一个空格：继续吞掉，宽容处理 */
                break;  /* 留在本状态等URI首字符 */
            default:  /* 其它字符：只可能是scheme首字母 */
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {  /* 字母：进入绝对URI的scheme段 */
                    r->schema_start = p - 1;  /* 记scheme起点 */
                    state = sw_schema;  /* 迁入scheme状态 */
                    break;  /* 字符处理完 */
                }  /* scheme首字母判断结束 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 既非'/'、空格又非字母：请求行语法错误(400) */
            }  /* URI首字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_schema:  /* scheme段：收"http"这类协议名 */
            switch (ch) {  /* 对当前字符分派 */
            case ':':  /* scheme结束定界符：形如"http:" */
                r->schema_end = p - 1;  /* 记scheme终点 */
                state = sw_schema_slash;  /* 紧接着应出现"//"的第一个'/' */
                break;  /* 字符处理完 */
            default:  /* 其它字符：scheme只能由字母构成 */
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {  /* 合法scheme字符 */
                    break;  /* 停留本状态继续收 */
                }  /* scheme字符校验结束 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* scheme中出现杂字符：请求行错误(400) */
            }  /* scheme字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_schema_slash:  /* 已收"scheme/"：等待第二个'/' */
            switch (ch) {  /* 对当前字符分派 */
            case '/':  /* 第二个'/'到位："http://"前缀齐了 */
                state = sw_schema_slash_slash;  /* 转入主机名状态 */
                break;  /* 字符处理完 */
            default:  /* 缺少第二个'/' */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 绝对URI格式残缺(400) */
            }  /* 第二斜杠分派结束 */
            break;  /* 状态处理完毕 */

        case sw_schema_slash_slash:  /* 已收"scheme//"：等待主机名首字符 */
            switch (ch) {  /* 对当前字符分派 */
            case '/':  /* 又遇'/'：主机名从这里开始(如"//"后直接跟host) */
                r->host_start = p - 1;  /* 记主机名起点 */
                state = sw_host;  /* 迁入主机名状态 */
                break;  /* 字符处理完 */
            default:  /* 其它字符：规范化形式要求第三个'/' */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* "http:x"这类写法不接受(400) */
            }  /* 主机名首字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_host:  /* 主机名段：绝对URI中host:port的host部分 */
            switch (ch) {  /* 对当前字符分派 */
            case ':':  /* 冒号：主机名结束，后面跟端口 */
                r->host_end = p - 1;  /* 记主机名终点 */
                state = sw_port;  /* 迁入端口状态 */
                break;  /* 字符处理完 */
            case '/':  /* 无端口直接跟路径：host到'/'前为止 */
                r->host_end = p - 1;  /* 记主机名终点 */
                r->uri_start = p - 1;  /* 该'/'即URI首字符 */
                state = sw_after_slash_in_uri;  /* 越过'/'，转入URI警惕状态 */
                break;  /* 字符处理完 */
            default:  /* 其它字符：校验主机名合法字符集 */
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')  /* 字母 */
                    || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')  /* 数字、点、连字符：DNS名语法 */
                {  /* 合法字符集合判断 */
                    break;  /* 停留本状态继续收主机名 */
                }  /* 主机名字符校验结束 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 主机名含非法字符(400) */
            }  /* 主机名字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_port:  /* 端口段：host后的十进制端口号 */
            switch (ch) {  /* 对当前字符分派 */
            case '/':  /* 端口收完转入路径 */
                r->port_end = p - 1;  /* 记端口终点 */
                r->uri_start = p - 1;  /* 该'/'即URI首字符 */
                state = sw_after_slash_in_uri;  /* 越过'/'，转入URI警惕状态 */
                break;  /* 字符处理完 */
            default:  /* 其它字符：应为数字 */
                if (ch < '0' && ch > '9') {  /* 0.1.0原笔误(逻辑恒假，应为||)，原样保留：端口未真正校验 */
                    return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 原意是拒绝非数字端口(400) */
                }  /* 端口校验块结束(恒假笔误，见上) */
                break;  /* 数字或任意字符都停留本状态继续收 */
            }  /* 端口字符分派结束 */
            break;  /* 状态处理完毕 */

        /* check "/.", "//", and "%" in URI */
        case sw_after_slash_in_uri:  /* 刚越过'/'的URI态：为URI规范化预判/./、//、%XX */
            switch (ch) {  /* 对当前字符分派 */
            case CR:  /* 行结束CR：URI到此为止，且无版本段 */
                r->uri_end = p - 1;  /* 记URI终点(CR不算URI内容) */
                r->http_minor = 9;  /* 无版本号=HTTP/0.9请求 */
                state = sw_almost_done;  /* 只差一个LF：请求行收尾 */
                break;  /* 字符处理完 */
            case LF:  /* 宽容裸LF作为行结束(某些客户端不发CR) */
                r->uri_end = p - 1;  /* 记URI终点 */
                r->http_minor = 9;  /* 同样按0.9处理 */
                state = sw_done;  /* 请求行直接完成 */
                break;  /* 字符处理完 */
            case ' ':  /* 空格：URI结束且后面无版本串(0.9风格的"GET /x \r\n") */
                r->uri_end = p - 1;  /* 记URI终点 */
                state = sw_http_09;  /* 进入URI后空白/版本判定状态 */
                break;  /* 字符处理完 */
            case '.':  /* '/'后紧跟'.'：构成/./或/../候选，需规范化 */
            case '%':  /* 出现%XX转义：需解码后重新走本状态机 */
                r->complex_uri = 1;  /* 标记复杂URI：解析头后还要过ngx_http_parse_complex_uri */
                state = sw_uri;  /* 迁入通用URI状态(不再逐步预判) */
                break;  /* 字符处理完 */
            case '/':  /* 连续'//'：空路径段，也是待规范化的序列 */
                r->complex_uri = 1;  /* 同样标记为复杂URI */
                break;  /* 留在本状态继续盯下一个字符 */
            case '?':  /* 问号：路径结束，查询串(参数)开始 */
                r->args_start = p;  /* 记参数串起点(p未回退，'?'不属于参数) */
                state = sw_uri;  /* 参数段中不再做'/'类规范化预判 */
                break;  /* 字符处理完 */
            default:  /* 普通字符：无特殊序列风险 */
                state = sw_check_uri;  /* 迁入普通URI扫描状态 */
                break;  /* 字符处理完 */
            }  /* 斜杠后字符分派结束 */
            break;  /* 状态处理完毕 */

        /* check "/" and "%" in URI */
        case sw_check_uri:  /* URI普通字符扫描态：顺带定位扩展名、监视%与'/' */
            switch (ch) {  /* 对当前字符分派 */
            case CR:  /* 行结束CR：URI结束且无版本段 */
                r->uri_end = p - 1;  /* 记URI终点 */
                r->http_minor = 9;  /* 无版本号=HTTP/0.9 */
                state = sw_almost_done;  /* 等随行的LF */
                break;  /* 字符处理完 */
            case LF:  /* 宽容裸LF作行结束 */
                r->uri_end = p - 1;  /* 记URI终点 */
                r->http_minor = 9;  /* 按0.9处理 */
                state = sw_done;  /* 请求行完成 */
                break;  /* 字符处理完 */
            case ' ':  /* 空格：URI后还有内容(可能版本串) */
                r->uri_end = p - 1;  /* 记URI终点 */
                state = sw_http_09;  /* 进入URI后判定状态 */
                break;  /* 字符处理完 */
            case '.':  /* 点：可能是文件扩展名起点(如".html") */
                r->uri_ext = p;  /* 记扩展名候选起点：随扫描推进可被后面的'/'作废 */
                break;  /* 留在本状态 */
            case '/':  /* 新的'/'：之前记的扩展名属于上一段目录，作废 */
                r->uri_ext = NULL;  /* 清掉扩展名候选 */
                state = sw_after_slash_in_uri;  /* 越过'/'，回到警惕特殊序列的状态 */
                break;  /* 字符处理完 */
            case '%':  /* %XX转义：解码后可能产生/./、/../，必须走规范化 */
                r->complex_uri = 1;  /* 标记复杂URI */
                state = sw_uri;  /* 迁入通用URI状态 */
                break;  /* 字符处理完 */
            case '?':  /* 问号：路径与查询串的分界 */
                r->args_start = p;  /* 记参数串起点 */
                state = sw_uri;  /* 迁入通用URI状态 */
                break;  /* 字符处理完 */
            }  /* URI扫描字符分派结束 */
            break;  /* 其它字符留在本状态继续扫 */

        /* URI */
        case sw_uri:  /* 通用URI态：已判明复杂(含%XX/参数)或已入参数段，只关心URI何时结束 */
            switch (ch) {  /* 对当前字符分派 */
            case CR:  /* 行结束CR：URI结束且无版本段 */
                r->uri_end = p - 1;  /* 记URI终点 */
                r->http_minor = 9;  /* 无版本号=HTTP/0.9 */
                state = sw_almost_done;  /* 等LF收尾 */
                break;  /* 字符处理完 */
            case LF:  /* 宽容裸LF作行结束 */
                r->uri_end = p - 1;  /* 记URI终点 */
                r->http_minor = 9;  /* 按0.9处理 */
                state = sw_done;  /* 请求行完成 */
                break;  /* 字符处理完 */
            case ' ':  /* 空格：URI后是版本段或0.9式收尾 */
                r->uri_end = p - 1;  /* 记URI终点 */
                state = sw_http_09;  /* 进入URI后判定状态 */
                break;  /* 字符处理完 */
            }  /* 通用URI字符分派结束 */
            break;  /* 其余字符(含参数与转义)一律留本状态继续吞 */

        /* space+ after URI */
        case sw_http_09:  /* URI后判定态：随后的字节决定这是0.9请求还是1.x版本串 */
            switch (ch) {  /* 对当前字符分派 */
            case ' ':  /* 又是空格：继续吞，版本串还没露头 */
                break;  /* 留在本状态 */
            case CR:  /* 直接CR：后面不会有版本号了 */
                r->http_minor = 9;  /* 判为HTTP/0.9：响应只发正文不发头 */
                state = sw_almost_done;  /* 等LF */
                break;  /* 字符处理完 */
            case LF:  /* 裸LF：同上按0.9收尾 */
                r->http_minor = 9;  /* 判为0.9 */
                state = sw_done;  /* 请求行完成 */
                break;  /* 字符处理完 */
            case 'H':  /* 出现'H'：多半是"HTTP/x.y"版本串的开头 */
                state = sw_http_H;  /* 逐字符校验版本串：这是把一行当作多状态序列来消化的典型 */
                break;  /* 字符处理完 */
            default:  /* 既非空格/CR/LF又非'H' */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* URI后的垃圾字节：请求行错误(400) */
            }  /* 0.9判定字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_http_H:  /* 版本串已收'H'：校验第2字符 */
            switch (ch) {  /* 对当前字符分派 */
            case 'T':  /* 'T'到位："HT" */
                state = sw_http_HT;  /* 推进到下一状态 */
                break;  /* 字符处理完 */
            default:  /* 其它字符："H"后不是HTTP版本串 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 请求行错误(400) */
            }  /* 版本串字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_http_HT:  /* 已收"HT"：校验第3字符 */
            switch (ch) {  /* 对当前字符分派 */
            case 'T':  /* 'T'到位："HTT" */
                state = sw_http_HTT;  /* 推进 */
                break;  /* 字符处理完 */
            default:  /* 偏离"HTTP"字面量 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 请求行错误(400) */
            }  /* 版本串字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_http_HTT:  /* 已收"HTT"：校验第4字符 */
            switch (ch) {  /* 对当前字符分派 */
            case 'P':  /* 'P'到位："HTTP" */
                state = sw_http_HTTP;  /* 推进：只差'/' */
                break;  /* 字符处理完 */
            default:  /* 不是HTTP字样 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 请求行错误(400) */
            }  /* 版本串字符分派结束 */
            break;  /* 状态处理完毕 */

        case sw_http_HTTP:  /* 已收"HTTP"：等待版本分隔符'/' */
            switch (ch) {  /* 对当前字符分派 */
            case '/':  /* '/'到位：进入版本号部分 */
                state = sw_first_major_digit;  /* 先收主版本号首位 */
                break;  /* 字符处理完 */
            default:  /* "HTTP"后不是'/' */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 请求行错误(400) */
            }  /* 版本分隔符分派结束 */
            break;  /* 状态处理完毕 */

        /* first digit of major HTTP version */
        case sw_first_major_digit:  /* 主版本号首位：决定协议代际(1.x) */
            if (ch < '1' || ch > '9') {  /* 首位不允许0(RFC禁止前导零，杜绝"HTTP/01.1"歧义) */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 非法版本号(400) */
            }  /* 首位非0校验结束 */

            r->http_major = ch - '0';  /* 字符转数值：主版本号 */
            state = sw_major_digit;  /* 转入后续数字/小数点状态 */
            break;  /* 字符处理完 */

        /* major HTTP version or dot */
        case sw_major_digit:  /* 主版本号后续：更多数字或转入小版本 */
            if (ch == '.') {  /* 小数点：主次版本分界 */
                state = sw_first_minor_digit;  /* 转收次版本号首位 */
                break;  /* 字符处理完 */
            }  /* 小数点分支结束 */

            if (ch < '0' || ch > '9') {  /* 既非点又非数字 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 版本号语法错误(400) */
            }  /* 主版本数字校验结束 */

            r->http_major = r->http_major * 10 + ch - '0';  /* 逐位累加：`*10+数字`是流式读多位数的标准手法 */
            break;  /* 留在本状态继续收 */

        /* first digit of minor HTTP version */
        case sw_first_minor_digit:  /* 次版本号首位 */
            if (ch < '0' || ch > '9') {  /* 必须是数字 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 版本号语法错误(400) */
            }  /* 次版本首位校验结束 */

            r->http_minor = ch - '0';  /* 字符转数值：次版本号 */
            state = sw_minor_digit;  /* 转入后续数字/行结束状态 */
            break;  /* 字符处理完 */

        /* minor HTTP version or end of request line */
        case sw_minor_digit:  /* 次版本号后续：数字或行结束符——请求行在此收尾 */
            if (ch == CR) {  /* 见CR：行将结束 */
                state = sw_almost_done;  /* 拆包可能断在CR/LF之间，单设状态记住这个进度 */
                break;  /* 字符处理完 */
            }  /* 裸LF分支结束 */

            if (ch == LF) {  /* 裸LF：行直接结束(宽容无CR的客户端) */
                state = sw_done;  /* 请求行完成 */
                break;  /* 字符处理完 */
            }  /* CR分支结束 */

            if (ch < '0' || ch > '9') {  /* 既非数字也非行结束符 */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 版本号后跟垃圾字节(400) */
            }  /* 次版本数字校验结束 */

            r->http_minor = r->http_minor * 10 + ch - '0';  /* 逐位累加次版本号 */
            break;  /* 留在本状态继续收次版本号 */

        /* end of request line */
        case sw_almost_done:  /* 已收CR只差LF：请求行的最后一步 */
            r->request_end = p - 2;  /* 请求行终点在CR前(p已越过CR，再回退1)：按指针切分不再扫回 */
            switch (ch) {  /* 对当前字符分派 */
            case LF:  /* LF到位：CRLF齐全，行边界确认 */
                state = sw_done;  /* 请求行完成 */
                break;  /* 字符处理完 */
            default:  /* CR后不是LF：如"GET / HTTP/1.1\rX" */
                return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 行结束符残缺(400) */
            }  /* 行尾字符分派结束 */
            break;  /* 状态处理完毕 */

        /* suppress warning */
        case sw_done:  /* 终态：仅为消除编译器对枚举未覆盖的告警，实际到不了这里 */
            break;  /* 空操作 */
        }  /* switch状态分派结束 */
    }  /* while逐字节循环结束：缓冲区耗尽或已达终态 */

    b->pos = p;  /* 回写读游标：告知事件框架已消费到哪，未到的字节留待下次可读事件——跨包续析的基础 */

    if (state == sw_done) {  /* 结局一：请求行完整收到(CRLF边界确认) */
        if (r->request_end == NULL) {  /* 未经sw_almost_done路径(如裸LF收尾)时终点尚未记录 */
            r->request_end = p - 1;  /* 补记请求行终点：在结束符之前 */
        }  /* 终点补记结束 */

        r->http_version = r->http_major * 1000 + r->http_minor;  /* 主次版本合成整数(1.1→1001)：便于数值比较 */
        r->state = sw_start;  /* 解析器复位：本函数使命完成，下次调用从sw_start解析头部行 */

        if (r->http_version == 9 && r->method != NGX_HTTP_GET) {  /* 0.9协议只有GET语义：无版本段却声明其它方法属伪装 */
            return NGX_HTTP_PARSE_INVALID_09_METHOD;  /* 0.9请求中的非法方法：上层回400 Bad Request */
        }  /* 0.9方法校验结束 */

        return NGX_OK;  /* 请求行解析成功：可开始逐行解析头部 */

    } else {  /* 结局二：字节流先断了——请求行只来了一部分(典型拆包) */
        r->state = state;  /* 把断点状态外置到请求：这是解析可恢复的关键——下个事件带着状态原地继续 */
        return NGX_AGAIN;  /* 通知上层数据不够，挂起等更多字节，而非报错 */
    }  /* 请求行结局判定结束 */
}  /* 请求行解析函数结束 */


/*
 * ngx_http_parse_header_line()：头部行逐字节状态机——每次调用解析一行
 * "Name: value"或判定头部块结束。与请求行解析同理：TCP不保留消息边界，
 * 一行头部可能分多次到达(拆包)，也可能多行连同请求体一起到达(粘包)，
 * 因此同样用state记录"行内进度"并外置到r->state，字节不够返回NGX_AGAIN
 * 挂起续析；一行完整返回NGX_OK(调用方循环调用来收多行)；见到头块结束的
 * CRLF返回NGX_HTTP_PARSE_HEADER_DONE。长度上限由调用方配合本函数防御
 * 超大头部攻击(见ngx_http_alloc_large_header_buffer)。
 */
ngx_int_t ngx_http_parse_header_line(ngx_http_request_t *r, ngx_buf_t *b)  /* r=请求(存行内断点与各段定位) b=收包缓冲 */
{  /* 函数体开始 */
    u_char  c, ch, *p;  /* c=小写化后的字符; ch=当前字符; p=读游标 */
    enum {  /* 头部行状态机的全部状态 */
        sw_start = 0,  /* 行首：判断是头名首字符还是头块结束的CRLF */
        sw_name,  /* 头名段：收合法名字符直到':' */
        sw_space_before_value,  /* ':'后的空格段 */
        sw_value,  /* 值段：直到空白或行结束 */
        sw_space_after_value,  /* 值后的空格段：可能值还没完(值内空格折返) */
        sw_almost_done,  /* 行已见CR只差LF */
        sw_header_almost_done,  /* 头块结束的CRLF已见CR只差LF */
        sw_ignore_line,  /* 忽略整行状态(如IIS残留的重复响应行) */
        sw_done,  /* 一行头部完成：while终止哨兵 */
        sw_header_done  /* 整个头部块完成 */
    } state;  /* 局部状态：退出前回存r->state */

    state = r->state;  /* 恢复断点：上一包可能只送到这行的半截 */
    p = b->pos;  /* 从未消费字节处继续：pos/last界定本次读到的区间 */

    while (p < b->last && state < sw_done) {  /* 还有字节且未达"行完成"级终态 */
        ch = *p++;  /* 逐字节消费：行语法完全由字符序列界定，不依赖整行连续在内存 */

        switch (state) {  /* 按当前状态分派字符 */

        /* first char */
        case sw_start:  /* 行首态：空行=头块结束；非空行=头名开始 */
            switch (ch) {  /* 对当前字符分派 */
            case CR:  /* 空行的CR：头部区到此结束(请求头与请求体以CRLF空行分界) */
                r->header_end = p - 1;  /* 记头块结束位置 */
                state = sw_header_almost_done;  /* 只差配对的LF */
                break;  /* 字符处理完 */
            case LF:  /* 宽容裸LF空行(不发CR的客户端) */
                r->header_end = p - 1;  /* 记头块结束位置 */
                state = sw_header_done;  /* 头块完成 */
                break;  /* 字符处理完 */
            default:  /* 非CR/LF：本行是"Name: value" */
                state = sw_name;  /* 迁入头名状态 */
                r->header_name_start = p - 1;  /* 记头名起点 */

                c = (u_char) (ch | 0x20);  /* 置低位bit实现小写化：一条或指令完成大小写归一 */
                if (c >= 'a' && c <= 'z') {  /* 字母(大小写均可)：头名首字符合法 */
                    break;  /* 留待sw_name继续 */
                }  /* 首字符字母校验结束 */

                if (ch == '-' || ch == '_' || ch == '~' || ch == '.') {  /* 连字符等：如"Content-Length"首字符虽罕见但允许 */
                    break;  /* 头名首字符合法 */
                }  /* 符号首字符校验结束 */

                if (ch >= '0' && ch <= '9') {  /* 数字开头：非标准但放行 */
                    break;  /* 头名首字符合法 */
                }  /* 数字首字符校验结束 */

                return NGX_HTTP_PARSE_INVALID_HEADER;  /* 首字符非法：畸形头部(上层回400) */

            }  /* 行首字符分派结束 */
            break;  /* 状态处理完毕 */

        /* header name */
        case sw_name:  /* 头名段：HTTP头名大小写不敏感，逐字符归一校验 */
            c = (u_char) (ch | 0x20);  /* 小写化：归一比较的基础 */
            if (c >= 'a' && c <= 'z') {  /* 字母：头名继续 */
                break;  /* 留在本状态 */
            }  /* 头名字母校验结束 */

            if (ch == ':') {  /* 冒号：头名与值的分界符——行内语法成分靠它切分 */
                r->header_name_end = p - 1;  /* 记头名终点 */
                state = sw_space_before_value;  /* 转入':'后空格段 */
                break;  /* 字符处理完 */
            }  /* 冒号定界分支结束 */

            if (ch == '-' || ch == '_' || ch == '~' || ch == '.') {  /* 允许的符号(兼容各类非标头名) */
                break;  /* 留在本状态 */
            }  /* 头名符号校验结束 */

            if (ch >= '0' && ch <= '9') {  /* 数字也允许出现在头名中 */
                break;  /* 留在本状态 */
            }  /* 头名数字校验结束 */

            /* IIS can send duplicate "HTTP/1.1 ..." lines */
            if (ch == '/'  /* 名字里出现'/'：疑似又一条"HTTP/1.1 ..."响应行 */
                && r->proxy  /* 且本机作代理：需容忍上游IIS的畸形重复状态行 */
                && p - r->header_start == 5  /* 行首5字符处遇'/'，即形如"HTTP/1.1" */
                && ngx_strncmp(r->header_start, "HTTP", 4) == 0)  /* 前4字节确为"HTTP"：确认是重复状态行 */
            {  /* 重复状态行判定成立 */
                state = sw_ignore_line;  /* 整行丢弃：代理场景对上游缺陷的兼容 */
                break;  /* 字符处理完 */
            }  /* IIS重复状态行处理结束 */

            return NGX_HTTP_PARSE_INVALID_HEADER;  /* 头名含非法字符：畸形请求(400) */

        /* space* before header value */
        case sw_space_before_value:  /* ':'后空格段：吞掉分隔空格 */
            switch (ch) {  /* 对当前字符分派 */
            case ' ':  /* 空格：RFC允许':'与值之间有空格 */
                break;  /* 继续吞 */
            case CR:  /* 空格后直接CR：该头有名无值(空值行) */
                r->header_start = r->header_end = p - 1;  /* 值起止都为空：指向同一处 */
                state = sw_almost_done;  /* 只差LF */
                break;  /* 字符处理完 */
            case LF:  /* 裸LF：空值行收尾 */
                r->header_start = r->header_end = p - 1;  /* 空值 */
                state = sw_done;  /* 本行完成 */
                break;  /* 字符处理完 */
            default:  /* 非 空 格/CR/LF：值开始了 */
                r->header_start = p - 1;  /* 记值起点(回退1：该字符属于值) */
                state = sw_value;  /* 迁入值段 */
                break;  /* 字符处理完 */
            }  /* 值前字符分派结束 */
            break;  /* 状态处理完毕 */

        /* header value */
        case sw_value:  /* 值段：值内部允许空格，遇到空白先悬置判定 */
            switch (ch) {  /* 对当前字符分派 */
            case ' ':  /* 值中空格：可能是值内词间空格，也可能是行尾填充 */
                r->header_end = p - 1;  /* 先暂记为值终点 */
                state = sw_space_after_value;  /* 交由下一状态裁决：再来字符则值未完 */
                break;  /* 字符处理完 */
            case CR:  /* 值以CR结束 */
                r->header_end = p - 1;  /* 记值终点(CR不算值内容) */
                state = sw_almost_done;  /* 只差LF */
                break;  /* 字符处理完 */
            case LF:  /* 值以裸LF结束 */
                r->header_end = p - 1;  /* 记值终点 */
                state = sw_done;  /* 本行完成 */
                break;  /* 字符处理完 */
            }  /* 其余字符(含冒号等)一律属于值，留本状态继续收 */
            break;  /* 状态处理完毕 */

        /* space* before end of header line */
        case sw_space_after_value:  /* 值后空格段：裁决"空格是值的一部分还是行尾填充" */
            switch (ch) {  /* 对当前字符分派 */
            case ' ':  /* 连续空格：继续视为行尾填充 */
                break;  /* 留在本状态 */
            case CR:  /* 空格后CR：本行确实结束 */
                state = sw_almost_done;  /* 只差LF */
                break;  /* 字符处理完 */
            case LF:  /* 裸LF：本行结束 */
                state = sw_done;  /* 本行完成 */
                break;  /* 字符处理完 */
            default:  /* 空格后又出字符：说明那是值内空格，值被撕裂了 */
                state = sw_value;  /* 折返值段继续收：值终点由后续CR/LF重新定界 */
                break;  /* 字符处理完 */
            }  /* 值后字符分派结束 */
            break;  /* 状态处理完毕 */

        /* ignore header line */
        case sw_ignore_line:  /* 整行忽略态：丢弃IIS重复状态行直到其行尾 */
            switch (ch) {  /* 对当前字符分派 */
            case LF:  /* 行尾LF：忽略完毕，回到行首迎接下一条真正的头部 */
                state = sw_start;  /* 复位到行首状态 */
                break;  /* 字符处理完 */
            default:  /* 行内其它字节：一律丢弃不校验 */
                break;  /* 留在本状态继续吞 */
            }  /* 忽略行字符分派结束 */
            break;  /* 状态处理完毕 */

        /* end of header line */
        case sw_almost_done:  /* 行已见CR只差LF：拆包断在CR/LF之间时状态就停这里 */
            switch (ch) {  /* 对当前字符分派 */
            case LF:  /* LF到位：CRLF齐，一行头部完成 */
                state = sw_done;  /* 本行完成 */
                break;  /* 字符处理完 */
            default:  /* CR后不是LF：行结束符畸形 */
                return NGX_HTTP_PARSE_INVALID_HEADER;  /* 畸形头部(400) */
            }  /* 行尾字符分派结束 */
            break;  /* 状态处理完毕 */

        /* end of header */
        case sw_header_almost_done:  /* 头块结束的CR已见只差LF */
            switch (ch) {  /* 对当前字符分派 */
            case LF:  /* LF到位：空行确认，请求头整体结束(其后即请求体) */
                state = sw_header_done;  /* 头块完成 */
                break;  /* 字符处理完 */
            default:  /* CR后非LF：畸形 */
                return NGX_HTTP_PARSE_INVALID_HEADER;  /* 畸形头部(400) */
            }  /* 头块行尾分派结束 */
            break;  /* 状态处理完毕 */

        /* suppress warning */
        case sw_done:  /* 行完成终态(哨兵，实际走不到) */
        case sw_header_done:  /* 头块完成终态(哨兵) */
            break;  /* 空操作 */
        }  /* 头部行状态分派结束 */
    }  /* while逐字节循环结束：缓冲区耗尽或已达终态 */

    b->pos = p;  /* 回写游标：未消费的字节(可能已粘着下一行甚至请求体)留给后续处理 */

    if (state == sw_done) {  /* 结局一：恰好完成一行头部 */
        r->state = sw_start;  /* 复位：下一次调用解析下一行 */
        return NGX_OK;  /* 交给调用方：登记本行后继续循环解析 */

    } else if (state == sw_header_done) {  /* 结局二：见到空行，整个头部块结束 */
        r->state = sw_start;  /* 复位解析器 */
        return NGX_HTTP_PARSE_HEADER_DONE;  /* 通知上层：可转入处理请求体/执行请求阶段 */

    } else {  /* 结局三：字节流中断，本行未完(拆包) */
        r->state = state;  /* 断点状态外置：下个可读事件从半行处继续 */
        return NGX_AGAIN;  /* 挂起等待更多数据而非报错 */
    }  /* 头部行结局判定结束 */
}  /* 头部行解析函数结束 */


/*
 * ngx_http_parse_complex_uri()：URI规范化状态机——安全理论的教科书案例。
 * 请求行解析时若发现complex_uri标记(含%XX、/./、//、/../)，须把URI原地
 * 规范化后才能用于文件映射，否则攻击者可用"/a/../.."或"/%2e%2e/"绕过
 * root/docroot约束造成目录穿越。本函数边解码%XX边按目标系统的路径语义
 * 折叠路径段：p为读游标，u为写游标(输出只可能更短，故可原地覆写)。
 * 注意它不像前两个函数那样返回NGX_AGAIN：调用时整个请求头已完整收妥
 * (头部以空行定界)，URI必然连续完整地在内存中，无需跨事件续析。
 */
ngx_int_t ngx_http_parse_complex_uri(ngx_http_request_t *r)  /* r=请求(URI已在缓冲内) */
{  /* 函数体开始 */
    u_char  c, ch, decoded, *p, *u;  /* c=小写化字符; ch=当前字符; decoded=%XX高半字节; p=读游标; u=写游标 */
    enum {  /* 规范化状态机状态 */
        sw_usual = 0,  /* 普通字符态 */
        sw_slash,  /* 刚写过'/'：用于吞并连续'//' */
        sw_dot,  /* '/'后一个'.'：可能是/./ */
        sw_dot_dot,  /* '/'后两个'.'：可能是/../(回退一级) */
#if (WIN32)  /* Windows路径语义："..."也按点段处理(兼容历史DOS 8.3名) */
        sw_dot_dot_dot,  /* '/'后三个'.'：视同".." */
#endif  /* WIN32分支结束 */
        sw_quoted,  /* 已见'%'：收转义高半字节 */
        sw_quoted_second  /* 已收高半字节：收低半字节并拼成原字节 */
    } state, quoted_state;  /* quoted_state记进入%XX解码前的状态，解完须回去 */

    decoded = '\0';  /* 高半字节缓存清零 */
    quoted_state = sw_usual;  /* 解码返回态默认为普通态 */

    state = sw_usual;  /* 从普通态起步(本状态机一次性跑完，无需断点外置) */
    p = r->uri_start;  /* 读游标指向请求行中URI的起点 */
    u = r->uri.data;  /* 写游标指向预分配的输出缓冲(长度<=原URI，可原地写) */
    r->uri_ext = NULL;  /* 扩展名指针先清空：规范化过程中重新定位 */

    ch = *p++;  /* 取第一个字符并前移读游标 */

    while (p < r->uri_start + r->uri.len + 1) {  /* 逐字符扫过整个URI(含结尾哨兵位) */

        ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,  /* 调试日志：打印状态机轨迹 */
                       "s:%d in:'%x:%c', out:'%c'", state, ch, ch, *u);  /* 当前状态/输入字符/输出位置 */

        switch (state) {  /* 按状态分派 */
        case sw_usual:  /* 普通字符态：原样复制，唯'/'、'%'、'.'需特殊处理 */
            switch(ch) {  /* 对当前字符分派 */
            case '/':  /* 路径分隔符：可能后面跟着/./或//序列 */
                r->uri_ext = NULL;  /* 跨过目录：此前记的扩展名作废 */
                state = sw_slash;  /* 转入"刚写过斜杠"状态盯下一个字符 */
                *u++ = ch;  /* 写出该'/' */
                break;  /* 字符处理完 */
            case '%':  /* %XX转义开始：必须解码，否则"%2e%2e/"可伪装成"../"穿透 */
                quoted_state = state;  /* 记住解码前状态：解完字节按此状态继续走图 */
                state = sw_quoted;  /* 转入解码状态收高半字节 */
                break;  /* 字符处理完(不写出'%') */
            case '.':  /* 点：可能是扩展名起点，也可能与前面'/'构成路径点段 */
                r->uri_ext = u + 1;  /* 记扩展名候选(输出缓冲中的位置) */
            default:  /* '.'与普通字符共用出口：'.'先记扩展名再原样写出(无break直落) */
                *u++ = ch;  /* 原样写出 */
                break;  /* 字符处理完 */
            }  /* 普通字符分派结束 */
            ch = *p++;  /* 预取下一字符：本状态机采用"读游标领先一格"的写法 */
            break;  /* 状态处理完毕 */

        case sw_slash:  /* 刚写出'/'：判别连续'//'与点段序列 */
            switch(ch) {  /* 对当前字符分派 */
            case '/':  /* 又一个'/'：空路径段，规范化时直接吞掉(//折叠为/) */
                break;  /* 不写输出，停留本状态继续盯 */
            case '.':  /* '/'后跟'.'：可能构成/./或/../，先原样暂写 */
                state = sw_dot;  /* 转入单点状态 */
                *u++ = ch;  /* 暂写出'.'：若后续确认是/./再回退删掉 */
                break;  /* 字符处理完 */
            case '%':  /* %XX：解码结果要按"刚出斜杠"的语义继续判定 */
                quoted_state = state;  /* 记返回态 */
                state = sw_quoted;  /* 转入解码 */
                break;  /* 字符处理完 */
            default:  /* 普通字符：前一段路径正常延续 */
                state = sw_usual;  /* 回普通态 */
                *u++ = ch;  /* 写出 */
                break;  /* 字符处理完 */
            }  /* 斜杠后字符分派结束 */
            ch = *p++;  /* 预取下一字符 */
            break;  /* 状态处理完毕 */

        case sw_dot:  /* '/'后已有一个'.'：判定是/./还是普通文件名 */
            switch(ch) {  /* 对当前字符分派 */
            case '/':  /* 又遇'/'：确认"/./"，折叠掉这个无意义点段 */
                state = sw_slash;  /* 回到斜杠态(该'/'已等价于上次的'/') */
                u--;  /* 回退写游标：抹掉暂写的'.'——折叠发生在输出侧 */
                break;  /* 不写出本次'/'：与已有的'/'合并 */
            case '.':  /* 再一个'.'：升级为/../候选，须回退上一级目录 */
                state = sw_dot_dot;  /* 转入双点状态 */
                *u++ = ch;  /* 暂写第二个'.'：等下一个'/'再整体回退 */
                break;  /* 字符处理完 */
            case '%':  /* %XX：解码后续按单点态语义继续 */
                quoted_state = state;  /* 记返回态 */
                state = sw_quoted;  /* 转入解码 */
                break;  /* 字符处理完 */
            default:  /* 其它字符：如"/a.b"的普通文件名带点 */
                state = sw_usual;  /* 回普通态 */
                *u++ = ch;  /* 写出(暂写的'.'就此转正) */
                break;  /* 字符处理完 */
            }  /* 单点段字符分派结束 */
            ch = *p++;  /* 预取下一字符 */
            break;  /* 状态处理完毕 */

        case sw_dot_dot:  /* '/'后已有".."：这是目录穿越攻击的经典载体，需折叠一级 */
            switch(ch) {  /* 对当前字符分派 */
            case '/':  /* '/'确认"/../"：删除上一级路径段 */
                state = sw_slash;  /* 折叠完成后等价于刚写过'/' */
                u -= 4;  /* 回退抹掉"/.."三字符并再退一格：交由下循环退到段首 */
                if (u < r->uri.data) {  /* 退无可退：已试图越过根目录 */
                    return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 拒绝(400)——穿越防御的硬边界 */
                }  /* 越界检查结束 */
                while (*(u - 1) != '/') {  /* 继续回退直到u-1指向'/'：把上一段剩余字符也抹掉 */
                    u--;  /* 逐字节回退 */
                }  /* 段首回退循环结束 */
                break;  /* 输出已回退到上一级目录处 */
            case '%':  /* %XX：可能解码出'.'等，解码后回本状态再判 */
                quoted_state = state;  /* 记返回态 */
                state = sw_quoted;  /* 转入解码 */
                break;  /* 字符处理完 */
#if (WIN32)  /* Windows分支："..."段在DOS语义下等价".." */
            case '.':  /* 第三个'.'：按三点的特殊路径段处理 */
                state = sw_dot_dot_dot;  /* 转入三点状态 */
                *u++ = ch;  /* 暂写第三个'.' */
                break;  /* 字符处理完 */
#endif  /* WIN32分支结束 */
            default:  /* 其它字符：如"/..x"只是普通名字 */
                state = sw_usual;  /* 回普通态 */
                *u++ = ch;  /* 写出(暂写的".."转正为文件名一部分) */
                break;  /* 字符处理完 */
            }  /* 双点段字符分派结束 */
            ch = *p++;  /* 预取下一字符 */
            break;  /* 状态处理完毕 */

#if (WIN32)  /* Windows专用路径折叠分支 */
        case sw_dot_dot_dot:  /* '/'后已有"..."：DOS历史语义下视同".."，须同样折叠 */
            switch(ch) {  /* 对当前字符分派 */
            case '/':  /* '/'确认"/.../"：按回退一级处理 */
                state = sw_slash;  /* 折叠完等价于刚写过'/' */
                u -= 5;  /* 回退抹掉"/..."四字符并再退一格 */
                if (u < r->uri.data) {  /* 越过根目录 */
                    return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 拒绝(400)：穿越防御 */
                }  /* 越界检查结束 */
                while (*u != '/') {  /* 先退到当前段首的'/'(Windows分支用了另一写法) */
                    u--;  /* 逐字节回退 */
                }  /* 段首回退循环结束 */
                if (u < r->uri.data) {  /* 再次校验未越界 */
                    return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 拒绝(400) */
                }  /* 二次越界检查结束 */
                while (*(u - 1) != '/') {  /* 再退到上一段末：把上一级段整体抹掉 */
                    u--;  /* 逐字节回退 */
                }  /* 上级段回退循环结束 */
                break;  /* 折叠完成 */
            case '%':  /* %XX：解码后回本状态再判 */
                quoted_state = state;  /* 记返回态 */
                state = sw_quoted;  /* 转入解码 */
                break;  /* 字符处理完 */
            default:  /* 其它字符："/..x"类普通名字 */
                state = sw_usual;  /* 回普通态 */
                *u++ = ch;  /* 写出 */
                break;  /* 字符处理完 */
            }  /* 三点段字符分派结束 */
            ch = *p++;  /* 预取下一字符 */
            break;  /* 状态处理完毕 */
#endif  /* WIN32专用分支结束 */

        case sw_quoted:  /* %XX解码态：收高半字节(hex第一位) */
            if (ch >= '0' && ch <= '9') {  /* 数字：值=字符-'0' */
                decoded = (u_char) (ch - '0');  /* 缓存高半字节 */
                state = sw_quoted_second;  /* 转收低半字节 */
                ch = *p++;  /* 预取下一字符 */
                break;  /* 字符处理完 */
            }  /* 数字半字节分支结束 */

            c = (u_char) (ch | 0x20);  /* 小写化：统一比较 */
            if (c >= 'a' && c <= 'f') {  /* 十六进制字母a-f */
                decoded = (u_char) (c - 'a' + 10);  /* 高半字节=10..15 */
                state = sw_quoted_second;  /* 转收低半字节 */
                ch = *p++;  /* 预取 */
                break;  /* 字符处理完 */
            }  /* 字母半字节分支结束 */

            return NGX_HTTP_PARSE_INVALID_REQUEST;  /* %后非hex字符：畸形转义(400) */

        case sw_quoted_second:  /* 解码收尾态：拼出原始字节并"重放"进状态机 */
            if (ch >= '0' && ch <= '9') {  /* 低半字节为数字 */
                ch = (u_char) ((decoded << 4) + ch - '0');  /* 高低半字节合成原字节：ch被原地替换为解码结果 */
                if (ch == '%') {  /* 解出字面'%'(如%25)：只当普通字符，绝不再解一遍 */
                    state = sw_usual;  /* 回普通态：二次解码会让"%252e%252e"伪装成"../"绕过折叠 */
                    *u++ = ch;  /* 写出字面'%' */
                    ch = *p++;  /* 预取 */
                    break;  /* 字符处理完 */
                }  /* 字面%分支结束 */
                state = quoted_state;  /* 回到进入%XX前的状态：解码字节按原语境重新判定(可触发/./折叠) */
                break;  /* 用解码后的ch走switch——这是"解码后重放"防绕过的关键 */
            }  /* 数字半字节分支结束 */

            c = (u_char) (ch | 0x20);  /* 小写化 */
            if (c >= 'a' && c <= 'f') {  /* 低半字节为hex字母 */
                ch = (u_char) ((decoded << 4) + c - 'a' + 10);  /* 合成原字节 */
                if (ch == '%') {  /* 解出字面'%'：同上，不二次解码 */
                    state = sw_usual;  /* 回普通态 */
                    *u++ = ch;  /* 写出'%' */
                    ch = *p++;  /* 预取 */
                    break;  /* 字符处理完 */
                }  /* 字面%分支结束 */
                state = quoted_state;  /* 回解码前状态，重放该字节 */
                break;  /* 字符处理完 */
            }  /* 字母半字节分支结束 */

            return NGX_HTTP_PARSE_INVALID_REQUEST;  /* 第二位非hex：畸形%XX(400) */
        }  /* 规范化状态分派结束 */
    }  /* URI扫描循环结束：全部字节消费完毕 */

    r->uri.len = u - r->uri.data;  /* 规范化后URI变短：按写游标回填真实长度 */
    r->uri.data[r->uri.len] = '\0';  /* 补C串结尾符：便于日志与C库调用(路径折叠縮短了串) */

    if (r->uri_ext) {  /* 存在扩展名：如"/a.html"的"html" */
        r->exten.len = u - r->uri_ext;  /* 扩展名终点即规范化后URI末尾：按指针差求长 */

        if (!(r->exten.data = ngx_palloc(r->pool, r->exten.len + 1))) {  /* 从连接内存池分配扩展名存储(+1留给'\0') */
            return NGX_HTTP_INTERNAL_SERVER_ERROR;  /* 分配失败：500 */
        }  /* 分配失败分支结束 */

        ngx_cpystrn(r->exten.data, r->uri_ext, r->exten.len + 1);  /* 拷贝扩展名并补结尾符 */
    }  /* 扩展名提取结束 */

    r->uri_ext = NULL;  /* 清指针：避免悬挂指向旧缓冲 */

    return NGX_OK;  /* 规范化成功 */
}  /* URI规范化函数结束 */
