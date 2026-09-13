
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置总头：基础类型与编译期特性 */
#include <ngx_core.h>  /* 核心设施：ngx_output_chain输出链、ngx_bufs_t、内存池 */
#include <ngx_http.h>  /* HTTP框架接口：请求结构r、body过滤链类型与宏 */


/* copy filter的location级配置结构：存放output_buffers指令的值 */
typedef struct {
    ngx_bufs_t  bufs;  /* 输出缓冲参数：buf个数与单个buf大小 */
} ngx_http_copy_filter_conf_t;  /* 类型名 */


/* 前置声明：解析配置时创建本模块location级配置 */
static void *ngx_http_copy_filter_create_conf(ngx_conf_t *cf);
/* 前置声明：父子配置合并(http/server/location继承链) */
static char *ngx_http_copy_filter_merge_conf(ngx_conf_t *cf,
                                             void *parent, void *child);  /* 参数：父/子配置指针 */
/* 前置声明：模块初始化时把自己挂上输出过滤链 */
static ngx_int_t ngx_http_copy_filter_init(ngx_cycle_t *cycle);


/* 指令表：本模块唯一的output_buffers指令 */
static ngx_command_t  ngx_http_copy_filter_commands[] = {

    {ngx_string("output_buffers"),  /* 指令名：控制响应体经内存中转时的缓冲个数/大小 */
     NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,  /* 作用域http/server/location，带2参数 */
     ngx_conf_set_bufs_slot,  /* 解析函数：把"num size"解析进ngx_bufs_t */
     NGX_HTTP_LOC_CONF_OFFSET,  /* 值存于location级配置结构 */
     offsetof(ngx_http_copy_filter_conf_t, bufs),  /* offsetof定位bufs字段：免硬编码结构偏移 */
     NULL},  /* 无后处理回调 */

    ngx_null_command  /* 指令表结束哨兵 */
};  /* 指令表结束 */


/* 模块上下文：8个生命周期钩子槽位，本模块只用location两级配置 */
static ngx_http_module_t  ngx_http_copy_filter_module_ctx = {
    NULL,                                  /* pre conf */  /* 预配置钩子：未使用 */

    NULL,                                  /* create main configuration */  /* 无main级配置 */
    NULL,                                  /* init main configuration */  /* 无main级初始化 */

    NULL,                                  /* create server configuration */  /* 无server级配置 */
    NULL,                                  /* merge server configuration */  /* 无server级合并 */

    ngx_http_copy_filter_create_conf,      /* create location configuration */  /* 创建location配置 */
    ngx_http_copy_filter_merge_conf        /* merge location configuration */  /* 合并location配置 */
};  /* 模块上下文结束 */


/* 模块定义：nginx模块模型的静态描述(版本/上下文/指令表/类型/钩子) */
ngx_module_t  ngx_http_copy_filter_module = {
    NGX_MODULE,  /* 通用宏：填充版本号等公共字段 */
    &ngx_http_copy_filter_module_ctx,      /* module context */  /* 指向模块上下文 */
    ngx_http_copy_filter_commands,         /* module directives */  /* 指令表 */
    NGX_HTTP_MODULE,                       /* module type */  /* HTTP类型模块 */
    ngx_http_copy_filter_init,             /* init module */  /* 初始化钩子：挂过滤链 */
    NULL                                   /* init process */  /* 无进程级初始化 */
};  /* 模块定义结束 */


/* 责任链的"下一个"body过滤器指针：本filter处理完把数据交给它 */
static ngx_http_output_body_filter_pt    ngx_http_next_filter;


/*
 * ngx_http_copy_filter()：输出过滤链(copy filter)入口。
 * 责任链模式：响应体依次流经各过滤器，本过滤器负责"内存/文件"形态适配——
 * sendfile开启时文件buf可原样透传(零拷贝，数据不过用户态)；sendfile关闭
 * 或下游filter(SSL/gzip)需要内存数据时，由ngx_output_chain按output_buffers
 * 把文件数据拷进内存buf再后传。
 */
ngx_int_t ngx_http_copy_filter(ngx_http_request_t *r, ngx_chain_t *in)  /* r:当前请求 in:上游filter传来的缓冲链 */
{
    ngx_output_chain_ctx_t       *ctx;  /* ngx_output_chain的运行上下文 */
    ngx_http_copy_filter_conf_t  *conf;  /* 本模块配置：缓冲个数与大小 */

    if (r->connection->write->error) {  /* 写方向已出错(对端RST/发送失败) */
        return NGX_ERROR;  /* 快速失败：不再做无谓的过滤处理 */
    }  /* 错误检查结束 */

    ctx = ngx_http_get_module_ctx(r->main ? r->main : r,  /* 取模块ctx：子请求共享主请求的上下文 */
                                            ngx_http_copy_filter_module);  /* 模块标识 */

    if (ctx == NULL) {  /* 首次进入：惰性创建上下文(每请求只做一次) */
        conf = ngx_http_get_module_loc_conf(r->main ? r->main : r,  /* 同样以主请求为准取location配置 */
                                            ngx_http_copy_filter_module);  /* 模块标识 */

        ngx_http_create_ctx(r, ctx, ngx_http_copy_filter_module,  /* 从请求池分配ctx并登记(按需分配思想) */
                            sizeof(ngx_output_chain_ctx_t), NGX_ERROR);  /* 失败返回NGX_ERROR */

        ctx->sendfile = r->sendfile;  /* 记录sendfile开关：决定文件buf是透传还是拷入内存 */
        ctx->need_in_memory = r->filter_need_in_memory;  /* 下游filter要求内存数据时置位：强制拷贝(如SSL加密需读明文) */
        ctx->need_in_temp = r->filter_need_temporary;  /* 下游要求可写临时buf时置位：需原地改写数据 */

        ctx->pool = r->pool;  /* 拷贝用内存在请求池分配：请求结束统一释放，防泄漏 */
        ctx->bufs = conf->bufs;  /* 缓冲参数：output_buffers的num/size */
        ctx->tag = (ngx_buf_tag_t) &ngx_http_copy_filter_module;  /* 分配者标签：free时防误释放他模块的buf */

        ctx->output_filter = (ngx_output_chain_filter_pt) ngx_http_next_filter;  /* 拷贝后调用链上下一个body filter */
        ctx->filter_ctx = r;  /* 传给下一过滤器的上下文：请求对象 */

    }  /* ctx惰性初始化结束 */

    return ngx_output_chain(ctx, in);  /* 交给通用输出链：按need_in_memory/sendfile决定拷贝或透传 */
}  /* ngx_http_copy_filter()结束 */


/* 创建location级配置：框架对每个location调用一次 */
static void *ngx_http_copy_filter_create_conf(ngx_conf_t *cf)
{
    ngx_http_copy_filter_conf_t *conf;  /* 新配置结构指针 */

    ngx_test_null(conf,  /* 从配置池分配并清零，失败返回NULL终止启动 */
                  ngx_palloc(cf->pool, sizeof(ngx_http_copy_filter_conf_t)),  /* 分配调用 */
                  NULL);  /* 失败返回值 */

    conf->bufs.num = 0;  /* 显式置0作"未配置"标记：供合并阶段识别 */

    return conf;  /* 交回框架保存，随后被merge */
}  /* create_conf结束 */


/* 合并父子配置：location未配置时继承server/http层的值(配置继承模型) */
static char *ngx_http_copy_filter_merge_conf(ngx_conf_t *cf,
                                             void *parent, void *child)
{
    ngx_http_copy_filter_conf_t *prev = parent;  /* 上一层(server/http)配置 */
    ngx_http_copy_filter_conf_t *conf = child;  /* 当前层(location)配置 */

    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs, 1, 32768);  /* 未配置则继承父级，最终默认1个32KB buf */

    return NULL;  /* NULL表示合并无错误 */
}  /* merge_conf结束 */


/* 模块初始化：把自己插到输出过滤链头，形成责任链(后注册者在链头先执行) */
static ngx_int_t ngx_http_copy_filter_init(ngx_cycle_t *cycle)
{
    ngx_http_next_filter = ngx_http_top_body_filter;  /* 保存原链头为"下一个"后继 */
    ngx_http_top_body_filter = ngx_http_copy_filter;  /* 自己成为新链头：各filter依次入栈构成链 */

    return NGX_OK;  /* 初始化成功 */
}  /* ngx_http_copy_filter_init()结束 */

