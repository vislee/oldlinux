
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>                    /* 引入 configure 生成的平台配置头：OS 类型与各能力探测宏(HAVE_EPOLL 等) */
#include <ngx_core.h>                      /* 引入核心头：内存池、连接、共享内存、原子操作等基础设施声明 */
#include <ngx_event.h>                     /* 引入事件框架头：ngx_event_t、事件模块接口、accept 互斥锁等核心定义 */


#define DEFAULT_CONNECTIONS  512           /* connections 指令缺省值：连接池预分配规模，也是每 worker 的 fd 软上限 */


extern ngx_module_t ngx_select_module;     /* 引用 select 事件模块对象：无高级机制平台(及 WIN32)的兜底事件机制 */
extern ngx_event_module_t ngx_select_module_ctx;   /* select 模块上下文：含 actions 动作表与模块名，缺省选择时使用 */

#if (HAVE_KQUEUE)                          /* 编译期条件：FreeBSD/BSD 的 kqueue 事件机制 */
#include <ngx_kqueue_module.h>             /* 引入 kqueue 模块头：模块对象在 init_conf 中作缺省机制 */
#endif                                     /* 结束 HAVE_KQUEUE 条件编译 */

#if (HAVE_DEVPOLL)                         /* 编译期条件：Solaris 的 /dev/poll 事件机制 */
extern ngx_module_t ngx_devpoll_module;            /* 引用 devpoll 模块对象：供缺省机制选择 */
extern ngx_event_module_t ngx_devpoll_module_ctx;  /* 引用 devpoll 模块上下文(动作表+模块名) */
#endif                                     /* 结束 HAVE_DEVPOLL 条件编译 */

#if (HAVE_EPOLL)                           /* 编译期条件：Linux 2.5.44+ 的 epoll(本平台首选机制) */
extern ngx_module_t ngx_epoll_module;              /* 引用 epoll 模块对象：供缺省机制选择 */
extern ngx_event_module_t ngx_epoll_module_ctx;    /* 引用 epoll 模块上下文(动作表+模块名) */
#endif                                     /* 结束 HAVE_EPOLL 条件编译 */

#if (HAVE_RTSIG)                           /* 编译期条件：Linux 实时信号(SIG_RT)机制，epoll 普及前的方案 */
extern ngx_module_t ngx_rtsig_module;              /* 引用 rtsig 模块对象：供缺省机制选择 */
extern ngx_event_module_t ngx_rtsig_module_ctx;    /* 引用 rtsig 模块上下文(动作表+模块名) */
#endif                                     /* 结束 HAVE_RTSIG 条件编译 */

#if (HAVE_AIO)                             /* 编译期条件：异步 I/O(FreeBSD aio 等) */
#include <ngx_aio_module.h>                /* 引入 aio 事件模块头 */
#endif                                     /* 结束 HAVE_AIO 条件编译 */

static ngx_int_t ngx_event_module_init(ngx_cycle_t *cycle);        /* 前置声明：建跨进程共享内存(accept 互斥锁、连接计数器) */
static ngx_int_t ngx_event_process_init(ngx_cycle_t *cycle);       /* 前置声明：worker 初始化——预分配连接池并注册监听读事件 */
static char *ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);   /* 前置声明：解析 events{} 配置块 */

static char *ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd,   /* 前置声明：connections 指令回调(续行见下) */
                                   void *conf);    /* 参数表续行 */
static char *ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);      /* 前置声明：use 指令回调，选定事件机制 */
static char *ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd,      /* 前置声明：debug_connection 回调(续行见下) */
                                        void *conf);       /* 参数表续行 */

static void *ngx_event_create_conf(ngx_cycle_t *cycle);          /* 前置声明：创建事件核心配置，字段全部置 UNSET */
static char *ngx_event_init_conf(ngx_cycle_t *cycle, void *conf);   /* 前置声明：配置收尾，为未设置项补平台缺省机制 */
static char *ngx_accept_mutex_check(ngx_conf_t *cf, void *post, void *data);   /* 前置声明：accept_mutex 取值后处理校验 */


static ngx_uint_t                 ngx_event_max_module;    /* 已编译的事件类模块个数：给每个模块分配事件作用域内下标用 */

ngx_uint_t                        ngx_event_flags;         /* 当前事件机制特性位图：一步式 I/O、精确就绪计数等能力标志 */
ngx_event_actions_t               ngx_event_actions;       /* 事件动作函数表：add/del/process_events 等，运行时指向所选模块的实现 */


ngx_atomic_t                      connection_counter;      /* 连接号计数器的实际存储(静态定义) */
ngx_atomic_t                     *ngx_connection_counter = &connection_counter;   /* 指向计数器：master 模式下会被改指共享内存 */


ngx_atomic_t                     *ngx_accept_mutex_ptr;    /* 共享内存中 accept 锁原子变量的地址：NULL 表示多进程无共享锁 */
ngx_atomic_t                     *ngx_accept_mutex;        /* 本 worker 视角的锁地址：抢锁/放锁对它做原子 CAS(硬件 lock cmpxchg) */
ngx_uint_t                        ngx_accept_mutex_held;   /* 标志：本进程当前是否持锁(持有者才监听并 accept 新连接) */
ngx_msec_t                        ngx_accept_mutex_delay;  /* 抢锁失败后的重试间隔(ms)：错峰争用，实现 worker 间负载均衡 */
ngx_int_t                         ngx_accept_disabled;     /* 连接饥饿度：fd 占比超阈值后为正，事件循环据此主动让出锁 */


#if (NGX_STAT_STUB)                        /* 条件编译：stub_status 状态页所需的原子计数器 */

ngx_atomic_t   ngx_stat_accepted0;         /* "已接受连接总数"的实际存储 */
ngx_atomic_t  *ngx_stat_accepted = &ngx_stat_accepted0;    /* 指针形式：模块初始化后统一改指共享内存 */
ngx_atomic_t   ngx_stat_requests0;         /* "已处理请求总数"的实际存储 */
ngx_atomic_t  *ngx_stat_requests = &ngx_stat_requests0;    /* 指针形式：同上 */
ngx_atomic_t   ngx_stat_active0;           /* "当前活跃连接数"的实际存储 */
ngx_atomic_t  *ngx_stat_active = &ngx_stat_active0;        /* 指针形式：同上 */
ngx_atomic_t   ngx_stat_reading0;          /* "正在读请求的连接数"的实际存储 */
ngx_atomic_t  *ngx_stat_reading = &ngx_stat_reading0;      /* 指针形式：同上 */
ngx_atomic_t   ngx_stat_writing0;          /* "正在写响应的连接数"的实际存储 */
ngx_atomic_t  *ngx_stat_writing = &ngx_stat_reading0;      /* 指针形式：0.1.0 的笔误，仍指向 reading 的存储(两者恒相等) */

#endif                                     /* 结束 NGX_STAT_STUB 条件编译 */



static ngx_command_t  ngx_events_commands[] = {   /* events 模块指令表：仅定义 events{} 块本身 */

    { ngx_string("events"),                /* 指令名：events */
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,   /* 属性：主配置作用域+块指令+不带参数 */
      ngx_events_block,                    /* 回调：进入块时由它接管后续解析 */
      0,                                   /* conf 偏移：块指令不使用 */
      0,                                   /* 数据偏移：块指令不使用 */
      NULL },                              /* 无 post 后处理 */

      ngx_null_command                     /* 指令表结束哨兵 */
};   /* events 指令表定义结束 */

    
static ngx_core_module_t  ngx_events_module_ctx = {   /* events 模块上下文：核心模块壳，不产生自身配置 */
    ngx_string("events"),                  /* 模块名 */
    NULL,                                  /* 无 create_conf：events 块只做二级作用域切换 */
    NULL                                   /* 无 init_conf */
};  /* events 模块上下文定义结束 */


ngx_module_t  ngx_events_module = {        /* events 模块对象：纯配置壳，无任何运行期回调 */
    NGX_MODULE,                            /* 模块结构版本标记 */
    &ngx_events_module_ctx,                /* module context */ /* 上下文指针 */
    ngx_events_commands,                   /* module directives */ /* 指令表 */
    NGX_CORE_MODULE,                       /* module type */ /* 核心模块类型 */
    NULL,                                  /* init module */ /* 无模块级初始化 */
    NULL                                   /* init process */ /* 无进程级初始化(本模块不建共享内存) */
};   /* events 模块定义结束 */


static ngx_str_t  event_core_name = ngx_string("event_core");   /* 事件核心模块名：init_conf 扫描模块表时用于排除自身 */

static ngx_conf_post_t  ngx_accept_mutex_post = { ngx_accept_mutex_check } ;   /* post 结构：accept_mutex 取值后调用 check 校验 */


static ngx_command_t  ngx_event_core_commands[] = {   /* 事件核心模块指令表：即 events{} 块内可用的指令 */

    { ngx_string("connections"),           /* 指令名：connections */
      NGX_EVENT_CONF|NGX_CONF_TAKE1,       /* 属性：事件作用域+恰 1 个参数 */
      ngx_event_connections,               /* 回调：解析连接池大小 */
      0,                                   /* conf 偏移：0 */
      0,                                   /* 数据偏移：0 */
      NULL },                              /* 无 post */

    { ngx_string("use"),                   /* 指令名：use */
      NGX_EVENT_CONF|NGX_CONF_TAKE1,       /* 属性：事件作用域+1 参数 */
      ngx_event_use,                       /* 回调：按名选定事件机制 */
      0,                                   /* conf 偏移：0 */
      0,                                   /* 数据偏移：0 */
      NULL },                              /* 无 post */

    { ngx_string("multi_accept"),          /* 指令名：multi_accept */
      NGX_EVENT_CONF|NGX_CONF_TAKE1,       /* 属性：事件作用域+1 参数(on/off) */
      ngx_conf_set_flag_slot,              /* 通用回调：布尔开关写入配置字段 */
      0,                                   /* conf 偏移：0 */
      offsetof(ngx_event_conf_t, multi_accept),   /* 写入字段：multi_accept */
      NULL },                              /* 无 post */

    { ngx_string("accept_mutex"),          /* 指令名：accept_mutex */
      NGX_EVENT_CONF|NGX_CONF_TAKE1,       /* 属性：事件作用域+1 参数 */
      ngx_conf_set_flag_slot,              /* 通用布尔回调 */
      0,                                   /* conf 偏移：0 */
      offsetof(ngx_event_conf_t, accept_mutex),   /* 写入字段：accept_mutex */
      &ngx_accept_mutex_post },            /* post：取值后校验(无原子操作则强制关) */

    { ngx_string("accept_mutex_delay"),    /* 指令名：accept_mutex_delay */
      NGX_EVENT_CONF|NGX_CONF_TAKE1,       /* 属性：事件作用域+1 参数 */
      ngx_conf_set_msec_slot,              /* 通用回调：解析为毫秒时长 */
      0,                                   /* conf 偏移：0 */
      offsetof(ngx_event_conf_t, accept_mutex_delay),   /* 写入字段：accept_mutex_delay */
      NULL },                              /* 无 post */

    { ngx_string("debug_connection"),      /* 指令名：debug_connection */
      NGX_EVENT_CONF|NGX_CONF_TAKE1,       /* 属性：事件作用域+1 参数 */
      ngx_event_debug_connection,          /* 回调：IP/主机名入调试名单 */
      0,                                   /* conf 偏移：0 */
      0,                                   /* 数据偏移：0 */
      NULL },                              /* 无 post */

      ngx_null_command                     /* 哨兵：指令表结束 */
};   /* 事件核心指令表定义结束 */


ngx_event_module_t  ngx_event_core_module_ctx = {   /* 事件核心模块上下文：事件框架的配置中枢 */
    &event_core_name,                      /* 模块名 "event_core" */
    ngx_event_create_conf,                 /* create configuration */ /* 创建配置回调 */
    ngx_event_init_conf,                   /* init configuration */ /* 配置收尾回调 */

    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }   /* actions 全空：核心模块自身不做 I/O，动作由所选机制模块提供 */
};   /* 事件核心上下文定义结束 */


ngx_module_t  ngx_event_core_module = {    /* 事件核心模块对象：每个 worker 事件框架的装配入口 */
    NGX_MODULE,                            /* 模块结构版本标记 */
    &ngx_event_core_module_ctx,            /* module context */ /* 上下文指针 */
    ngx_event_core_commands,               /* module directives */ /* 指令表 */
    NGX_EVENT_MODULE,                      /* module type */ /* 事件类模块 */
    ngx_event_module_init,                 /* init module */ /* 模块级初始化：建共享内存 */
    ngx_event_process_init                 /* init process */ /* 进程级初始化：建连接池、注册监听事件 */
};   /* 事件核心模块定义结束 */


/*
 * ngx_event_module_init：事件核心模块的模块级初始化，在 master 进程(或单进程模式)执行一次。
 * 职责：申请一块跨进程共享内存，放置两类多进程共享数据——accept 互斥锁(原子变量)与全局
 * 连接计数器。多 worker 模型下 accept 锁是消除"惊群"(thundering herd)的关键：若所有
 * worker 都监听同一端口，一个新连接会唤醒全部阻塞在 epoll_wait 中的进程，却只有一个
 * accept() 成功，其余进程白白付出上下文切换代价。nginx 让 worker 通过原子 CAS(硬件上为
 * 带 lock 前缀的 cmpxchg 指令，保证多核读写一致性)竞争共享锁，任何时刻仅一个进程持锁并
 * 监听，惊群从根源上被消除。每个共享变量独占 128 字节(不小于一条 CPU 缓存行，典型 64B)，
 * 避免 worker 频繁改写相邻变量导致缓存行在核间反复失效同步(伪共享，false sharing)。
 */
static ngx_int_t ngx_event_module_init(ngx_cycle_t *cycle)
{   /* 函数体开始 */
#if !(WIN32)                               /* 非 Windows 平台：POSIX 共享内存由 master 创建 */

    size_t             size;               /* 所需共享内存总字节数 */
    char              *shared;             /* 指向新申请共享内存的首地址 */
    ngx_core_conf_t   *ccf;                /* 核心配置：读 master 开关 */
    ngx_event_conf_t  *ecf;                /* 事件核心配置：本函数取出备用 */

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);   /* 从配置总表中取核心模块配置 */

    if (ccf->master == 0 || ngx_accept_mutex_ptr) {   /* 单进程模式无需共享锁；或共享内存已建(重读配置)则跳过 */
        return NGX_OK;                     /* 直接成功返回 */
    }   /* 跳过条件结束 */

    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);   /* 取事件核心配置(此处仅取出，未使用) */


    /* TODO: 128 is cache line size */

    /* 计算共享内存总大小：每个原子变量独占 128 字节(一条缓存行以上)，杜绝伪共享 */
    size = 128            /* ngx_accept_mutex */   /* accept 互斥锁：跨进程原子 CAS 争用的共享字 */
           + 128;         /* ngx_connection_counter */   /* 全局连接计数器：所有 worker 共同递增 */

#if (NGX_STAT_STUB)                        /* 启用状态统计桩时还需容纳 5 个计数器 */

    size += 128           /* ngx_stat_accepted */   /* 已接受连接计数，占 128 字节缓存行 */
           + 128          /* ngx_stat_requests */   /* 已处理请求计数，占 128 字节缓存行 */
           + 128          /* ngx_stat_active */     /* 活跃连接计数，占 128 字节缓存行 */
           + 128          /* ngx_stat_reading */    /* 读状态连接计数，占 128 字节缓存行 */
           + 128;         /* ngx_stat_writing */    /* 写状态连接计数，占 128 字节缓存行 */

#endif                                     /* 结束 NGX_STAT_STUB 条件编译 */

    if (!(shared = ngx_create_shared_memory(size, cycle->log))) {   /* 创建共享内存映射：fork 后各 worker 共享同一批物理页 */
        return NGX_ERROR;                  /* 创建失败：master 放弃启动 */
    }   /* 共享内存创建结束 */

    ngx_accept_mutex_ptr = (ngx_atomic_t *) shared;              /* 偏移 0：accept 锁原子变量，初值 0 表示无人持有 */
    ngx_connection_counter = (ngx_atomic_t *) (shared + 128);    /* 偏移 128：全局连接计数器，跨进程唯一递增 */

#if (NGX_STAT_STUB)                        /* 状态统计计数器依次排入后续缓存行 */

    ngx_stat_accepted = (ngx_atomic_t *) (shared + 2 * 128);     /* 偏移 256：已接受连接计数器 */
    ngx_stat_requests = (ngx_atomic_t *) (shared + 3 * 128);     /* 偏移 384：已处理请求计数器 */
    ngx_stat_active = (ngx_atomic_t *) (shared + 4 * 128);       /* 偏移 512：活跃连接计数器 */
    ngx_stat_reading = (ngx_atomic_t *) (shared + 5 * 128);      /* 偏移 640：读状态计数器 */
    ngx_stat_writing = (ngx_atomic_t *) (shared + 6 * 128);      /* 偏移 768：写状态计数器 */

#endif                                     /* 结束 NGX_STAT_STUB 条件编译 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,           /* 调试日志：打印计数器地址与初值，核对共享内存布局 */
                   "counter: " PTR_FMT ", %d",                   /* 格式串：指针+整数 */
                   ngx_connection_counter, *ngx_connection_counter);   /* 参数：计数器地址与其当前值(应为 0) */

#endif                                     /* 结束非 Windows 条件：WIN32 由 IOCP 机制另行处理 */

    return NGX_OK;                         /* 共享内存就绪，模块初始化成功 */
}   /* ngx_event_module_init 函数结束 */


/*
 * ngx_event_process_init：每个 worker(及单进程模式)启动时的进程级初始化，整个事件驱动
 * 框架的装配点。工作：1) 多 worker 且配置允许时接上共享 accept 锁，为抗惊群做准备；
 * 2) 初始化定时器红黑树；3) 调用所选事件模块(epoll/kqueue/select...)的 init；
 * 4) 一次性预分配 connections 个 ngx_connection_t 及配套读/写 ngx_event_t——连接池理论：
 * 启动时一次 malloc，运行期 accept/close 只是在池中摘取/归还槽位，零内存分配、fd 即
 * 下标 O(1) 定位，且避免运行期碎片与分配失败路径；5) 为每个监听 socket 绑定连接对象、
 * 设置读事件 handler=ngx_event_accept 并注册。"监听 fd 可读"在内核中的语义即 TCP 全
 * 连接队列(accept 队列，长度为 min(listen backlog, somaxconn))非空，于是 accept 被统一
 * 成普通读事件交给 epoll 等待。
 */
static ngx_int_t ngx_event_process_init(ngx_cycle_t *cycle)
{   /* 函数体开始 */
    ngx_uint_t           m, i;             /* m:模块表下标；i:通用循环变量 */
    ngx_socket_t         fd;               /* 监听 socket 的文件描述符，兼作池下标 */
    ngx_event_t         *rev, *wev;        /* 分别指向连接的读事件与写事件对象 */
    ngx_listening_t     *s;                /* 指向监听 socket 数组首元素 */
    ngx_connection_t    *c;                /* 连接池中的连接对象 */
    ngx_core_conf_t     *ccf;             /* 核心配置：worker 进程数等 */
    ngx_event_conf_t    *ecf;             /* 事件核心配置：connections/use/accept_mutex 等 */
    ngx_event_module_t  *module;          /* 选定事件模块的上下文 */
#if (WIN32)                                /* Windows 专属 */
    ngx_iocp_conf_t     *iocpcf;           /* IOCP 模块配置：AcceptEx 投递数量 */
#endif                                     /* 结束 Windows 条件 */

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);   /* 取核心配置(worker 数) */
    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);   /* 取事件核心配置(连接数/机制) */

    /* 抗惊群 accept 锁的启用条件：共享锁已由 master 创建 + 多 worker + 配置 accept_mutex on */
    if (ngx_accept_mutex_ptr && ccf->worker_processes > 1 && ecf->accept_mutex)
    {   /* 三条件同时成立：启用锁机制 */
        ngx_accept_mutex = ngx_accept_mutex_ptr;   /* 锁指针指向共享内存原子变量：抢锁即原子 CAS(0->1) */
        ngx_accept_mutex_held = 0;                 /* 启动时未持锁：本进程暂不监听 */
        ngx_accept_mutex_delay = ecf->accept_mutex_delay;   /* 记录抢锁重试间隔：错峰争用实现负载均衡 */
    }   /* accept 锁启用配置结束 */

#if (NGX_THREADS)                          /* 多线程支持 */
    if (!(ngx_posted_events_mutex = ngx_mutex_init(cycle->log, 0))) {   /* 创建 posted 事件队列的线程互斥锁 */
        return NGX_ERROR;                  /* 失败则中止初始化 */
    }   /* 互斥锁创建结束 */
#endif                                     /* 结束 NGX_THREADS 条件 */

    if (ngx_event_timer_init(cycle->log) == NGX_ERROR) {   /* 初始化定时器红黑树：所有带超时事件的容器 */
        return NGX_ERROR;                  /* 失败则中止初始化 */
    }   /* 定时器初始化结束 */

    cycle->connection_n = ecf->connections;   /* 记录连接池容量(= connections 指令值) */

    for (m = 0; ngx_modules[m]; m++) {     /* 遍历全局模块表寻找所选事件模块 */
        if (ngx_modules[m]->type != NGX_EVENT_MODULE) {   /* 非事件类模块(核心/HTTP 等)跳过 */
            continue;                      /* 继续下一个 */
        }   /* 类型过滤结束 */

        if (ngx_modules[m]->ctx_index == ecf->use) {   /* ctx_index 等于 use 选定编号：即目标事件机制 */
            module = ngx_modules[m]->ctx;  /* 取模块上下文(含 actions 动作表) */
            if (module->actions.init(cycle) == NGX_ERROR) {   /* 调用事件模块初始化：如 epoll_create 建内核事件表 */
                /* fatal */                /* 原注释：致命错误 */
                exit(2);                   /* 事件机制无法建立，进程无法工作，直接退出 */
            }   /* init 失败分支结束 */
            break;                         /* 装配完成，停止扫描 */
        }   /* 选中模块分支结束 */
    }   /* 模块扫描循环结束 */

    cycle->connections = ngx_alloc(sizeof(ngx_connection_t) * ecf->connections,   /* 一次性分配整个连接池：运行期零 malloc */
                                   cycle->log);   /* 参数：日志对象 */
    if (cycle->connections == NULL) {      /* 分配失败(内存不足) */
        return NGX_ERROR;                  /* 中止启动 */
    }   /* 连接池分配检查结束 */

    c = cycle->connections;                /* c 指向池首元素，便于逐槽初始化 */
    for (i = 0; i < cycle->connection_n; i++) {   /* 逐个初始化每个连接槽位 */
        c[i].fd = (ngx_socket_t) -1;       /* fd=-1 标记槽位空闲(合法 fd 恒非负)：连接池的空闲位图 */
        c[i].data = NULL;                  /* 清空复用指针(空闲链/协议层数据) */
#if (NGX_THREADS)                          /* 多线程支持 */
        c[i].lock = 0;                     /* 槽锁字清零：无线程占用 */
#endif                                     /* 结束线程条件 */
    }   /* 连接池初始化循环结束 */

    cycle->read_events = ngx_alloc(sizeof(ngx_event_t) * ecf->connections,   /* 分配与连接池等长的读事件数组 */
                                   cycle->log);   /* 参数：日志对象 */
    if (cycle->read_events == NULL) {      /* 分配失败 */
        return NGX_ERROR;                  /* 中止启动 */
    }   /* 读事件数组分配检查结束 */

    rev = cycle->read_events;              /* rev 指向读事件数组首元素 */
    for (i = 0; i < cycle->connection_n; i++) {   /* 逐槽初始化读事件 */
        rev[i].closed = 1;                 /* closed=1：尚未绑定有效连接，防事件误用悬空 fd */
#if (NGX_THREADS)                          /* 多线程支持 */
        rev[i].lock = &c[i].lock;          /* 读事件与同槽位连接共用槽锁 */
        rev[i].own_lock = &c[i].lock;      /* 记录归属锁：供事件对象转移时判断 */
#endif                                     /* 结束线程条件 */
    }   /* 读事件初始化循环结束 */

    cycle->write_events = ngx_alloc(sizeof(ngx_event_t) * ecf->connections,   /* 同样一次分配写事件数组 */
                                   cycle->log);   /* 参数：日志对象 */
    if (cycle->write_events == NULL) {     /* 分配失败 */
        return NGX_ERROR;                  /* 中止启动 */
    }   /* 写事件数组分配检查结束 */

    wev = cycle->write_events;             /* wev 指向写事件数组首元素 */
    for (i = 0; i < cycle->connection_n; i++) {   /* 逐槽初始化写事件 */
        wev[i].closed = 1;                 /* 同读事件：标记未绑定连接 */
#if (NGX_THREADS)                          /* 多线程支持 */
        wev[i].lock = &c[i].lock;          /* 写事件与同槽位连接共用槽锁 */
        wev[i].own_lock = &c[i].lock;      /* 记录归属锁 */
#endif                                     /* 结束线程条件 */
    }   /* 写事件初始化循环结束 */

    /* for each listening socket */
    /* 以下为每个监听 socket 建立"监听连接"对象，并把 accept 读事件纳入事件管理 */

    s = cycle->listening.elts;             /* 取监听数组首地址(cycle 初始化时已完成 socket/bind/listen) */
    for (i = 0; i < cycle->listening.nelts; i++) {   /* 逐个处理每个监听 socket */

        fd = s[i].fd;                      /* 取本监听 fd，作为连接池/事件数组的下标 */

#if (WIN32)                                /* Windows 特例 */
        /*
         * Winsock assignes a socket number divisible by 4
         * so to find a connection we divide a socket number by 4.
         */

        fd /= 4;                           /* Winsock 句柄恒为 4 的倍数：除 4 得到连续数组下标 */
#endif                                     /* 结束平台条件 */

        c = &cycle->connections[fd];       /* fd 直接索引连接池：监听 socket 同样占用一个连接槽位 */
        rev = &cycle->read_events[fd];     /* 同下标读事件：承载"全连接队列非空"这一 accept 事件 */
        wev = &cycle->write_events[fd];    /* 同下标写事件：监听不用，占位对齐 */

        ngx_memzero(c, sizeof(ngx_connection_t));   /* 清零连接对象：覆盖池初始化时的空闲标记 */
        ngx_memzero(rev, sizeof(ngx_event_t));      /* 清零读事件对象：归零后再逐项设置 */

        c->fd = s[i].fd;                   /* 连接对象绑定监听 fd */
        c->listening = &s[i];              /* 回指监听结构：accept 时取协议处理器与配置 */

        c->ctx = s[i].ctx;                 /* 继承协议层配置上下文(如 HTTP 配置数组) */
        c->servers = s[i].servers;         /* 继承虚拟主机表：按 Host 头分流的基础 */
        c->log = s[i].log;                 /* 监听级日志对象：accept 阶段错误记到这里 */
        c->read = rev;                     /* 连接指向自己的读事件 */

        /* required by iocp in "c->write->active = 1" */
        c->write = wev;                    /* 连接指向自己的写事件(原注释：IOCP 提前需要) */

        /* required by poll */
        wev->index = NGX_INVALID_INDEX;    /* poll 需要的事件集索引先置无效：注册时由事件模块回填 */

        rev->log = c->log;                 /* 读事件携带日志对象 */
        rev->data = c;                     /* 读事件回指连接：事件触发时由 data 取回连接上下文 */
        rev->index = NGX_INVALID_INDEX;    /* 事件集索引置无效 */

        rev->available = 0;                /* 就绪字节数清零(kqueue/一步式机制才填充) */

        rev->accept = 1;                   /* 标记 accept 专用读事件：读就绪即去 accept 而非读数据 */

#if (HAVE_DEFERRED_ACCEPT)                 /* 支持 TCP_DEFER_ACCEPT(数据到达才唤醒 accept) */
        rev->deferred_accept = s[i].deferred_accept;   /* 继承监听选项：决定 accept 后能否立即读 */
#endif                                     /* 结束 DEFERRED_ACCEPT 条件 */

        if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) {   /* 非 IOCP(UNIX 常规事件机制)才处理旧 cycle 事件 */
            if (s[i].remain) {             /* remain=1：该监听 fd 是平滑升级从旧 cycle 继承来的 */

                /*
                 * delete the old accept events that were bound to
                 * the old cycle read events array
                 */

                if (ngx_del_event(&cycle->old_cycle->read_events[fd],   /* 从内核事件表摘除绑在旧 cycle 事件对象上的 accept 事件 */
                                 NGX_READ_EVENT, NGX_CLOSE_EVENT) == NGX_ERROR)   /* NGX_CLOSE_EVENT：连关闭标志一并清除 */
                {   /* 摘除失败分支开始 */
                    return NGX_ERROR;          /* 初始化中止 */
                }   /* 摘除失败分支结束 */

                cycle->old_cycle->connections[fd].fd = (ngx_socket_t) -1;   /* 旧槽位置 fd=-1：防旧 cycle 退出时重复 close */
            }   /* remain 分支结束 */
        }   /* 非 IOCP 分支结束 */

#if (WIN32)                                /* Windows 分支：AcceptEx 重叠 I/O */

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {   /* IOCP 模式 */
            rev->event_handler = &ngx_event_acceptex;   /* 读事件回调换成 AcceptEx 版本(预投递建连) */

            if (ngx_add_event(rev, 0, NGX_IOCP_ACCEPT) == NGX_ERROR) {   /* 向 IOCP 注册 accept 完成事件 */
                return NGX_ERROR;              /* 注册失败中止 */
            }   /* 注册检查结束 */

            iocpcf = ngx_event_get_conf(cycle->conf_ctx, ngx_iocp_module);   /* 取 IOCP 模块配置 */
            if (ngx_event_post_acceptex(&s[i], iocpcf->post_acceptex)   /* 预投递 post_acceptex 个 AcceptEx：连接未到先备好 */
                                                                  == NGX_ERROR)   /* 与错误比较(续行) */
            {   /* 投递失败分支开始 */
                return NGX_ERROR;              /* 投递失败中止 */
            }   /* 投递失败分支结束 */

        } else {                               /* Windows 非 IOCP(如 select) */
            rev->event_handler = &ngx_event_accept;   /* 与 UNIX 相同的 accept 处理器 */
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {   /* 注册监听读事件 */
                return NGX_ERROR;              /* 注册失败中止 */
            }   /* 注册检查结束 */
        }   /* Windows 非 IOCP 分支结束 */

#else                                      /* UNIX 分支 */

        rev->event_handler = &ngx_event_accept;   /* 设置监听读事件处理器：读就绪(全连接队列非空)即调 accept */

        if (ngx_accept_mutex) {                /* 已启用抗惊群锁：初始不注册监听事件 */
            continue;                          /* 谁抢到锁谁才通过 ngx_enable_accept_events 注册监听 */
        }   /* 锁模式分支结束 */

        if (ngx_event_flags & NGX_USE_RTSIG_EVENT) {   /* rtsig 机制：信号只报 fd 不报事件类型 */
            if (ngx_add_conn(c) == NGX_ERROR) {   /* add_conn：把整个连接的读写一并注册 */
                return NGX_ERROR;              /* 注册失败中止 */
            }   /* 注册检查结束 */

        } else {                               /* epoll/kqueue/select/poll 等常规机制 */
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {   /* 把监听读事件挂入内核事件表(epoll_ctl ADD) */
                return NGX_ERROR;              /* 注册失败中止 */
            }   /* 注册检查结束 */
        }   /* 常规机制分支结束 */

#endif                                     /* 结束平台分支 */
    }   /* 监听 socket 循环结束 */

    return NGX_OK;                         /* worker 事件框架装配完成 */
}   /* ngx_event_process_init 函数结束 */


/*
 * ngx_events_block：解析 nginx.conf 中 events{} 配置块的回调。先给所有事件模块分配
 * ctx_index(事件配置数组中的下标)，为每个模块创建配置结构；随后把解析器上下文切换到
 * "事件模块作用域"递归解析块内指令(use/connections/accept_mutex...)；最后回调各模块的
 * init_conf 补缺省值。这是 nginx 二级配置作用域机制的典型实现。
 */
static char *ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{   /* 函数体开始 */
    int                    m;              /* 模块表下标 */
    char                  *rv;             /* 解析结果串：NGX_CONF_OK 或错误文案 */
    void               ***ctx;             /* 事件作用域配置数组：ctx[i] 指向第 i 个事件模块的配置 */
    ngx_conf_t            pcf;             /* 备份外层解析器状态，保证嵌套解析可恢复 */
    ngx_event_module_t   *module;          /* 事件模块上下文临时指针 */

    /* count the number of the event modules and set up their indices */
    /* 统计事件模块个数并建立各自的下标 */

    ngx_event_max_module = 0;              /* 事件模块计数清零 */
    for (m = 0; ngx_modules[m]; m++) {     /* 遍历全局模块表 */
        if (ngx_modules[m]->type != NGX_EVENT_MODULE) {   /* 只统计事件类模块 */
            continue;                      /* 跳过其他类型 */
        }   /* 类型过滤结束 */

        ngx_modules[m]->ctx_index = ngx_event_max_module++;   /* 给每个事件模块发事件作用域内的唯一下标 */
    }   /* 计数循环结束 */

    ngx_test_null(ctx, ngx_pcalloc(cf->pool, sizeof(void *)), NGX_CONF_ERROR);   /* 分配存放"配置数组地址"的槽位(池分配) */

    ngx_test_null(*ctx,                    /* 分配真正的配置指针数组 */
                  ngx_pcalloc(cf->pool, ngx_event_max_module * sizeof(void *)),   /* 长度=事件模块个数：每格存一个模块配置 */
                  NGX_CONF_ERROR);         /* 失败返回配置错误 */

    *(void **) conf = ctx;                 /* 挂到上层 conf_ctx 对应槽位：ngx_event_get_conf 运行期据此取配置 */

    for (m = 0; ngx_modules[m]; m++) {     /* 再次遍历：为各事件模块创建配置 */
        if (ngx_modules[m]->type != NGX_EVENT_MODULE) {   /* 过滤非事件模块 */
            continue;                      /* 跳过 */
        }   /* 类型过滤结束 */

        module = ngx_modules[m]->ctx;      /* 取模块上下文 */

        if (module->create_conf) {         /* 模块若定义了 create_conf 回调 */
            ngx_test_null((*ctx)[ngx_modules[m]->ctx_index],   /* 在其专属槽位创建配置(字段全 UNSET) */
                          module->create_conf(cf->cycle),      /* 调用模块自己的创建函数 */
                          NGX_CONF_ERROR);     /* 失败返回配置错误 */
        }   /* create_conf 结束 */
    }   /* 创建配置循环结束 */

    pcf = *cf;                             /* 备份当前解析器状态 */
    cf->ctx = ctx;                         /* 切换到事件作用域：指令解析从事件配置数组取 conf */
    cf->module_type = NGX_EVENT_MODULE;    /* 限定块内指令属于事件类模块 */
    cf->cmd_type = NGX_EVENT_CONF;         /* 指令属性过滤：只接受 NGX_EVENT_CONF 指令 */
    rv = ngx_conf_parse(cf, NULL);         /* 递归解析 events{} 块内全部指令直到 '}' */
    *cf = pcf;                             /* 恢复外层解析器状态：嵌套解析互不污染 */

    if (rv != NGX_CONF_OK)                 /* 块内解析出错 */
        return rv;                         /* 错误向上传递 */

    for (m = 0; ngx_modules[m]; m++) {     /* 解析完成后遍历：各模块配置收尾 */
        if (ngx_modules[m]->type != NGX_EVENT_MODULE) {   /* 过滤 */
            continue;                      /* 跳过 */
        }   /* 类型过滤结束 */

        module = ngx_modules[m]->ctx;      /* 取模块上下文 */

        if (module->init_conf) {           /* 模块若定义了 init_conf 回调 */
            rv = module->init_conf(cf->cycle,                   /* 用解析结果收尾：补缺省、做一致性校验 */
                                   (*ctx)[ngx_modules[m]->ctx_index]);   /* 参数：本模块配置结构 */
            if (rv != NGX_CONF_OK) {       /* 校验失败 */
                return rv;                 /* 返回错误文案 */
            }   /* 校验检查结束 */
        }   /* init_conf 结束 */
    }   /* 收尾循环结束 */

    return NGX_CONF_OK;                    /* events{} 块解析全部成功 */
}   /* ngx_events_block 函数结束 */


/*
 * ngx_event_connections：connections 指令回调。设定每个 worker 的连接池大小——它同时
 * 决定读/写事件数组长度与 fd 软上限，是"启动时一次性预分配"模型的规模参数。
 */
static char *ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd,
                                   void *conf)   /* 参数表续行 */
{   /* 函数体开始 */
    ngx_event_conf_t  *ecf = conf;         /* conf 即本模块配置结构(create_conf 创建) */

    ngx_str_t  *value;                     /* 指令参数数组元素 */

    if (ecf->connections != NGX_CONF_UNSET_UINT) {   /* 已设置过：同块内不允许重复出现 */
        return "is duplicate" ;            /* 返回重复错误文案 */
    }   /* 重复检查结束 */

    value = cf->args->elts;                /* value[0]=指令名，value[1]=参数 */
    ecf->connections = ngx_atoi(value[1].data, value[1].len);   /* 字符串转整数：连接池大小 */
    if (ecf->connections == (ngx_uint_t) NGX_ERROR) {   /* 转换失败：参数不是合法数字 */
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,               /* 致命错误日志(带配置文件行号) */
                           "invalid number \"%s\"", value[1].data);   /* 报出参数原文 */

        return NGX_CONF_ERROR;             /* 配置失败 */
    }   /* 数值校验结束 */

    cf->cycle->connection_n = ecf->connections;   /* 同步到 cycle：process_init 按此值分配连接池 */

    return NGX_CONF_OK;                    /* 指令解析成功 */
}   /* ngx_event_connections 函数结束 */


/*
 * ngx_event_use：use 指令回调。按模块名(epoll/kqueue/select...)选定事件机制并记录其
 * ctx_index，供 process_init 装配动作表。单进程(无 master)模式下不允许更换事件机制：
 * 旧机制的内核事件对象仍在，热切换会丢失全部已注册事件。
 */
static char *ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{   /* 函数体开始 */
    ngx_event_conf_t  *ecf = conf;         /* 本模块配置 */

    ngx_int_t             m;               /* 模块表下标 */
    ngx_str_t            *value;           /* 指令参数 */
    ngx_event_conf_t     *old_ecf;         /* 旧 cycle(平滑升级前)的事件配置 */
    ngx_event_module_t   *module;          /* 事件模块上下文 */

    if (ecf->use != NGX_CONF_UNSET_UINT) {   /* 重复设置检查 */
        return "is duplicate" ;            /* 同一作用域只能 use 一次 */
    }   /* 重复检查结束 */

    value = cf->args->elts;                /* value[1] 为事件机制名 */

    if (cf->cycle->old_cycle->conf_ctx) {  /* 存在旧 cycle：保留了上一份配置 */
        old_ecf = ngx_event_get_conf(cf->cycle->old_cycle->conf_ctx,   /* 取旧配置中事件核心模块的配置 */
                                     ngx_event_core_module);   /* 续行：模块参数 */
    } else {                               /* 首次启动无旧 cycle */
        old_ecf = NULL;                    /* 置空便于后面判断 */
    }   /* 旧配置获取结束 */


    for (m = 0; ngx_modules[m]; m++) {     /* 遍历模块表按名匹配事件机制 */
        if (ngx_modules[m]->type != NGX_EVENT_MODULE) {   /* 只看事件类模块 */
            continue;                      /* 跳过 */
        }   /* 类型过滤结束 */

        module = ngx_modules[m]->ctx;      /* 取模块上下文(含名字) */
        if (module->name->len == value[1].len) {   /* 先比长度：快速排除不同名候选 */
            if (ngx_strcmp(module->name->data, value[1].data) == 0) {   /* 逐字节比较确认同名 */
                ecf->use = ngx_modules[m]->ctx_index;   /* 记下选中机制编号：process_init 据此装配 */
                ecf->name = module->name->data;   /* 记下机制名首址 */

                if (ngx_process == NGX_PROCESS_SINGLE   /* 单进程模式(无 master) */
                    && old_ecf                          /* 且存在旧配置 */
                    && old_ecf->use != ecf->use)        /* 且新旧事件机制不同 */
                {   /* 拒绝条件成立 */
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,   /* 致命错误：单进程不允许换机制 */
                        "when the server runs without a master process "   /* 文案片段：无 master 时 */
                        "the \"%s\" event type must be the same as "       /* 文案片段：机制须与旧配置一致 */
                        "in previous configuration - \"%s\" "              /* 文案片段：旧机制名占位 */
                        "and it can not be changed on the fly, "           /* 文案片段：不能在线更换 */
                        "to change it you need to stop server "            /* 文案片段：需停止服务 */
                        "and start it again",                              /* 文案片段：再重新启动 */
                        value[1].data, old_ecf->name);                     /* 参数：新名与旧名 */

                    return NGX_CONF_ERROR;   /* 拒绝该配置 */
                }   /* 单进程校验结束 */

                return NGX_CONF_OK;          /* 匹配成功，指令完成 */
            }   /* 名字匹配结束 */
        }   /* 长度匹配结束 */
    }   /* 模块遍历结束 */

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,   /* 无一匹配：非法事件机制名 */
                       "invalid event type \"%s\"", value[1].data);   /* 报出该名字 */

    return NGX_CONF_ERROR;                 /* 配置失败 */
}   /* ngx_event_use 函数结束 */


/*
 * ngx_event_debug_connection：debug_connection 指令回调。仅在 --with-debug 构建下生效：
 * 把指定客户端 IP(点分十进制或域名)加入调试名单，只有名单内连接输出 debug 级日志。
 */
static char *ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd,
                                        void *conf)   /* 参数表续行 */
{   /* 函数体开始 */
#if (NGX_DEBUG)                            /* 仅 debug 构建有实际逻辑 */
    ngx_event_conf_t  *ecf = conf;         /* 本模块配置 */

    in_addr_t       *addr;                 /* 名单中的网络序 IPv4 地址 */
    ngx_str_t       *value;                /* 指令参数 */
    struct hostent  *h;                    /* 域名解析结果(gethostbyname) */

    value = cf->args->elts;                /* value[1] 为 IP 或主机名 */

    /* AF_INET only */

    if (!(addr = ngx_push_array(&ecf->debug_connection))) {   /* 在调试名单数组追加一个槽位 */
        return NGX_CONF_ERROR;             /* 内存分配失败 */
    }   /* 追加检查结束 */

    *addr = inet_addr((char *) value[1].data);   /* 先按点分十进制解析(返回网络字节序地址) */

    if (*addr != INADDR_NONE) {            /* 解析成功即完成(仅 255.255.255.255 有歧义，可忽略) */
        return NGX_OK;                     /* 返回成功 */
    }   /* 点分解析结束 */

    h = gethostbyname((char *) value[1].data);   /* 非点分 IP：尝试 DNS 解析主机名 */

    if (h == NULL || h->h_addr_list[0] == NULL) {   /* 解析失败或无 A 记录 */
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,   /* 致命错误日志 */
                           "host %s not found", value[1].data);   /* 报告主机未找到 */
        return NGX_CONF_ERROR;             /* 配置失败 */
    }   /* 域名解析检查结束 */

    *addr = *(in_addr_t *)(h->h_addr_list[0]);   /* 取解析到的第一个 IPv4 地址(网络字节序)入名单 */

#else                                      /* 非 debug 构建 */

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,   /* 仅告警：指令被忽略 */
                       "\"debug_connection\" is ignored, you need to rebuild "   /* 文案片段：需重新编译 */
                       "nginx using --with-debug option to enable it");          /* 文案片段：加 --with-debug */

#endif                                     /* 结束 NGX_DEBUG 条件 */

    return NGX_OK;                         /* 成功返回 */
}   /* ngx_event_debug_connection 函数结束 */


/*
 * ngx_event_create_conf：为事件核心模块创建配置结构。nginx 配置理论：所有字段先置
 * UNSET 哨兵值，以区分"用户未配置"与"配置为 0/false"，由 init_conf 收尾时补缺省。
 */
static void *ngx_event_create_conf(ngx_cycle_t *cycle)
{   /* 函数体开始 */
    ngx_event_conf_t  *ecf;                /* 待创建的配置结构 */

    ngx_test_null(ecf, ngx_palloc(cycle->pool, sizeof(ngx_event_conf_t)),   /* 从配置内存池分配：整池统一释放 */
                  NGX_CONF_ERROR);         /* 失败返回配置错误 */

    ecf->connections = NGX_CONF_UNSET_UINT;   /* connections 未设 */
    ecf->use = NGX_CONF_UNSET_UINT;           /* use 未设 */
    ecf->multi_accept = NGX_CONF_UNSET;       /* multi_accept 未设 */
    ecf->accept_mutex = NGX_CONF_UNSET;       /* accept_mutex 未设 */
    ecf->accept_mutex_delay = NGX_CONF_UNSET_MSEC;   /* 抢锁间隔未设 */
    ecf->name = (void *) NGX_CONF_UNSET;      /* 机制名未设 */

#if (NGX_DEBUG)                            /* debug 构建才需要调试名单数组 */
    ngx_init_array(ecf->debug_connection, cycle->pool, 5, sizeof(in_addr_t),   /* 初始化可增长数组：初始容量 5 个地址 */
                   NGX_CONF_ERROR);        /* 失败返回配置错误 */
#endif                                     /* 结束 NGX_DEBUG 条件 */

    return ecf;                            /* 返回初始化好的配置结构 */
}   /* ngx_event_create_conf 函数结束 */


/*
 * ngx_event_init_conf：events{} 块解析收尾回调。用户未显式 use 时按编译期探测结果选择
 * 本平台最优机制(kqueue > devpoll > epoll > rtsig > select)；select 因 fd_set 位图容量
 * (FD_SETSIZE，通常 1024)还须封顶 connections。最后为 accept 锁相关项填缺省：
 * accept_mutex 默认开启——多 worker 下这是抑制惊群、均衡 accept 的基石。
 */
static char *ngx_event_init_conf(ngx_cycle_t *cycle, void *conf)
{   /* 函数体开始 */
    ngx_event_conf_t  *ecf = conf;         /* 本模块配置 */
#if (HAVE_RTSIG)                           /* rtsig 校验分支需要核心配置 */
    ngx_core_conf_t  *ccf;                 /* 核心配置指针：取 worker 数 */
#endif                                     /* 结束条件 */

#if (HAVE_KQUEUE)                          /* 首选：BSD kqueue(内核事件队列，支持精确就绪计数) */

    ngx_conf_init_unsigned_value(ecf->connections, DEFAULT_CONNECTIONS);   /* 未配置则缺省 512 */
    ngx_conf_init_unsigned_value(ecf->use, ngx_kqueue_module.ctx_index);   /* 未 use 则选 kqueue */
    ngx_conf_init_ptr_value(ecf->name, ngx_kqueue_module_ctx.name->data);  /* 名字同步为 kqueue */

#elif (HAVE_DEVPOLL)                       /* 次选：Solaris /dev/poll */

    ngx_conf_init_unsigned_value(ecf->connections, DEFAULT_CONNECTIONS);   /* 缺省 512 */
    ngx_conf_init_unsigned_value(ecf->use, ngx_devpoll_module.ctx_index);  /* 选 devpoll */
    ngx_conf_init_ptr_value(ecf->name, ngx_devpoll_module_ctx.name->data); /* 名字同步 */

#elif (HAVE_EPOLL)                         /* Linux 首选：epoll(红黑树+就绪链表，O(1) 通知) */

    ngx_conf_init_unsigned_value(ecf->connections, DEFAULT_CONNECTIONS);   /* 缺省 512 */
    ngx_conf_init_unsigned_value(ecf->use, ngx_epoll_module.ctx_index);    /* 选 epoll */
    ngx_conf_init_ptr_value(ecf->name, ngx_epoll_module_ctx.name->data);   /* 名字同步 */

#elif (HAVE_RTSIG)                         /* 再次：Linux 实时信号机制 */

    ngx_conf_init_unsigned_value(ecf->connections, DEFAULT_CONNECTIONS);   /* 缺省 512 */
    ngx_conf_init_unsigned_value(ecf->use, ngx_rtsig_module.ctx_index);    /* 选 rtsig */
    ngx_conf_init_ptr_value(ecf->name, ngx_rtsig_module_ctx.name->data);   /* 名字同步 */

#elif (HAVE_SELECT)                        /* 兜底：select(全量扫描+fd_set 容量上限) */

#if (WIN32)                                /* Windows 的 select 无 fd_set 位图限制 */
    ngx_conf_init_unsigned_value(ecf->connections, DEFAULT_CONNECTIONS);   /* 直接用缺省 512 */
#else                                      /* UNIX 的 select 受 FD_SETSIZE 限制 */
    ngx_conf_init_unsigned_value(ecf->connections,          /* connections 不得超过 FD_SETSIZE(通常 1024) */
          FD_SETSIZE < DEFAULT_CONNECTIONS ? FD_SETSIZE : DEFAULT_CONNECTIONS);   /* 两者取小：否则 fd_set 越界 */
#endif                                     /* 结束平台条件 */

    ngx_conf_init_unsigned_value(ecf->use, ngx_select_module.ctx_index);   /* 选 select */
    ngx_conf_init_ptr_value(ecf->name, ngx_select_module_ctx.name->data);  /* 名字同步 */

#else                                      /* 无编译期探测到的机制：运行时扫描 */

    ngx_int_t            i, m;             /* i:模块表下标；m:选中机制编号 */
    ngx_event_module_t  *module;           /* 选中模块上下文 */

    m = -1;                                /* -1 表示尚未找到任何机制 */
    module = NULL;                         /* 上下文同步置空 */

    for (i = 0; ngx_modules[i]; i++) {     /* 遍历模块表找第一个可用机制 */
        if (ngx_modules[i]->type == NGX_EVENT_MODULE) {   /* 命中事件类模块 */
            module = ngx_modules[i]->ctx;  /* 记下其上下文 */

            if (ngx_strcmp(module->name->data, event_core_name.data) == 0) {   /* 跳过 event_core 自身(配置壳非机制) */
                continue;                  /* 找下一个 */
            }   /* 排除判断结束 */

            m = ngx_modules[i]->ctx_index; /* 记录第一个真实机制的编号 */
            break;                         /* 找到即止 */
        }   /* 事件模块分支结束 */
    }   /* 扫描循环结束 */

    if (m == -1) {                         /* 一个事件机制都没编译进来 */
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0, "no events module found");   /* 致命：无法工作 */
        return NGX_CONF_ERROR;             /* 配置失败 */
    }   /* 找到检查结束 */

    ngx_conf_init_unsigned_value(ecf->connections, DEFAULT_CONNECTIONS);   /* 缺省 512 */

    ngx_conf_init_unsigned_value(ecf->use, m);           /* 使用扫描到的机制 */
    ngx_conf_init_ptr_value(ecf->name, module->name->data);   /* 名字同步 */

#endif                                     /* 结束事件机制选择链 */

    cycle->connection_n = ecf->connections;   /* 最终连接数同步到 cycle：进程初始化据此分配连接池 */

    ngx_conf_init_value(ecf->multi_accept, 0);        /* multi_accept 缺省关：单事件只 accept 一个，worker 间公平轮转 */
    ngx_conf_init_value(ecf->accept_mutex, 1);        /* accept_mutex 缺省开：多 worker 抗惊群的核心开关 */
    ngx_conf_init_msec_value(ecf->accept_mutex_delay, 500);   /* 抢锁重试间隔缺省 500ms：越小越均匀但锁争用越多 */

#if (HAVE_RTSIG)                           /* rtsig 特有约束 */
    if (ecf->use == ngx_rtsig_module.ctx_index && ecf->accept_mutex == 0) {   /* rtsig 却关闭 accept 锁 */
        ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx,   /* 取核心配置判断是否多进程 */
                                               ngx_core_module);   /* 续行：模块参数 */
        if (ccf->worker_processes) {       /* worker 数大于 0(多进程) */
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,   /* 致命错误日志 */
                          "the \"rtsig\" method requires "     /* 文案片段：rtsig 机制要求 */
                          "\"accept_mutex\" to be on");        /* 文案片段：必须开 accept 锁 */
            return NGX_CONF_ERROR;             /* 拒绝配置：信号随机投递到任一进程，无锁则 accept 失控 */
        }   /* 多进程判断结束 */
    }   /* rtsig 校验结束 */
#endif                                     /* 结束 rtsig 条件 */

    return NGX_CONF_OK;                    /* 配置收尾成功 */
}   /* ngx_event_init_conf 函数结束 */


/*
 * ngx_accept_mutex_check：accept_mutex 指令的 post 后处理回调。平台缺少原子 CAS 指令
 * (NGX_HAVE_ATOMIC_OPS 未定义)时无法实现跨进程自旋锁，只能强制关闭 accept_mutex，
 * 退回所有 worker 都监听、由内核惊群唤醒所有竞争者的原始模式。
 */
static char *ngx_accept_mutex_check(ngx_conf_t *cf, void *post, void *data)
{   /* 函数体开始 */
#if !(NGX_HAVE_ATOMIC_OPS)                 /* 无原子操作支持的平台 */

    ngx_flag_t *fp = data;                 /* data 即指令写入的开关值 */

    *fp = 0;                               /* 强制清零：禁用 accept 互斥锁 */

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,   /* 告警日志告知用户 */
                       "\"accept_mutex\" is not supported, ignored");   /* 文案：不支持，已忽略 */

#endif                                     /* 结束条件编译 */

    return NGX_CONF_OK;                    /* 无论是否改写，指令解析均算成功 */
}   /* ngx_accept_mutex_check 函数结束 */
