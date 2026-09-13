
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置总头：编译期探测宏、平台相关类型与函数映射 */
#include <ngx_core.h>    /* 核心头：内存池/数组/字符串/监听对象等基础设施 */
#include <ngx_event.h>   /* 事件头：事件驱动框架，accept出的新连接靠读事件进入HTTP层 */
#include <ngx_http.h>    /* HTTP框架总头：模块结构/三层配置上下文/phase与虚拟主机定义 */


static char *ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);  /* 前置声明：http{}块指令回调，HTTP框架初始化总入口 */
static char *ngx_http_merge_locations(ngx_conf_t *cf,  /* 前置声明：沿location树递归合并各级loc_conf */
                                      ngx_array_t *locations,  /* 续行：本级location数组(含嵌套location) */
                                      void **loc_conf,  /* 续行：父级loc_conf指针数组，作为合并默认值来源 */
                                      ngx_http_module_t *module,  /* 续行：当前模块钩子表(取merge_loc_conf) */
                                      ngx_uint_t ctx_index);  /* 续行：模块槽位下标，定位conf数组 */

int         ngx_http_max_module;  /* HTTP模块总数：各模块ctx_index的上界，三层conf数组按它定长 */

ngx_uint_t  ngx_http_total_requests;  /* 全局统计：框架累计处理的HTTP请求数 */
uint64_t    ngx_http_total_sent;  /* 全局统计：框架累计发送给客户端的响应字节数 */


ngx_int_t  (*ngx_http_top_header_filter) (ngx_http_request_t *r);  /* 响应头过滤链顶端指针：责任链入口，各过滤模块init时把旧顶端存为自己的next再自居顶端 */
ngx_int_t  (*ngx_http_top_body_filter) (ngx_http_request_t *r, ngx_chain_t *ch);  /* 响应体过滤链顶端指针：body经各过滤模块(如分块/压缩)链式加工后写出 */


static ngx_command_t  ngx_http_commands[] = {  /* 本模块指令表：仅声明http{}这一个块级指令 */

    {ngx_string("http"),  /* 指令名：配置文件顶层的http{}块 */
     NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,  /* 属性：主配置级+块结构+不带参数 */
     ngx_http_block,  /* 解析回调：读入http{}并完成HTTP框架全部初始化 */
     0,  /* conf偏移：取conf_ctx中本模块槽位地址，http块上下文ctx将写入该槽 */
     0,  /* offset：未用，回调自行管理配置结构 */
     NULL},  /* post：无附加解析数据 */

    ngx_null_command  /* 指令表结束哨兵 */
};  /* 指令表结束 */


    
static ngx_core_module_t  ngx_http_module_ctx = {  /* 模块上下文：http是core型模块，不参与HTTP模块编号 */
    ngx_string("http"),  /* 模块名"http" */
    NULL,  /* create_conf钩子：http块无core级配置结构(其ctx由ngx_http_block自建) */
    NULL  /* init_conf钩子：无需对配置做初始化校验 */
};  /* ngx_http_module_ctx 模块上下文结束 */


ngx_module_t  ngx_http_module = {  /* 模块描述符：编译进ngx_modules[]，配置解析器据此识别http{}指令 */
    NGX_MODULE,  /* 模块版本/签名宏 */
    &ngx_http_module_ctx,                  /* module context */  /* 指向上面的core模块上下文 */
    ngx_http_commands,                     /* module directives */  /* 指令表：声明http{}块 */
    NGX_CORE_MODULE,                       /* module type */  /* 类型：core模块(而非HTTP模块) */
    NULL,                                  /* init module */  /* 无模块初始化钩子：HTTP初始化集中在ngx_http_block */
    NULL                                   /* init child */  /* 无子进程初始化钩子 */
};  /* 模块描述结束 */


/*
 * ngx_http_block整体说明：
 * http{}块的解析回调，HTTP框架的总初始化函数，分四步：
 * 1) 分配http块三层配置上下文(main_conf/srv_conf/loc_conf)，为每个HTTP模块
 *    编号(ctx_index)并回调其create_main_conf/create_srv_conf/create_loc_conf；
 * 2) 递归解析http{}内的server/location指令，再按 模块×server 全组合执行
 *    init_main_conf/merge_srv_conf/merge_loc_conf，实现 main→server→location
 *    三层继承(低层显式配置覆盖高层默认值)；
 * 3) 搭建phase处理阶段引擎：rewrite/find_config/access/content各阶段的
 *    handler数组，把请求处理流程抽象为有序阶段链，模块在各阶段注册handler；
 * 4) 把所有server{}的listen指令归并为 端口→地址→server_name 三层查找表，
 *    为每个地址生成ngx_listening_t(内核层socket/bind/listen)，并将
 *    ls->handler指向ngx_http_init_connection——accept出新连接后进入HTTP层。
 * 理论要点：同一监听端口上的并发连接由TCP五元组(源IP,源端口,目的IP,目的端口,
 * 协议)唯一区分，故多个server{}可安全共用一个监听端口；而同一IP:端口上的
 * 多个域名则由HTTP的Host请求头区分，即"端口定进程、Host定虚拟主机"两级分路。
 */
static char *ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)  /* conf：conf_ctx中http模块槽位地址，块上下文写回此处 */
{  /* 函数体开始 */
    char                        *rv;  /* 各钩子/解析器的返回值：NGX_CONF_OK或错误串 */
    ngx_uint_t                   mi, m, s, l, p, a, n;  /* 循环下标：mi/m模块，s服务器，l监听，p端口，a地址，n服务器名 */
    ngx_uint_t                   port_found, addr_found, virtual_names;  /* 标志：端口/地址是否已收录；是否为真虚拟主机 */
    ngx_conf_t                   pcf;  /* 外层解析现场快照：http{}解析完恢复，避免污染main级解析状态 */
    ngx_array_t                  in_ports;  /* 监听端口数组：port→addr→server_name三层查找表 */
    ngx_listening_t             *ls;  /* 全局监听对象：描述socket/bind/listen参数，事件模块据此创建监听套接字 */
    ngx_http_listen_t           *lscf;  /* server{}内listen指令的配置数组 */
    ngx_http_module_t           *module;  /* 当前HTTP模块的方法表(create/merge/init钩子) */
    ngx_http_handler_pt         *h;  /* 向phase的handler数组push元素用的暂存指针 */
    ngx_http_conf_ctx_t         *ctx;  /* http{}块上下文：main_conf/srv_conf/loc_conf三个指针数组 */
    ngx_http_in_port_t          *in_port, *inport;  /* 端口节点：in_port遍历用，inport拆分构造用 */
    ngx_http_in_addr_t          *in_addr, *inaddr;  /* 地址节点：in_addr遍历用，inaddr新建插入用 */
    ngx_http_server_name_t      *s_name, *name;  /* 服务器名节点：s_name源server块中，name写入查找表中 */
    ngx_http_core_srv_conf_t   **cscfp, *cscf;  /* server级core配置：cscfp遍历servers数组，cscf取单个server */
    ngx_http_core_loc_conf_t    *clcf;  /* location级core配置：借它取server的错误日志 */
    ngx_http_core_main_conf_t   *cmcf;  /* http main级core配置：含servers数组与phase引擎 */
#if (WIN32)  /* Windows平台的IOCP特有处理 */
    ngx_iocp_conf_t             *iocpcf;  /* IOCP事件模型配置 */
#endif  /* WIN32条件编译结束 */

    /* the main http context */  /* 创建http{}块的主上下文 */
    ngx_test_null(ctx,  /* 宏：分配失败则直接return；此处分配http块上下文 */
                  ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t)),  /* 从配置内存池清零分配，随配置生命周期统一释放 */
                  NGX_CONF_ERROR);  /* 宏第三参：失败时的返回值 */

    *(ngx_http_conf_ctx_t **) conf = ctx;  /* 块上下文写回conf_ctx槽位：http{}内指令解析时由此取父级配置 */

    /* count the number of the http modules and set up their indices */  /* 统计HTTP模块个数并分配下标 */

    ngx_http_max_module = 0;  /* 计数清零：从0重新编号 */
    for (m = 0; ngx_modules[m]; m++) {  /* 遍历全局模块表ngx_modules(所有已注册模块) */
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {  /* 只要HTTP型模块 */
            continue;  /* 其余类型跳过 */
        }   /* 非HTTP模块判断结束 */

        ngx_modules[m]->ctx_index = ngx_http_max_module++;  /* 0,1,2…连续编号：即三层conf数组中的槽位 */
    }   /* 模块编号遍历结束 */

    /* the main http main_conf, it's the same in the all http contexts */  /* http级main_conf：全局唯一份 */
    ngx_test_null(ctx->main_conf,  /* 分配main_conf指针数组：每模块一槽 */
                  ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module),  /* 长度=HTTP模块总数 */
                  NGX_CONF_ERROR);  /* 失败返回错误 */

    /* the http null srv_conf, it's used to merge the server{}s' srv_conf's */  /* http级"空"srv_conf：合并各server{}的默认值基线 */
    ngx_test_null(ctx->srv_conf,  /* 分配srv_conf指针数组 */
                  ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module),  /* 长度=模块总数 */
                  NGX_CONF_ERROR);  /* 失败返回错误 */

    /* the http null loc_conf, it's used to merge the server{}s' loc_conf's */  /* http级"空"loc_conf：server/location继承链的顶层默认值 */
    ngx_test_null(ctx->loc_conf,  /* 分配loc_conf指针数组 */
                  ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module),  /* 长度=模块总数 */
                  NGX_CONF_ERROR);  /* 失败返回错误 */


    /* create the main_conf, srv_conf and loc_conf in all http modules */  /* 令每个HTTP模块创建自己的三级配置 */

    for (m = 0; ngx_modules[m]; m++) {  /* 再遍历模块表：调用创建配置钩子 */
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {  /* 过滤非HTTP模块 */
            continue;  /* 跳过 */
        }   /* 非HTTP模块过滤判断结束 */

        module = ngx_modules[m]->ctx;  /* 取模块方法表 */
        mi = ngx_modules[m]->ctx_index;  /* 取上面编好的槽位号 */

        if (module->pre_conf) {  /* 有解析前钩子？(模块预注册资源的时机) */
            if (module->pre_conf(cf) != NGX_OK) {  /* 失败即整体放弃 */
                return NGX_CONF_ERROR;  /* 返回配置错误串 */
            }   /* pre_conf失败判断结束 */
        }   /* pre_conf钩子判断结束 */

        if (module->create_main_conf) {  /* 实现了main级配置创建？ */
            ngx_test_null(ctx->main_conf[mi], module->create_main_conf(cf),  /* 创建并存入main_conf槽位 */
                          NGX_CONF_ERROR);  /* 失败返回错误 */
        }   /* create_main_conf判断结束 */

        if (module->create_srv_conf) {  /* 实现了server级配置创建？ */
            ngx_test_null(ctx->srv_conf[mi], module->create_srv_conf(cf),  /* 创建http级默认srv_conf(每个server{}解析时再各自建一份) */
                          NGX_CONF_ERROR);  /* 失败返回错误 */
        }   /* create_srv_conf判断结束 */

        if (module->create_loc_conf) {  /* 实现了location级配置创建？ */
            ngx_test_null(ctx->loc_conf[mi], module->create_loc_conf(cf),  /* 创建http级默认loc_conf：三层继承的顶层默认值 */
                          NGX_CONF_ERROR);  /* 失败返回错误 */
        }   /* create_loc_conf判断结束 */
    }   /* 模块创建配置遍历结束 */

    /* parse inside the http{} block */  /* 解析http{}内部指令 */

    pcf = *cf;  /* 整体拷贝保存外层解析现场 */
    cf->ctx = ctx;  /* 压入http块上下文：内部指令由此取父级配置 */
    cf->module_type = NGX_HTTP_MODULE;  /* 限定http{}内只认HTTP型模块的指令 */
    cf->cmd_type = NGX_HTTP_MAIN_CONF;  /* 当前层级：http main级(嵌套指令据此校验合法性) */
    rv = ngx_conf_parse(cf, NULL);  /* 递归进入解析器逐条读http{}内指令(含各server{})直到块结束 */

    if (rv != NGX_CONF_OK) {  /* 解析失败(含指令回调报错)？ */
        *cf = pcf;  /* 先恢复现场 */
        return rv;  /* 错误上抛 */
    }   /* http{}解析失败处理结束 */

    /* 中文：下面开始配置合并——先初始化http级main_conf，再把各server{}的
     * srv_conf及其location{}的loc_conf逐级向下合并(main→server→location三层继承) */
    /*
     * init http{} main_conf's, merge the server{}s' srv_conf's
     * and its location{}s' loc_conf's
     */

    cmcf = ctx->main_conf[ngx_http_core_module.ctx_index];  /* 取core模块main_conf：phase引擎与servers数组都挂在这里 */
    cscfp = cmcf->servers.elts;  /* servers数组元素区：每项指向一个server{}的core_srv_conf */

    for (m = 0; ngx_modules[m]; m++) {  /* 外层按模块：模块×server全组合逐一合并 */
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {  /* 过滤非HTTP模块 */
            continue;  /* 跳过 */
        }   /* 非HTTP模块过滤判断结束 */

        module = ngx_modules[m]->ctx;  /* 模块方法表 */
        mi = ngx_modules[m]->ctx_index;  /* 模块槽位号 */

        /* init http{} main_conf's */  /* 初始化http级main_conf(全局唯一份) */
        if (module->init_main_conf) {  /* 有main级初始化钩子？(校验/补默认值) */
            rv = module->init_main_conf(cf, ctx->main_conf[mi]);  /* 对http级那份调用 */
            if (rv != NGX_CONF_OK) {  /* 失败？ */
                *cf = pcf;  /* 恢复现场 */
                return rv;  /* 上抛错误 */
            }   /* init_main_conf失败处理结束 */
        }   /* init_main_conf判断结束 */

        for (s = 0; s < cmcf->servers.nelts; s++) {  /* 遍历每个server{}(即每个虚拟主机) */

            /* merge the server{}s' srv_conf's */  /* 合并server{}的srv_conf */

            if (module->merge_srv_conf) {  /* 有srv级合并钩子？ */
                rv = module->merge_srv_conf(cf,  /* 以http级为父默认值合并 */
                                            ctx->srv_conf[mi],  /* 续行：父级(http级)配置 */
                                            cscfp[s]->ctx->srv_conf[mi]);  /* 续行：子级(该server{}自有)配置 */
                if (rv != NGX_CONF_OK) {  /* 合并失败？ */
                    *cf = pcf;  /* 恢复现场 */
                    return rv;  /* 上抛错误 */
                }   /* merge_srv_conf失败处理结束 */
            }   /* merge_srv_conf判断结束 */

            if (module->merge_loc_conf) {  /* 有loc级合并钩子？ */

                /* merge the server{}'s loc_conf */  /* 先合并server{}自身的loc_conf */

                rv = module->merge_loc_conf(cf,  /* 以http级为父默认值合并 */
                                            ctx->loc_conf[mi],  /* 续行：父级配置 */
                                            cscfp[s]->ctx->loc_conf[mi]);  /* 续行：server级配置 */
                if (rv != NGX_CONF_OK) {  /* 失败？ */
                    *cf = pcf;  /* 恢复现场 */
                    return rv;  /* 上抛错误 */
                }   /* server自身loc_conf合并失败处理结束 */

                /* merge the locations{}' loc_conf's */  /* 再递归合并其下的location树 */

                rv = ngx_http_merge_locations(cf, &cscfp[s]->locations,  /* 深度优先逐层合并 */
                                              cscfp[s]->ctx->loc_conf,  /* 续行：父级取server级那份 */
                                              module, mi);  /* 续行：模块钩子与槽位 */
                if (rv != NGX_CONF_OK) {  /* 失败？ */
                    *cf = pcf;  /* 恢复现场 */
                    return rv;  /* 上抛错误 */
                }   /* location树合并失败处理结束 */

#if 0  /* 条件编译关闭：扁平合并写法已被上面的递归版取代 */
                clcfp = (ngx_http_core_loc_conf_t **) cscfp[s]->locations.elts;  /* (未编译)直接取location数组 */

                for (l = 0; l < cscfp[s]->locations.nelts; l++) {  /* (未编译)只合并第一层location */
                    rv = module->merge_loc_conf(cf,  /* (未编译)以server级为父 */
                                                cscfp[s]->ctx->loc_conf[mi],  /* (未编译)父级配置 */
                                                clcfp[l]->loc_conf[mi]);  /* (未编译)子级配置 */
                    if (rv != NGX_CONF_OK) {  /* (未编译)失败处理 */
                        *cf = pcf;  /* (未编译)恢复现场 */
                        return rv;  /* (未编译)上抛错误 */
                    }   /* (未编译)失败处理结束 */
                }   /* (未编译)第一层location遍历结束 */
#endif  /* 条件编译结束 */
            }   /* merge_loc_conf判断结束 */
        }   /* server{}遍历结束 */
    }   /* 模块×server合并遍历结束 */

    /* we needed "http"'s cf->ctx while merging configuration */  /* 合并期间需要http级的cf->ctx，至此才恢复外层现场 */
    *cf = pcf;  /* 恢复main级解析现场 */

    /* init lists of the handlers */  /* 初始化请求处理phase引擎的handler数组 */

    ngx_init_array(cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers,  /* REWRITE阶段handler数组：预分配10个函数指针 */
                   cf->cycle->pool, 10, sizeof(ngx_http_handler_pt),  /* 续行：内存池/预分配个数/元素大小 */
                   NGX_CONF_ERROR);  /* 续行：失败返回值 */
    cmcf->phases[NGX_HTTP_REWRITE_PHASE].type = NGX_OK;  /* 阶段返回值约定：handler返回NGX_OK即结束本阶段(run_phases按下标逆序执行各handler) */


    /* the special find config phase for single handler */  /* find config阶段的特殊处理：固定单handler */

    ngx_init_array(cmcf->phases[NGX_HTTP_FIND_CONFIG_PHASE].handlers,  /* FIND_CONFIG阶段数组：只预分配1个位置 */
                   cf->cycle->pool, 1, sizeof(ngx_http_handler_pt),  /* 续行：内存池/个数/元素大小 */
                   NGX_CONF_ERROR);  /* 续行：失败返回值 */
    cmcf->phases[NGX_HTTP_FIND_CONFIG_PHASE].type = NGX_OK;  /* 返回NGX_OK即结束本阶段 */

    ngx_test_null(h, ngx_push_array(  /* 追加元素 */
                           &cmcf->phases[NGX_HTTP_FIND_CONFIG_PHASE].handlers),  /* 续行：目标数组 */
                  NGX_CONF_ERROR);  /* 宏第三参：失败返回值 */
    *h = ngx_http_find_location_config;  /* 注册find config处理器：按URI在location树中选定location配置(请求路由的核心一步) */


    ngx_init_array(cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers,  /* ACCESS阶段handler数组：访问控制checker链 */
                   cf->cycle->pool, 10, sizeof(ngx_http_handler_pt),  /* 续行：内存池/个数/元素大小 */
                   NGX_CONF_ERROR);  /* 续行：失败返回值 */
    cmcf->phases[NGX_HTTP_ACCESS_PHASE].type = NGX_DECLINED;  /* 约定返回NGX_DECLINED=本模块不否决、继续下一checker；NGX_OK不终止本阶段(所有checker都须通过) */


    ngx_init_array(cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers,  /* CONTENT阶段handler数组：内容产生者(static/index等)按优先级竞争 */
                   cf->cycle->pool, 10, sizeof(ngx_http_handler_pt),  /* 续行：内存池/个数/元素大小 */
                   NGX_CONF_ERROR);  /* 续行：失败返回值 */
    cmcf->phases[NGX_HTTP_CONTENT_PHASE].type = NGX_OK;  /* 某handler接手内容(返回NGX_OK)即结束本阶段 */


    /*
     * create the lists of the ports, the addresses and the server names
     * to allow quickly find the server core module configuration at run-time
     */
    /* 中文：建立 端口→地址→server_name 三层查找表，使运行期能快速定位server配置 */

    ngx_init_array(in_ports, cf->pool, 10, sizeof(ngx_http_in_port_t),  /* 端口表：预分配10个端口节点 */
                   NGX_CONF_ERROR);  /* 续行：失败返回值 */

    /* "server" directives */  /* 遍历server{}指令 */
    cscfp = cmcf->servers.elts;  /* 重取servers数组元素区 */
    for (s = 0; s < cmcf->servers.nelts; s++) {  /* 外层：每个server{}块 */

        /* "listen" directives */  /* 遍历listen指令 */
        lscf = cscfp[s]->listen.elts;  /* 该server{}的listen配置数组 */
        for (l = 0; l < cscfp[s]->listen.nelts; l++) {  /* 每条listen指令(一个server可监听多个IP:端口) */

            port_found = 0;  /* 先假设这是新端口 */

            /* AF_INET only */  /* 仅支持IPv4 */

            in_port = in_ports.elts;  /* 取端口表元素区 */
            for (p = 0; p < in_ports.nelts; p++) {  /* 查该端口是否已收录 */

                if (lscf[l].port == in_port[p].port) {  /* 端口相同：多个server共用监听端口是虚拟主机部署的常态 */

                    /* the port is already in the port list */  /* 端口已在表中 */

                    port_found = 1;  /* 置端口已存在标志 */
                    addr_found = 0;  /* 再假设该端口下这是新地址 */

                    in_addr = in_port[p].addrs.elts;  /* 取该端口下地址表 */
                    for (a = 0; a < in_port[p].addrs.nelts; a++) {  /* 遍历已绑定的地址 */

                        if (lscf[l].addr == in_addr[a].addr) {  /* IP:端口完全相同：同址多server_name，靠Host头分流的虚拟主机 */

                            /* the address is already bound to this port */  /* 地址已绑定在该端口上 */

                            /* "server_name" directives */  /* 处理server_name指令 */
                            s_name = cscfp[s]->server_names.elts;  /* 该server{}声明的域名字符串 */
                            for (n = 0; n < cscfp[s]->server_names.nelts; n++) {  /* 逐个登记到该地址:端口下 */

                                /*
                                 * add the server name and server core module
                                 * configuration to the address:port
                                 */
                                /* 中文：把server_name与其server配置登记到该地址:端口 */

                                /* TODO: duplicate names can be checked here */  /* 原注释：此处可做重名检查 */

                                ngx_test_null(name,  /* 登记一项 */
                                              ngx_push_array(&in_addr[a].names),  /* 名字表扩容取新位 */
                                              NGX_CONF_ERROR);  /* 失败返回错误 */

                                name->name = s_name[n].name;  /* 记录域名：运行期与请求Host头匹配 */
                                name->core_srv_conf = s_name[n].core_srv_conf;  /* 域名→server{}配置映射：命中后请求由它服务 */
                            }   /* server_name登记遍历结束 */

                            /*
                             * check duplicate "default" server that
                             * serves this address:port
                             */
                            /* 中文：检查同一地址:端口的"default" server是否重复 */

                            if (lscf[l].default_server) {  /* 本listen标了default？(Host未命中时的兜底server) */
                                if (in_addr[a].default_server) {  /* 该地址:端口已有default：冲突 */
                                    ngx_log_error(NGX_LOG_ERR, cf->log, 0,  /* 记错误日志 */
                                           "duplicate default server in %s:%d",  /* 正文：指出冲突位置 */
                                           lscf[l].file_name.data,  /* 参数：配置文件名 */
                                           lscf[l].line);  /* 参数：行号 */

                                    return NGX_CONF_ERROR;  /* 默认server必须唯一，拒绝启动 */
                                }   /* default重复冲突判断结束 */

                                in_addr[a].core_srv_conf = cscfp[s];  /* 设为该地址:端口的默认server */
                                in_addr[a].default_server = 1;  /* 打default标记 */
                            }   /* default server设置判断结束 */

                            addr_found = 1;  /* 地址已存在 */

                            break;  /* 跳出地址循环 */

                        } else if (in_addr[a].addr == INADDR_ANY) {  /* 撞上通配绑定*:port：新具体地址须插到它前面 */

                            /*
                             * "*:port" must be the last resort so move it
                             * to the end of the address list and add
                             * the new address at its place
                             */
                            /* 中文：通配*:port是最后手段，须挪到地址表末尾，新地址放到它的原位 */

                            ngx_test_null(inaddr,  /* 在表尾腾一个新位 */
                                          ngx_push_array(&in_port[p].addrs),  /* 地址表扩容 */
                                          NGX_CONF_ERROR);  /* 失败返回错误 */

                            ngx_memcpy(inaddr, &in_addr[a],  /* 把通配项整体搬到表尾 */
                                       sizeof(ngx_http_in_addr_t));  /* 按结构大小复制 */

                            in_addr[a].addr = lscf[l].addr;  /* 原通配位置改放新具体地址(具体地址匹配优先于通配) */
                            in_addr[a].default_server = lscf[l].default_server;  /* 继承default标记 */
                            in_addr[a].core_srv_conf = cscfp[s];  /* 新地址默认关联本server{} */

                            /*
                             * create the empty list of the server names that
                             * can be served on this address:port
                             */
                            /* 中文：为该地址:端口建立空的server_name表 */

                            ngx_init_array(inaddr->names, cf->pool, 10,  /* 搬到表尾的通配项重建空names表(memcpy浅拷来的旧指针不可复用) */
                                           sizeof(ngx_http_server_name_t),  /* 续行：元素大小 */
                                           NGX_CONF_ERROR);  /* 续行：失败返回值 */

                            addr_found = 1;  /* 地址已处理 */

                            break;  /* 跳出地址循环 */
                        }   /* 通配地址插入分支结束 */
                    }   /* 地址查重遍历结束 */

                    if (!addr_found) {  /* 该端口下确为新地址：追加地址项 */

                        /*
                         * add the address to the addresses list that
                         * bound to this port
                         */
                        /* 中文：把新地址加进该端口的地址表 */

                        ngx_test_null(inaddr,  /* 追加一项 */
                                      ngx_push_array(&in_port[p].addrs),  /* 地址表扩容 */
                                      NGX_CONF_ERROR);  /* 失败返回错误 */

                        inaddr->addr = lscf[l].addr;  /* 记录监听地址(网络序) */
                        inaddr->default_server = lscf[l].default_server;  /* default标记 */
                        inaddr->core_srv_conf = cscfp[s];  /* 地址默认关联的server{} */

                        /*
                         * create the empty list of the server names that
                         * can be served on this address:port
                         */
                        /* 中文：为该地址:端口建立空server_name表 */

                        ngx_init_array(inaddr->names, cf->pool, 10,  /* 初始化名字表 */
                                       sizeof(ngx_http_server_name_t),  /* 续行：元素大小 */
                                       NGX_CONF_ERROR);  /* 续行：失败返回值 */
                    }   /* 新地址追加判断结束 */
                }   /* 端口已存在分支结束 */
            }   /* 端口查重遍历结束 */

            if (!port_found) {  /* 全新端口：为它建立端口节点 */

                /* add the port to the in_port list */  /* 把端口加入端口表 */

                ngx_test_null(in_port,  /* 追加端口节点 */
                              ngx_push_array(&in_ports),  /* 端口表扩容 */
                              NGX_CONF_ERROR);  /* 失败返回错误 */

                in_port->port = lscf[l].port;  /* 记录端口号(主机序) */

                ngx_test_null(in_port->port_text.data, ngx_palloc(cf->pool, 7),  /* 预分配7字节，足够容纳":65535" */
                              NGX_CONF_ERROR);  /* 失败返回错误 */
                in_port->port_text.len = ngx_snprintf((char *)  /* 生成":端口"文本(日志与错误提示用) */
                                                      in_port->port_text.data,  /* 续行：写入目标 */
                                                      7, ":%d",  /* 续行：缓冲长度与格式 */
                                                      in_port->port);  /* 续行：端口号 */

                /* create list of the addresses that bound to this port ... */  /* 建立该端口的地址表 */

                ngx_init_array(in_port->addrs, cf->pool, 10,  /* 初始化地址表 */
                               sizeof(ngx_http_in_addr_t),  /* 续行：元素大小 */
                               NGX_CONF_ERROR);  /* 续行：失败返回值 */

                ngx_test_null(inaddr, ngx_push_array(&in_port->addrs),  /* 追加首个地址项 */
                              NGX_CONF_ERROR);  /* 失败返回错误 */

                /* ... and add the address to this list */  /* 填入地址内容 */

                inaddr->addr = lscf[l].addr;  /* 监听地址 */
                inaddr->default_server = lscf[l].default_server;  /* default标记 */
                inaddr->core_srv_conf = cscfp[s];  /* 关联server{}配置 */

                /*
                 * create the empty list of the server names that
                 * can be served on this address:port
                 */
                /* 中文：为该地址:端口建立空server_name表 */

                ngx_init_array(inaddr->names, cf->pool, 10,  /* 初始化名字表：同一IP:端口可服务多个域名 */
                               sizeof(ngx_http_server_name_t),  /* 续行：元素大小 */
                               NGX_CONF_ERROR);  /* 续行：失败返回值 */
            }   /* 新端口节点判断结束 */
        }   /* listen指令遍历结束 */
    }   /* server{}遍历结束 */

    /* optimize the lists of the ports, the addresses and the server names */  /* 优化端口/地址/服务器名表 */

    /* AF_INET only */  /* 仅IPv4 */

    in_port = in_ports.elts;  /* 取端口表 */
    for (p = 0; p < in_ports.nelts; p++) {  /* 逐端口优化并生成监听对象 */

        /* check whether the all server names point to the same server */  /* 检查所有server_name是否指向同一server */

        in_addr = in_port[p].addrs.elts;  /* 取地址表 */
        for (a = 0; a < in_port[p].addrs.nelts; a++) {  /* 逐地址检查 */

            virtual_names = 0;  /* 先假设非真虚拟主机 */

            name = in_addr[a].names.elts;  /* 取该地址的名字表 */
            for (n = 0; n < in_addr[a].names.nelts; n++) {  /* 遍历名字 */
                if (in_addr[a].core_srv_conf != name[n].core_srv_conf) {  /* 有名字指向不同server：确属虚拟主机 */
                    virtual_names = 1;  /* 置标志 */
                    break;  /* 一个即可判定 */
                }   /* 指向不同server判断结束 */
            }   /* 名字遍历结束 */

            /*
             * if the all server names point to the same server
             * then we do not need to check them at run-time
             */
            /* 中文：名字全指向同一server时，运行期无需再按Host逐个匹配 */

            if (!virtual_names) {  /* 非虚拟主机： */
                in_addr[a].names.nelts = 0;  /* 仅置长度0清空名字表，省去每个请求的Host查找开销 */
            }   /* 免Host查找优化判断结束 */
        }   /* 地址检查遍历结束 */

        /*
         * if there's the binding to "*:port" then we need to bind()
         * to "*:port" only and ignore the other bindings
         */
        /* 中文：若存在*:port通配绑定，则只bind它一次即可收到该端口全部连接 */

        if (in_addr[a - 1].addr == INADDR_ANY) {  /* 上循环结束时a==地址数：查末项(通配项已被排到末尾) */
            a--;  /* 从通配项开始：只需一个监听对象 */

        } else {  /* 末项非通配： */
            a = 0;  /* 从首个地址起逐个监听 */
        }   /* 通配判断分支结束 */

        in_addr = in_port[p].addrs.elts;  /* 重新指向地址表首元素 */
        while (a < in_port[p].addrs.nelts) {  /* 对每个需监听的地址生成ngx_listening_t */

            ls = ngx_listening_inet_stream_socket(cf, in_addr[a].addr,  /* 创建TCP流监听对象(记录socket/bind/listen所需的地址) */
                                                  in_port[p].port);  /* 续行：监听端口 */
            if (ls == NULL) {  /* 创建失败？ */
                return NGX_CONF_ERROR;  /* 放弃配置 */
            }   /* 监听创建失败判断结束 */

            ls->backlog = -1;  /* 未显式指定：backlog(连接队列长度)采用事件模块默认值 */
#if 0  /* 条件编译关闭：监听socket阻塞与否的实验代码 */
#if 0  /* 内层：选用非阻塞版本 */
            ls->nonblocking = 1;  /* (未编译)非阻塞监听socket */
#else  /* 内层反选： */
            ls->nonblocking = 0;  /* (未编译)阻塞监听socket */
#endif  /* 内层条件结束 */
#endif  /* 外层条件结束 */
            ls->addr_ntop = 1;  /* 地址转文本：日志输出点分十进制IP而非二进制 */

            ls->handler = ngx_http_init_connection;  /* 新连接HTTP层入口：accept后事件模块回调它(内部再把读事件handler设为ngx_http_init_request解析请求行/头) */

            cscf = in_addr[a].core_srv_conf;  /* 取该地址默认server的配置 */
            ls->pool_size = cscf->connection_pool_size;  /* 每连接内存池大小：连接与请求结构都从这里分配 */
            ls->post_accept_timeout = cscf->post_accept_timeout;  /* accept后等待客户端发完请求的超时 */

            clcf = cscf->ctx->loc_conf[ngx_http_core_module.ctx_index];  /* 取默认server的location级core配置 */
            ls->log = clcf->err_log;  /* 该监听的错误日志：继承server{}的error_log */

#if (WIN32)  /* Windows IOCP特有：AcceptEx可顺带预读首块数据 */
            iocpcf = ngx_event_get_conf(cf->cycle->conf_ctx, ngx_iocp_module);  /* 取IOCP事件模型配置 */
            if (iocpcf->acceptex_read) {  /* 配置了accept后预读？ */
                ls->post_accept_buffer_size = cscf->client_header_buffer_size;  /* 预读缓冲取客户端头缓冲大小 */
            }   /* 预读配置判断结束 */
#endif  /* WIN32条件编译结束 */

            ls->ctx = ctx;  /* 监听对象挂http块上下文：连接进来即可直接取http级配置 */

            if (in_port[p].addrs.nelts > 1) {  /* 该端口还剩多个地址？ */

                in_addr = in_port[p].addrs.elts;  /* 重取地址表首址 */
                if (in_addr[in_port[p].addrs.nelts - 1].addr != INADDR_ANY) {  /* 末项非通配：各具体地址需独立监听 */

                    /*
                     * if this port has not the "*:port" binding then create
                     * the separate ngx_http_in_port_t for the all bindings
                     */
                    /* 中文：无*:port通配绑定时，为每个绑定单独构造in_port结构 */

                    ngx_test_null(inport,  /* 新造单地址端口节点 */
                                  ngx_palloc(cf->pool,  /* 从配置池分配 */
                                             sizeof(ngx_http_in_port_t)),  /* 大小=端口节点结构 */
                                  NGX_CONF_ERROR);  /* 失败返回错误 */

                    inport->port = in_port[p].port;  /* 复制端口号 */
                    inport->port_text = in_port[p].port_text;  /* 复制":端口"文本 */

                    /* init list of the addresses ... */  /* 初始化地址表 */

                    ngx_init_array(inport->addrs, cf->pool, 1,  /* 只预留1个地址 */
                                   sizeof(ngx_http_in_addr_t),  /* 续行：元素大小 */
                                   NGX_CONF_ERROR);  /* 续行：失败返回值 */

                    /* ... and set up it with the first address */  /* 用首地址填充 */

                    inport->addrs.nelts = 1;  /* 有效元素数置1 */
                    inport->addrs.elts = in_port[p].addrs.elts;  /* 直接借用原表当前首元素(零拷贝) */

                    ls->servers = inport;  /* 挂单地址表：运行期按连接的本地地址精确定位server */

                    /* prepare for the next cycle */  /* 为下一轮做准备 */

                    in_port[p].addrs.elts = (char *) in_port[p].addrs.elts  /* 原表首址后移一个元素：吃掉已挂给ls的地址 */
                                                       + in_port[p].addrs.size;  /* 续行：一个元素的字节数 */
                    in_port[p].addrs.nelts--;  /* 剩余地址数减一 */

                    in_addr = (ngx_http_in_addr_t *) in_port[p].addrs.elts;  /* 遍历指针同步到新首址 */
                    a = 0;  /* 相对新表从头计数 */

                    continue;  /* 立即下一轮while：继续为剩余地址建监听 */
                }   /* 无通配须独立监听判断结束 */
            }   /* 多地址判断结束 */

            ls->servers = &in_port[p];  /* 单地址或含通配：整个端口表挂给该监听(accept后在此查server) */
            a++;  /* 推进到下一地址 */
        }   /* 监听地址生成循环结束 */
    }   /* 端口优化遍历结束 */

#if (NGX_DEBUG)  /* 调试编译：打印端口→地址→服务器名三层表 */
    in_port = in_ports.elts;  /* 取端口表 */
    for (p = 0; p < in_ports.nelts; p++) {  /* 逐端口打印 */
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cf->log, 0,  /* 打印端口号与节点地址 */
                      "port: %d %08x", in_port[p].port, &in_port[p]);  /* 续行：格式与参数 */
        in_addr = in_port[p].addrs.elts;  /* 取地址表 */
        for (a = 0; a < in_port[p].addrs.nelts; a++) {  /* 逐地址打印 */
            u_char ip[20];  /* 点分十进制缓冲(最长15字符+余量) */
            ngx_inet_ntop(AF_INET, &in_addr[a].addr, ip, 20);  /* 二进制IP转文本 */
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cf->log, 0,  /* 打印IP与默认server配置指针 */
                           "%s %08x", ip, in_addr[a].core_srv_conf);  /* 续行：格式与参数 */
            s_name = in_addr[a].names.elts;  /* 取名字表 */
            for (n = 0; n < in_addr[a].names.nelts; n++) {  /* 逐名打印 */
                 ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cf->log, 0,  /* 打印域名与对应server配置 */
                                "%s %08x", s_name[n].name.data,  /* 续行：域名字符串 */
                                s_name[n].core_srv_conf);  /* 续行：server配置指针 */
            }   /* 名字打印遍历结束 */
        }   /* 地址打印遍历结束 */
    }   /* 端口打印遍历结束 */
#endif  /* NGX_DEBUG条件编译结束 */

    return NGX_CONF_OK;  /* http{}解析与HTTP框架初始化全部完成 */
}  /* ngx_http_block结束 */


/*
 * ngx_http_merge_locations整体说明：递归合并location树。
 * server{}的loc_conf已与http级合并过；这里以父级配置为默认值，对server下
 * 每个location{}调用各模块merge_loc_conf，再下钻其嵌套location，
 * 实现 location > server > http 三层继承(子级显式设置覆盖父级默认值)。
 */
static char *ngx_http_merge_locations(ngx_conf_t *cf,  /* 解析上下文(日志/内存池) */
                                      ngx_array_t *locations,  /* 本级location数组 */
                                      void **loc_conf,  /* 父级loc_conf数组(默认值来源) */
                                      ngx_http_module_t *module,  /* 当前模块钩子表 */
                                      ngx_uint_t ctx_index)  /* 模块槽位下标 */
{  /* 函数体开始 */
    char                       *rv;  /* 合并钩子返回值 */
    ngx_uint_t                  i;  /* 循环下标 */
    ngx_http_core_loc_conf_t  **clcfp;  /* location级core配置数组(每个location一项) */

    clcfp = /* (ngx_http_core_loc_conf_t **) */ locations->elts;  /* 取元素区(原被注释的强转保留原样)：每项是一个location的core_loc_conf指针 */

    for (i = 0; i < locations->nelts; i++) {  /* 遍历本级每个location{} */
        rv = module->merge_loc_conf(cf, loc_conf[ctx_index],  /* 以父级为默认合并本location配置 */
                                    clcfp[i]->loc_conf[ctx_index]);  /* 续行：本location的配置 */
        if (rv != NGX_CONF_OK) {  /* 失败？ */
            return rv;  /* 上抛错误 */
        }   /* 本级合并失败处理结束 */

        rv = ngx_http_merge_locations(cf, &clcfp[i]->locations,  /* 递归下钻嵌套location(深度优先) */
                                      clcfp[i]->loc_conf, module, ctx_index);  /* 续行：以本location为新的父级 */
        if (rv != NGX_CONF_OK) {  /* 递归中失败？ */
            return rv;  /* 逐层上抛 */
        }   /* 递归合并失败处理结束 */
    }   /* location遍历结束 */

    return NGX_CONF_OK;  /* 本级location树全部合并成功 */
}  /* 函数结束 */
