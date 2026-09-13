
/*
 * Copyright (C) Igor Sysoev
 */



#include <ngx_config.h>    /* 平台配置头: 定义各OS特性宏与基础类型 */
#include <ngx_core.h>      /* nginx核心头: 字符串/内存池等核心API声明 */


/*
 * 整体功能: 把一个0~255的字节按十进制写入text, 返回写入的位数(不含'\0')。
 * 网络字节序=大端序, sin_addr的4个字节按内存顺序恰好对应点分十进制从高到低的
 * 四段, 因此逐字节十进制化再以'.'连接即可。这里手工做除法拆位而不用sprintf,
 * 省去格式串解析开销——IP转换位于accept新连接的热路径上, 性能敏感。
 */
ngx_inline static size_t ngx_sprint_uchar(u_char *text, u_char c, size_t len)    /* 输出位置/待转字节/剩余空间 */
{                          /* 函数体开始 */
    size_t      n;         /* 已写入的字符数 */
    ngx_uint_t  c1, c2;    /* c1=百位, c2=十位/个位 */

    n = 0;                 /* 初始尚未写入任何字符 */

    if (len == n) {        /* 输出空间已用完 */
        return n;          /* 返回0表示什么都没写 */
    }   /* if结束 */

    c1 = c / 100;          /* 取百位数字(0/1/2) */

    if (c1) {              /* 百位非0才输出, 避免前导零(如"007") */
        *text++ = (u_char) (c1 + '0');    /* 数字+'0'转ASCII写入, 指针前移 */
        n++;                              /* 写入计数加1 */

        if (len == n) {    /* 空间恰好用尽 */
            return n;      /* 提前返回, 由调用方负责补'\0' */
        }   /* if结束 */
    }   /* if结束 */

    c2 = (c % 100) / 10;   /* 去掉百位后取十位 */

    if (c1 || c2) {        /* 已输出百位或十位非0才输出十位 */
        *text++ = (u_char) (c2 + '0');    /* 写入十位ASCII字符 */
        n++;                              /* 计数加1 */

        if (len == n) {    /* 空间用尽 */
            return n;      /* 提前返回 */
        }   /* if结束 */
    }   /* if结束 */

    c2 = c % 10;           /* 取个位 */

    *text++ = (u_char) (c2 + '0');    /* 个位必定输出(保证至少写1位) */
    n++;                              /* 计数加1 */

    return n;              /* 返回本次写入的位数(1~3) */
}   /* ngx_sprint_uchar结束 */


/* AF_INET only */         /* (原有注释)本文件仅支持IPv4 */

/*
 * 整体功能: 内核二进制sockaddr → 点分十进制文本。从sockaddr_in(sin_family/
 * sin_port/sin_addr)中取出sin_addr——网络字节序即大端序的4个字节, 依次按
 * "高字节.次高.次低.低字节"十进制化输出; 缓冲区不足时截断并保证'\0'结尾。
 */
size_t ngx_sock_ntop(int family, struct sockaddr *addr, u_char *text,    /* 地址族/通用sockaddr/输出缓冲 */
                     size_t len)    /* 缓冲区长度 */
{                          /* 函数体开始 */
    u_char              *p;        /* 指向sin_addr的4字节首地址 */
    size_t               n;        /* 已写入的字符数 */
    ngx_uint_t           i;        /* 当前处理的字节下标(0~3) */
    struct sockaddr_in  *addr_in;  /* IPv4专用地址结构 */

    if (len == 0) {        /* 缓冲区长度为0 */
        return 0;          /* 无处可写, 返回失败 */
    }   /* if结束 */

    if (family != AF_INET) {    /* 地址族不是IPv4 */
        return 0;               /* 本函数不支持, 返回失败 */
    }   /* if结束 */

    addr_in = (struct sockaddr_in *) addr;    /* 通用sockaddr强转为sockaddr_in */
    p = (u_char *) &addr_in->sin_addr;        /* 大端序: p[0]是IP最高字节, p[3]是最低字节 */

    if (len > INET_ADDRSTRLEN) {    /* 超过标准最大长度(15+'\0'=16) */
        len = INET_ADDRSTRLEN;      /* 钳制到16, 防止缓冲区越界 */
    }   /* if结束 */

    n = ngx_sprint_uchar(text, p[0], len);    /* 输出第一段: 最高字节(如"192") */

    i = 1;                 /* 下一个待输出字节下标 */

    do {                   /* 循环输出剩余3段 */
        if (len == n) {    /* 缓冲区用尽 */
            text[n - 1] = '\0';    /* 把最后一位替换为'\0'截断 */
            return n;              /* 返回已写长度 */
        }   /* if结束 */

        text[n++] = '.';   /* 写段间分隔符'.' */

        if (len == n) {    /* 写完'.'后空间用尽 */
            text[n - 1] = '\0';    /* 撤掉'.'换成'\0'截断 */
            return n;              /* 返回已写长度 */
        }   /* if结束 */

        n += ngx_sprint_uchar(&text[n], p[i++], len - n);    /* 输出下一段, 剩余空间=len-n */

    } while (i < 4);   /* 共4个字节, 循环3次 */

    if (len == n) {    /* 空间用尽(防御分支) */
        text[n] = '\0';    /* 补'\0'后返回 */
        return n;          /* 返回已写长度 */
    }   /* if结束 */

    text[n] = '\0';    /* 补字符串结束符 */

    return n;          /* 返回文本长度(不含'\0') */

#if 0                 /* 被禁用的备用实现 */
    return ngx_snprintf((char *) text,    /* 用格式化输出的等价写法 */
                        len > INET_ADDRSTRLEN ? INET_ADDRSTRLEN : len,    /* 长度钳制 */
                        "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);    /* 4字节按点分十进制输出 */
#endif                /* 禁用代码块结束 */
}   /* ngx_sock_ntop结束 */


/*
 * 整体功能: 与ngx_sock_ntop同样做"二进制IP→点分十进制"转换, 区别在于不经
 * sockaddr_in, 直接以void*接收4字节地址(大端序), 供配置阶段(如listen的
 * "address:port"解析)把二进制地址还原成可读文本使用。
 */
size_t ngx_inet_ntop(int family, void *addr, u_char *text, size_t len)    /* 地址族/4字节IP/输出缓冲/长度 */
{                          /* 函数体开始 */
    u_char      *p;        /* 指向IP的4个字节(大端序) */
    size_t       n;        /* 已写入字符数 */
    ngx_uint_t   i;        /* 字节下标 */

    if (len == 0) {        /* 无输出空间 */
        return 0;          /* 返回失败 */
    }   /* if结束 */

    if (family != AF_INET) {    /* 仅支持IPv4地址族 */
        return 0;               /* 返回失败 */
    }   /* if结束 */

    p = (u_char *) addr;   /* 取地址首字节: 大端序下p[0]为最高位字节 */

    if (len > INET_ADDRSTRLEN) {    /* 长度超过标准最大值 */
        len = INET_ADDRSTRLEN;      /* 钳制到16字节 */
    }   /* if结束 */

    n = ngx_sprint_uchar(text, p[0], len);    /* 输出第一段: 最高字节 */

    i = 1;                 /* 下一段字节下标 */

    do {                   /* 循环输出剩余3段 */
        if (len == n) {    /* 空间用尽 */
            text[n - 1] = '\0';    /* 截断补'\0' */
            return n;              /* 返回已写长度 */
        }   /* if结束 */

        text[n++] = '.';   /* 写'.'分隔符 */

        if (len == n) {    /* 空间用尽 */
            text[n - 1] = '\0';    /* 截断补'\0' */
            return n;              /* 返回已写长度 */
        }   /* if结束 */

        n += ngx_sprint_uchar(&text[n], p[i++], len - n);    /* 输出下一段 */

    } while (i < 4);   /* 4字节共循环3次 */

    if (len == n) {    /* 空间用尽(防御分支) */
        text[n] = '\0';    /* 补'\0'后返回 */
        return n;          /* 返回已写长度 */
    }   /* if结束 */

    text[n] = '\0';    /* 补字符串结束符 */

    return n;          /* 返回文本长度 */

#if 0                 /* 被禁用的备用实现 */
    return ngx_snprintf((char *) text,    /* 格式化输出的等价实现 */
                        len > INET_ADDRSTRLEN ? INET_ADDRSTRLEN : len,    /* 长度钳制 */
                        "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);    /* 点分十进制输出 */
#endif                /* 禁用代码块结束 */
}   /* ngx_inet_ntop结束 */


/* AF_INET only */         /* (原有注释)仅支持IPv4 */

/*
 * 整体功能: 解析"a.b.c.d/掩码位数"形式的CIDR文本为ngx_inet_cidr_t{addr,mask}。
 * 地址部分用inet_addr()转成网络字节序(大端)的32位整数; 掩码用0-(1<<(32-m))
 * 构造出高m位为1、低32-m位为0的值, 再htonl()转网络序, 供allow/deny等
 * 指令做整段网段的匹配。
 */
ngx_int_t ngx_ptocidr(ngx_str_t *text, void *cidr)    /* 入参: CIDR文本/输出结构 */
{                          /* 函数体开始 */
    ngx_int_t         m;           /* 掩码位长(0~32) */
    ngx_uint_t        i;           /* 扫描下标 */
    ngx_inet_cidr_t  *in_cidr;     /* 输出: 网络地址+掩码 */

    in_cidr = cidr;        /* void*还原为ngx_inet_cidr_t* */

    for (i = 0; i < text->len; i++) {    /* 从头扫描文本 */
        if (text->data[i] == '/') {      /* 找地址与掩码的分隔符'/' */
            break;       /* 找到即停, i停在'/'位置 */
        }   /* if结束 */
    }   /* for结束 */

    if (i == text->len) {    /* 扫描到末尾都没找到'/' */
        return NGX_ERROR;    /* 缺少掩码部分, 格式非法 */
    }   /* if结束 */

    text->data[i] = '\0';    /* 临时把'/'改成'\0', 截出纯IP子串 */
    in_cidr->addr = inet_addr((char *) text->data);    /* libc解析点分十进制→网络序32位整数 */
    text->data[i] = '/';     /* 恢复'/'(nginx字符串按长度管理, 不可破坏原文) */
    if (in_cidr->addr == INADDR_NONE) {    /* inet_addr失败返回INADDR_NONE(全1) */
        return NGX_ERROR;    /* 地址部分非法 */
    }   /* if结束 */

    m = ngx_atoi(&text->data[i + 1], text->len - (i + 1));    /* 解析'/'之后的掩码位数 */
    if (m == NGX_ERROR) {    /* 不是合法十进制数 */
        return NGX_ERROR;    /* 掩码部分非法 */
    }   /* if结束 */

    if (m == 0) {          /* 掩码0位: 匹配任意地址 */

        /* the x86 compilers use the shl instruction that shifts by modulo 32 */    /* (原有注释)x86移位按模32执行 */

        in_cidr->mask = 0;    /* 特判: 1<<32在x86上等同1<<0而非0, 掩码只能直接置0 */
        return NGX_OK;        /* 解析成功 */
    }   /* if结束 */

    in_cidr->mask = htonl((ngx_uint_t) (0 - (1 << (32 - m))));    /* 0借位得低(32-m)位全1→高m位为1的掩码, htonl转网络序 */

    return NGX_OK;         /* 解析成功 */
}   /* ngx_ptocidr结束 */


#if 0                 /* 被禁用的历史代码: listen指令"address:port"解析的早期实现 */

ngx_int_t ngx_inet_addr_port(ngx_conf_t *cf, ngx_command_t *cmd,    /* 配置上下文/当前指令 */
                             ngx_str_t *addr_port)    /* 待解析的"address:port"文本 */
{                          /* 函数体开始 */
    u_char          *host;    /* 主机名部分 */
    ngx_int_t        port;    /* 解析出的端口号 */
    ngx_uint_t       p;       /* 冒号所在下标 */
    struct hostent  *h;       /* 域名解析结果(本版未使用) */

    for (p = 0; p < addr_port->len; p++) {    /* 扫描文本 */
        if (addr_port->data[p] == ':') {    /* 找host与port的分隔符':' */
            break;    /* 找到即停 */
        }   /* if结束 */
    }   /* for结束 */

    in_addr->host.len = p;    /* 冒号前即host部分(引用了未定义变量, 死代码) */
    if (!(in_addr->host.data = ngx_palloc(pool, p + 1))) {    /* 为host分配缓冲(p+1含'\0') */
        return NGX_ERROR;    /* 分配失败 */
    }   /* if结束 */

    ngx_cpystrn(in_addr->host.data, addr_port->data, p + 1);    /* 拷贝host子串并保证'\0'结尾 */

    if (p == addr_port->len) {    /* 全串无冒号: 未指定端口 */
        p = 0;    /* 下标归零(从头解析端口) */
    }   /* if结束 */

    port = ngx_atoi(&addr[p], args[1].len - p);    /* 解析端口数字 */
    if (port == NGX_ERROR && p == 0) {    /* 无冒号且解析不到数字: 端口缺省 */

        /* default port */    /* (原有注释)缺省端口 */

        iap->port = 0;    /* 端口置0表示未指定 */

    } else if ((port == NGX_ERROR && p != 0) /* "listen host:NONNUMBER" */    /* (原有注释)有冒号但端口非数字 */
               || (port < 1 || port > 65536)) { /* "listen 99999" */    /* (原有注释)端口超出1~65535合法范围 */

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,    /* 配置期EMERG级错误 */
                           "invalid port \"%s\" in \"%s\" directive, "    /* 错误文案前半 */
                           "it must be a number between 1 and 65535",    /* 错误文案后半 */
                           &addr[p], cmd->name.data);    /* 参数: 端口文本/指令名 */

        return NGX_CONF_ERROR;    /* 配置解析失败 */

    } else if (p == 0) {    /* 只写了端口没写地址 */
        ls->addr = INADDR_ANY;    /* 监听全部本机地址(0.0.0.0) */
        ls->port = (in_port_t) port;    /* 保存端口号 */
        return NGX_CONF_OK;    /* 解析成功 */
    }   /* if-else链结束 */

    return NGX_OK;    /* 返回成功 */
}   /* ngx_inet_addr_port结束 */

#endif                /* 禁用代码块结束 */
