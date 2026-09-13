
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>     /* 编译期全局配置头：集中定义 WIN32、HAVE_SELECT_CHANGE_TIMEOUT 等平台特性宏，决定本文件各条件编译分支的取舍 */
#include <ngx_core.h>       /* nginx 核心头：基本类型(ngx_int_t/ngx_uint_t)、内存分配(ngx_alloc/ngx_free)、日志宏等基础设施 */
#include <ngx_event.h>      /* 事件框架头：ngx_event_t/ngx_connection_t/ngx_event_actions 等事件驱动抽象，本文件是其中 select 后端的具体实现 */



static ngx_int_t ngx_select_init(ngx_cycle_t *cycle);      /* 初始化回调：分配事件索引表并向事件框架注册 select 动作集 */
static void ngx_select_done(ngx_cycle_t *cycle);           /* 销毁回调：释放事件索引表等资源 */
static ngx_int_t ngx_select_add_event(ngx_event_t *ev, int event, u_int flags);   /* 登记事件：把 fd 对应的 bit 置入 master fd_set 位图 */
static ngx_int_t ngx_select_del_event(ngx_event_t *ev, int event, u_int flags);   /* 摘除事件：把 fd 的 bit 从 master fd_set 位图清除 */
static ngx_int_t ngx_select_process_events(ngx_cycle_t *cycle);   /* 事件处理主入口：阻塞在 select() 上等待内核就绪通知并分发事件 */
static char *ngx_select_init_conf(ngx_cycle_t *cycle, void *conf);  /* 配置校验回调：connections 不得超过 FD_SETSIZE 位图容量 */


static fd_set         master_read_fd_set;    /* 主读位图：fd_set 本质是定长位数组(每个 fd 占 1 bit)，硬上限 FD_SETSIZE=1024，保存本进程所有关心"可读"的 fd */
static fd_set         master_write_fd_set;   /* 主写位图：保存所有关心"可写"的 fd；FD_ZERO/FD_SET/FD_CLR 全是用户态对这张位图的操作，真正的监视要等 select() 拷入内核 */
static fd_set         work_read_fd_set;      /* 工作读位图：每轮 select 前从主集合整块拷贝，因为 select() 返回时会把位图破坏性改写成"仅就绪 fd 置位" */
static fd_set         work_write_fd_set;     /* 工作写位图：同上，select 的覆写语义决定了每轮调用都必须重建整个集合 */

#if (WIN32)     /* Windows 平台：winsock 的 select 第一个参数被忽略，无法用 max_fd 表达集合上界，改用计数方式管理容量 */
static int            max_read;    /* 读位图中已登记的 fd 计数，用于判断是否触及 FD_SETSIZE 上限 */
static int            max_write;   /* 写位图中已登记的 fd 计数，同上 */
#else     /* 类 Unix 平台 */
static int            max_fd;    /* 已登记的最大描述符编号；select 第一参数须传 max_fd+1，内核据此确定要线性扫描的位图区间 [0, max_fd] */
#endif     /* 声明平台分支结束 */

static ngx_uint_t     nevents;    /* 已登记事件个数：即本 worker 当前 select 监视的 fd 数量，也是 select 三次 O(n) 扫描的规模 */

static ngx_event_t  **event_index;    /* 事件指针索引表：下标即登记序号，select 返回后靠它把位图中的就绪位反查回具体事件(epoll 则由内核直接返回就绪链表，无需反查) */
#if 0     /* 被编译开关关闭的备选实现代码，不参与编译 */
static ngx_event_t  **ready_index;    /* 备选：独立的就绪事件数组，本可免去每轮对 event_index 的全量扫描 */
#endif     /* 备选声明代码段结束 */

static ngx_event_t   *accept_events;  /* accept 事件链表头：本轮就绪的监听 socket 事件串成单链表，延迟到持 accept 互斥锁后统一处理 */


static ngx_str_t    select_name = ngx_string("select");   /* 事件模块名，nginx.conf 的 use 指令按此名字选择事件模型 */

ngx_event_module_t  ngx_select_module_ctx = {    /* select 事件模块上下文：向事件框架声明本后端的全部能力 */
    &select_name,    /* 模块名字符串指针 */
    NULL,                                  /* create configuration */     /* 创建配置回调：select 无私有配置指令，置空 */
    ngx_select_init_conf,                  /* init configuration */       /* 配置初始化/校验回调 */

    {    /* actions 动作集：事件框架与具体 IO 多路复用实现之间的虚函数表 */
        ngx_select_add_event,              /* add an event */             /* 注册事件：fd 置位到 master 位图 */
        ngx_select_del_event,              /* delete an event */          /* 注销事件：fd 从 master 位图清位 */
        ngx_select_add_event,              /* enable an event */          /* 启用事件：select 语义下 enable 等价于 add */
        ngx_select_del_event,              /* disable an event */         /* 禁用事件：等价于 del */
        NULL,                              /* add an connection */        /* 连接级独立注册：select 以事件为登记单位，无需额外动作 */
        NULL,                              /* delete an connection */     /* 同上 */
        NULL,                              /* process the changes */      /* 增量变更集接口(epoll 风格)：select 全量重建位图，用不到 */
        ngx_select_process_events,         /* process the events */       /* 事件处理主函数：worker 事件循环每轮经 ngx_process_events 调到这里 */
        ngx_select_init,                   /* init the events */          /* 事件模型初始化回调 */
        ngx_select_done                    /* done the events */          /* 事件模型销毁回调 */
    }    /* actions 动作集初始化列表结束 */

};     /* select 事件模块上下文定义结束 */

ngx_module_t  ngx_select_module = {    /* nginx 通用模块结构：所有模块以统一形式注册进核心，供配置解析与初始化排序使用 */
    NGX_MODULE,                        /* 版本签名宏：编译时校验模块与核心的二进制兼容性 */
    &ngx_select_module_ctx,                /* module context */     /* 指向上面 select 事件模块上下文 */
    NULL,                                  /* module directives */  /* 本模块无配置指令表 */
    NGX_EVENT_MODULE,                      /* module type */        /* 类型标记：事件模块，参与事件模块 ctx_index 编号分配 */
    NULL,                                  /* init module */        /* 无全局(跨进程)初始化钩子 */
    NULL                                   /* init process */       /* 无每进程初始化钩子 */
};     /* select 通用模块描述结构定义结束 */


/*
 * ngx_select_init —— select 事件模型初始化回调。
 * 首次进入时清零 master fd_set 位图；按配置的最大连接数分配 event_index 事件索引表
 * (平滑 reload 时迁移旧表内容)；最后把本文件的函数集安装进全局 ngx_event_actions，
 * 此后事件框架的 add/del/process_events 调用都会间接落到这里。
 * 标志位表明 select 是水平触发(level-triggered)：只要 fd 仍就绪，每轮 select 都重复报告；
 * 同时支持 oneshot：通知一次后自动摘除，需要处理完重新注册。
 */
static ngx_int_t ngx_select_init(ngx_cycle_t *cycle)    /* 事件模型初始化回调：框架在启动/reload 时调用 */
{    /* 初始化逻辑开始 */
    ngx_event_t  **index;    /* 新索引表暂存指针，分配成功后再切换全局变量，避免中途失败破坏旧表 */

    if (event_index == NULL) {    /* 首次初始化：事件表尚不存在，主位图内容不可信 */
        FD_ZERO(&master_read_fd_set);    /* 用户态宏：把读 fd_set 位图整块清零，此刻 select 尚不监视任何 fd */
        FD_ZERO(&master_write_fd_set);   /* 写 fd_set 位图同样清零 */
        nevents = 0;    /* 已登记事件计数归零 */
    }    /* 首次初始化分支结束：两张主位图已清零 */

    if (ngx_process == NGX_PROCESS_WORKER    /* 只有 worker 进程真正跑事件循环，需要维护事件表 */
        || cycle->old_cycle == NULL    /* reload/启动时若无旧周期对象，必须全新分配 */
        || cycle->old_cycle->connection_n < cycle->connection_n)   /* 新配置连接数变大，旧表容量不够 */
    {    /* 满足任一条件即需要(重新)分配索引表 */
        ngx_test_null(index,    /* 分配包装宏：失败时记日志并让函数返回 NGX_ERROR */
                      ngx_alloc(sizeof(ngx_event_t *) * 2 * cycle->connection_n,    /* 容量 2*connection_n 个指针：同一连接的读/写事件可同时登记 */
                                cycle->log),    /* ngx_alloc 的日志参数：分配失败信息记录到此 log */
                      NGX_ERROR);    /* ngx_test_null 的失败返回值：分配失败时本函数返回 NGX_ERROR */

        if (event_index) {    /* reload 场景：旧表中还有已建立连接的事件指针 */
            ngx_memcpy(index, event_index, sizeof(ngx_event_t *) * nevents);    /* 把 nevents 个事件指针平移进新表，已登记连接的关系不丢 */
            ngx_free(event_index);    /* 释放旧表堆内存 */
        }    /* 旧表迁移分支结束 */
        event_index = index;    /* 全局指针切换到新表 */

#if 0     /* 未启用的备选就绪表代码 */
        if (ready_index) {    /* 旧就绪表存在则先释放 */
            ngx_free(ready_index);    /* 释放旧就绪表内存 */
        }    /* 就绪表释放分支结束 */
        ngx_test_null(ready_index,    /* 同样按最大连接数分配就绪事件表 */
                      ngx_alloc(sizeof(ngx_event_t *) * 2 * cycle->connection_n,    /* 同样按 2*connection_n 个指针容量分配就绪表 */
                      cycle->log),    /* 日志参数 */
                      NGX_ERROR);    /* 分配失败返回 NGX_ERROR */
#endif     /* 备选就绪表代码段结束 */
    }    /* 重新分配分支结束 */

    ngx_io = ngx_os_io;    /* 全局 IO 函数表(recv/send 等)指向平台实现，事件层与系统调用层解耦 */

    ngx_event_actions = ngx_select_module_ctx.actions;    /* 关键一步：把 select 动作集安装进全局事件接口，框架自此分派到本文件 */

    ngx_event_flags = NGX_USE_LEVEL_EVENT|NGX_USE_ONESHOT_EVENT;   /* 向框架声明能力：水平触发(就绪不处理则每轮重报)+oneshot(通知一次自动摘除，防重复处理) */

#if (WIN32)     /* Windows 分支 */
    max_read = max_write = 0;    /* 读/写集合登记计数清零 */
#else      /* 类 Unix 平台分支 */
    max_fd = -1;    /* 集合为空：-1 作哨兵值，select 第一参数将是 max_fd+1=0 */
#endif     /* 平台分支结束 */

    return NGX_OK;    /* 初始化成功 */
}/* ngx_select_init 结束 */


/*
 * ngx_select_done —— 事件模型销毁回调：释放事件索引表并把指针置空。
 */
static void ngx_select_done(ngx_cycle_t *cycle)    /* 事件模型销毁回调：进程退出时由框架调用 */
{    /* 清理逻辑开始 */
    ngx_free(event_index);    /* 归还索引表占用的堆内存 */
#if 0     /* 未启用的备选代码 */
    ngx_free(ready_index);    /* 释放就绪事件表 */
#endif     /* 备选代码段结束 */

    event_index = NULL;    /* 置空全局指针，下次 init 时据此重新分配并清零 fd_set 位图 */
}/* ngx_select_done 结束 */


/*
 * ngx_select_add_event —— 把一个读/写事件登记进 select 的主 fd_set 位图。
 * 用户态的 FD_SET 只是把 fd_set 中 fd/NFDBITS 单元里的对应位置 1，
 * 真正的内核监视要等下一轮 select() 把整张位图拷入内核后才开始；
 * 同时事件指针追加进 event_index，以便 select 返回后由登记序号反查事件。
 */
static ngx_int_t ngx_select_add_event(ngx_event_t *ev, int event, u_int flags)    /* 登记事件回调：框架开始关心某 fd 的读/写就绪时调用 */
{    /* 登记逻辑开始 */
    ngx_connection_t  *c;    /* 事件所属的连接对象，监视的 fd 从这里取 */

    c = ev->data;    /* 取出事件绑定的连接 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,    /* debug 日志：输出正在登记的 fd 与事件类型 */
                   "select add event fd:%d ev:%d", c->fd, event);    /* 格式串与参数：fd 号、读/写事件类型 */

    if (ev->index != NGX_INVALID_INDEX) {    /* index 不是非法值说明该事件早已登记，重复 add 属异常用法 */
        ngx_log_error(NGX_LOG_ALERT, ev->log, 0,    /* ALERT 级日志告警重复注册 */
                      "select event fd:%d ev:%d is already set", c->fd, event);    /* 输出重复事件的 fd 与类型 */
        return NGX_OK;    /* 直接返回，防止同一事件在位图与索引表中被记录两次 */
    }    /* 容量检查块结束 */

#if (WIN32)     /* Windows 专属登记分支 */

    if ((event == NGX_READ_EVENT) && (max_read >= FD_SETSIZE)    /* 读集合计数已达 winsock fd_set 的硬上限 FD_SETSIZE(默认1024) */
        || (event == NGX_WRITE_EVENT) && (max_write >= FD_SETSIZE))   /* 或写集合计数已达上限 */
    {    /* 任一集合将溢出 */
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,    /* ERR 级日志报告容量超限 */
                      "maximum number of descriptors "    /* 错误信息字符串拼接续行 */
                      "supported by select() is %d", FD_SETSIZE);    /* 说明 select 支持的最大描述符数 */
        return NGX_ERROR;    /* 拒绝登记；FD_SET 本不检查越界，不拦截会写坏位图之后的内存 */
    }    /* Windows 读/写置位分支结束 */

    if (event == NGX_READ_EVENT) {    /* 读事件 */
        FD_SET(c->fd, &master_read_fd_set);    /* 主读位图中该 fd 对应的 bit 置 1 */
        max_read++;    /* 读集合计数加一 */

    } else if (event == NGX_WRITE_EVENT) {    /* 写事件 */
        FD_SET(c->fd, &master_write_fd_set);   /* 主写位图中该 fd 对应的 bit 置 1 */
        max_write++;    /* 写集合计数加一 */
    }    /* Unix 读/写置位分支结束 */

#else      /* 类 Unix 登记分支 */

    if (event == NGX_READ_EVENT) {    /* 读事件 */
        FD_SET(c->fd, &master_read_fd_set);    /* 主读位图对应 bit 置 1 */

    } else if (event == NGX_WRITE_EVENT) {    /* 写事件 */
        FD_SET(c->fd, &master_write_fd_set);   /* 主写位图对应 bit 置 1 */
    }    /* 最大 fd 上界维护分支结束 */

    if (max_fd != -1 && max_fd < c->fd) {    /* 维护已登记的最大描述符号(-1 表示集合为空) */
        max_fd = c->fd;    /* 新 fd 更大则抬高上界，select 内核只需扫描 [0, max_fd] */
    }    /* Unix 读/写置位分支结束 */

#endif     /* 平台分支结束 */

    ev->active = 1;    /* 标记事件已激活，处于事件驱动框架的监视之中 */
    ev->oneshot = (u_char) ((flags & NGX_ONESHOT_EVENT) ? 1 : 0);   /* 按标志决定 oneshot 语义：触发一次后自动摘除，处理完须重新注册 */

    event_index[nevents] = ev;    /* 事件指针追加到索引表尾部，登记序号即当前 nevents */
    ev->index = nevents;    /* 事件记住自己的下标，del 时可 O(1) 定位，无需遍历 */
    nevents++;    /* 登记计数加一，select 监视规模扩大 */

    return NGX_OK;    /* 登记成功 */
}/* ngx_select_add_event 结束 */


/*
 * ngx_select_del_event —— 从主 fd_set 位图摘除事件，
 * 并用"尾部元素补位"法填平 event_index 中的空洞，保持索引表紧凑。
 */
static ngx_int_t ngx_select_del_event(ngx_event_t *ev, int event, u_int flags)    /* 摘除事件回调：框架不再关心该 fd 时调用 */
{    /* 摘除逻辑开始 */
    ngx_connection_t  *c;    /* 连接对象 */

    c = ev->data;    /* 取出事件绑定的连接 */

    ev->active = 0;    /* 先清激活标志：此后该事件不再处于监视状态 */

    if (ev->index == NGX_INVALID_INDEX) {    /* index 为非法值说明事件本就未登记 */
        return NGX_OK;    /* 无需删除，直接返回 */
    }    /* Windows 读/写清位分支结束 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,    /* debug 日志：输出被摘除的 fd 与事件类型 */
                   "select del event fd:%d ev:%d", c->fd, event);    /* 格式串与参数 */

#if (WIN32)     /* Windows 专属摘除分支 */

    if (event == NGX_READ_EVENT) {    /* 读事件 */
        FD_CLR(c->fd, &master_read_fd_set);    /* 清除主读位图中该 fd 的 bit */
        max_read--;    /* 读集合计数减一 */

    } else if (event == NGX_WRITE_EVENT) {    /* 写事件 */
        FD_CLR(c->fd, &master_write_fd_set);   /* 清除主写位图中该 fd 的 bit */
        max_write--;    /* 写集合计数减一 */
    }    /* Unix 读/写清位分支结束 */

#else      /* 类 Unix 摘除分支 */

    if (event == NGX_READ_EVENT) {    /* 读事件 */
        FD_CLR(c->fd, &master_read_fd_set);    /* 主读位图对应 bit 清零 */

    } else if (event == NGX_WRITE_EVENT) {    /* 写事件 */
        FD_CLR(c->fd, &master_write_fd_set);   /* 主写位图对应 bit 清零 */
    }    /* 上界失效分支结束 */

    if (max_fd == c->fd) {    /* 恰好删掉的是当前最大描述符 */
        max_fd = -1;    /* 上界失效：置 -1，下一轮 process_events 时重新全量扫描计算 */
    }    /* Unix 读/写清位分支结束 */

#endif     /* 平台分支结束 */

    if (ev->index < (u_int) --nevents) {    /* 先减一再比较：若删除的不是最后一个元素，索引表中会出现空洞 */
        event_index[ev->index] = event_index[nevents];    /* 把尾部最后一个事件指针搬到空洞处，O(1) 补位，维持数组紧凑 */
        event_index[ev->index]->index = ev->index;    /* 被搬动的事件须同步更新它记录的新下标 */
    }    /* 尾部补位分支结束 */

    ev->index = NGX_INVALID_INDEX;    /* 被删事件的下标标记为非法，表示未登记状态 */

    return NGX_OK;    /* 摘除成功 */
}/* ngx_select_del_event 结束 */


/*
 * ngx_select_process_events —— select 事件模型每一轮的事件处理主函数。
 * worker 进程主事件循环(ngx_process_events_and_timers)经 ngx_event_actions
 * 分派进入本函数。流程：确定 select 超时并先触发已到期定时器 -> 竞争 accept
 * 互斥锁 -> 把 master fd_set 拷贝到工作集合(select 会破坏性覆写位图) ->
 * select() 阻塞等待内核就绪 -> 校正缓存时钟 -> O(n) 扫描 event_index 并用
 * FD_ISSET 找出就绪事件分发。select 全程三次 O(n)：位图两轮用户态<->内核态
 * 全量拷贝、内核对 [0,max_fd] 线性扫描、返回后用户态再线性扫描，且集合每轮
 * 必须重建——这正是 epoll(红黑树+就绪链表+回调)被发明的动机。
 */
static ngx_int_t ngx_select_process_events(ngx_cycle_t *cycle)    /* 事件处理主回调：阻塞等待内核就绪通知并分发事件 */
{    /* 事件处理逻辑开始 */
    int                       ready, nready;    /* ready=select 返回值(-1出错/0超时/>0就绪fd个数)；nready=实际分发计数，用于与内核报告数对账 */
    ngx_uint_t                i, found, lock, expire;   /* i 循环下标；found 本事件是否就绪；lock 是否仍持有 posted 事件互斥锁；expire 是否允许触发到期定时器 */
    ngx_err_t                 err;    /* 保存 select 出错时的 errno(典型如 EINTR：被信号打断) */
    ngx_msec_t                timer;  /* 距最近一个定时器到期的毫秒数，作为 select 的超时值 */
    ngx_event_t              *ev;     /* 当前遍历到的事件 */
    ngx_connection_t         *c;      /* 事件所属连接 */
    ngx_epoch_msec_t          delta;  /* 本轮 select 实际阻塞的毫秒数，用于推进定时器过期 */
    struct timeval            tv, *tp;   /* select 超时参数：timeval 为秒+微秒两级精度；tp 指向它，或为 NULL 表示无限期阻塞 */
#if (HAVE_SELECT_CHANGE_TIMEOUT)   /* 特性分支：该平台 select 会把 tv 改写为剩余时间 */
    static ngx_epoch_msec_t   deltas = 0;   /* 部分系统 select 返回时把 tv 改写为剩余时间，用它累计由此省下的时钟误差 */
#endif     /* 特性声明结束 */

    for ( ;; ) {    /* 循环直到没有"已到期但未触发"的定时器 */
        timer = ngx_event_find_timer();   /* 查询最快到期定时器的剩余毫秒数 */

        if (timer != 0) {    /* 最近定时器尚未到期(剩余>0) */
            break;    /* 可以让 select 以 timer 为超时安心阻塞了 */
        }    /* 未到期则跳出，转去 select 阻塞 */

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：存在已到期定时器 */
                       "select expired timer");    /* 日志文本 */

        ngx_event_expire_timers((ngx_msec_t)    /* 触发所有到期定时器的回调；若 timer==0 就进 select 会造成 0 超时空转 */
                                    (ngx_elapsed_msec - ngx_old_elapsed_msec));   /* 用两轮之间流逝的毫秒数驱动过期判定 */
    }    /* 到期定时器清理循环结束 */

    ngx_old_elapsed_msec = ngx_elapsed_msec;    /* 记录本轮处理起点毫秒数，供下轮差值计算 */

    expire = 1;    /* 默认允许在 select 返回后按实际阻塞时长触发定时器 */

#if !(WIN32)      /* 非 Windows：accept 互斥锁与 max_fd 维护 */

    if (ngx_accept_mutex) {    /* 已启用多 worker 抢占监听 socket 的 accept 互斥锁 */
        if (ngx_accept_disabled > 0) {    /* 本 worker 连接数超载(超过均值的 7/8)，负载均衡计数未耗尽 */
            ngx_accept_disabled--;    /* 本轮放弃抢锁，把 accept 机会让给其他 worker */

        } else {    /* 未超载，尝试抢锁 */
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) {   /* 非阻塞 trylock：抢到则监听 fd 登记进本进程事件集 */
                return NGX_ERROR;    /* 抢锁过程出错则终止本轮 */
            }    /* 记录更大 fd 分支结束 */

            if (ngx_accept_mutex_held == 0    /* 没有抢到锁 */
                && (timer == NGX_TIMER_INFINITE    /* if 条件续行：剩余等待为无限或偏大 */
                    || timer > ngx_accept_mutex_delay))   /* 且剩余等待很久(或无限) */
            {                      /* 没锁就不能长期脱离竞争，否则连接会被其他 worker 垄断 */
                timer = ngx_accept_mutex_delay;   /* 把 select 超时压到 accept_mutex_delay(默认500ms)，届时再抢 */
                expire = 0;    /* 提前醒来是为抢锁而非定时器到期，禁止误触发定时器 */
            }    /* 压缩 select 超时分支结束 */
        }    /* 抢锁/让渡分支结束 */
    }    /* accept 互斥锁处理块结束 */

    if (max_fd == -1) {    /* 最大描述符上界失效(上轮被删或首次进入) */
        for (i = 0; i < nevents; i++) {    /* 线性扫描全部已登记事件 */
            c = event_index[i]->data;    /* 取事件对应的连接 */
            if (max_fd < c->fd) {    /* 求最大 fd */
                max_fd = c->fd;    /* 更新上界 */
            }    /* 更新上界分支结束 */
        }    /* max_fd 重算扫描结束 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印重算出的上界 */
                       "change max_fd: %d", max_fd);    /* 日志参数 */
    }    /* 上界重算块结束 */

#endif     /* 非 Windows 平台段结束 */

#if (NGX_DEBUG)      /* 调试诊断段：仅 NGX_DEBUG 编译时存在 */
    if (cycle->log->log_level & NGX_LOG_DEBUG_ALL) {   /* 仅当开启 debug 级日志才做诊断输出 */
        for (i = 0; i < nevents; i++) {    /* 遍历全部登记事件 */
            ev = event_index[i];    /* 取事件 */
            c = ev->data;    /* 取连接 */
            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* 打印每个被 select 监视的 fd 及其读写属性 */
                           "select event: fd:%d wr:%d", c->fd, ev->write);    /* 参数：fd 号、是否写事件 */
        }    /* 诊断遍历结束 */

#if !(WIN32)      /* 嵌套平台分支：仅 Unix 打印 max_fd */
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印本轮 select 的描述符上界 */
                       "max_fd: %d", max_fd);    /* 日志参数 */
#endif     /* 嵌套平台分支结束 */
    }    /* 诊断输出块结束 */
#endif     /* 调试诊断段结束 */

    if (timer == NGX_TIMER_INFINITE) {    /* 没有任何定时器在等待 */
        tp = NULL;    /* select 的 timeout 传 NULL：无限期阻塞，直到有 fd 就绪或被信号(EINTR)打断 */
        expire = 0;    /* 无超时则不存在"到期"一说 */

    } else {    /* 有定时器等待 */
        tv.tv_sec = timer / 1000;    /* 毫秒换算为 timeval 的秒部分 */
        tv.tv_usec = (timer % 1000) * 1000;   /* 余数换算为微秒；select 超时精度即微秒级 timeval，进程将在此时间内睡眠 */
        tp = &tv;    /* select 在 Linux 上返回时 tv 被改写为剩余时间(不可移植行为，见后 HAVE_SELECT_CHANGE_TIMEOUT) */
    }    /* select 超时参数构造结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印本轮超时值 */
                   "select timer: %d", timer);    /* 日志参数 */

    work_read_fd_set = master_read_fd_set;    /* 关键：整块拷贝主读位图到工作集合——select 返回会破坏性覆写位图(未就绪位被清零)，"每轮必须重建集合"是 select 的固有开销 */
    work_write_fd_set = master_write_fd_set;  /* 同样整块拷贝主写位图 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：以十六进制打印读位图内容 */
                   "select read fd_set: %08X", *(int *) &work_read_fd_set);   /* 把位图首 32 位强转整数输出 */

#if (WIN32)     /* Windows：第一参数被忽略，传 0 */
    ready = select(0, &work_read_fd_set, &work_write_fd_set, NULL, tp);   /* winsock 忽略第一参数(其 fd_set 内部自带容量计数) */
#else      /* 类 Unix：第一参数传 max_fd+1 */
    /* 核心：整组 fd_set 拷入内核，内核把进程挂到 [0,max_fd] 每个 fd 的等待队列后睡眠；就绪/超时唤醒后再 O(n) 扫描置位并返回就绪个数 */
    ready = select(max_fd + 1, &work_read_fd_set, &work_write_fd_set, NULL, tp);
#endif     /* 平台分支结束 */

    if (ready == -1) {    /* 返回 -1 表示出错(区别于 0：超时无就绪) */
        err = ngx_socket_errno;    /* 立即取出 errno(常见 EINTR：进程被 SIGCHLD 等信号打断) */
    } else {    /* >=0：0 为超时，>0 为就绪 fd 个数 */
        err = 0;    /* 无错误 */
    }    /* errno 提取结束 */

#if (HAVE_SELECT_CHANGE_TIMEOUT)   /* 分支一：tv 会被改写为剩余时间的平台 */

    if (timer != NGX_TIMER_INFINITE) {    /* 带超时进入：这类系统返回时把 tv 改写为"剩余时间" */
        delta = timer - (tv.tv_sec * 1000 + tv.tv_usec / 1000);   /* 设定值减剩余值=实际阻塞毫秒数，省一次 gettimeofday 系统调用 */

        /*
         * learn the real time and update the cached time
         * if the sum of the last deltas overcomes 1 second
         */

        deltas += delta;    /* 累计自上次向内核取真实时间以来的阻塞毫秒数 */
        if (deltas > 1000) {    /* 累计误差超过 1 秒，必须重新同步真实时钟 */
            ngx_gettimeofday(&tv);    /* gettimeofday 系统调用陷入内核取墙上时钟 */
            ngx_time_update(tv.tv_sec);    /* 更新 nginx 缓存的秒级时间(供日志、时间字符串使用) */
            deltas = tv.tv_usec / 1000;    /* 保留不足 1 秒的毫秒零头继续累计 */

            ngx_elapsed_msec = (ngx_epoch_msec_t) tv.tv_sec * 1000    /* 以真实时钟重算进程启动至今的毫秒数 */
                                          + tv.tv_usec / 1000 - ngx_start_msec;    /* 纪元毫秒减启动基准 */
        } else {    /* 累计误差不足 1 秒 */
            ngx_elapsed_msec += delta;    /* 直接用 select 阻塞时长推进缓存时钟，避免每次都做系统调用 */
        }    /* 时钟重同步分支结束 */

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印设定超时与实际阻塞时长 */
                       "select timer: %d, delta: %d", timer, (int) delta);    /* 日志参数 */

    } else {    /* 无限阻塞的 select */
        delta = 0;    /* 无超时语义，阻塞时长不参与定时器计算 */
        ngx_gettimeofday(&tv);    /* 返回后取真实时间 */
        ngx_time_update(tv.tv_sec);    /* 更新缓存秒级时间 */

        ngx_elapsed_msec = (ngx_epoch_msec_t) tv.tv_sec * 1000    /* 重算进程运行毫秒数 */
                                          + tv.tv_usec / 1000 - ngx_start_msec;    /* 同上 */

        if (ready == 0) {    /* 无限等待却返回 0 个就绪：逻辑上不可能，多半是时钟/实现异常 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,    /* ALERT 级告警 */
                          "select() returned no events without timeout");    /* 说明异常现象 */
            ngx_accept_mutex_unlock();    /* 先释放抢到的 accept 锁再退出 */
            return NGX_ERROR;    /* 视为致命错误 */
        }    /* 无就绪异常检查结束 */
    }    /* 无限阻塞分支结束 */

#else /* !(HAVE_SELECT_CHANGE_TIMEOUT) */  /* 分支二：tv 不被改写，需主动 gettimeofday */

    ngx_gettimeofday(&tv);    /* 该平台 select 不改写 tv，只能主动 gettimeofday 获取真实时间 */
    ngx_time_update(tv.tv_sec);    /* 更新缓存时间 */

    delta = ngx_elapsed_msec;    /* 记录 select 前的缓存毫秒数 */
    ngx_elapsed_msec = (ngx_epoch_msec_t) tv.tv_sec * 1000    /* 用真实时钟刷新缓存时钟 */
                                          + tv.tv_usec / 1000 - ngx_start_msec;    /* 纪元毫秒减启动基准 */

    if (timer != NGX_TIMER_INFINITE) {    /* 带超时进入 */
        delta = ngx_elapsed_msec - delta;    /* 两次缓存时钟之差=本轮实际阻塞的毫秒数 */

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印设定与实际阻塞时长 */
                       "select timer: %d, delta: %d", timer, (int) delta);    /* 日志参数 */

    } else {    /* 无限阻塞 */
        if (ready == 0) {    /* 同前：无限等待不应返回 0 个就绪 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,    /* ALERT 告警 */
                          "select() returned no events without timeout");    /* 日志文本 */
            ngx_accept_mutex_unlock();    /* 释放 accept 锁 */
            return NGX_ERROR;    /* 出错返回 */
        }    /* 无就绪异常检查结束 */
    }    /* 无限阻塞分支结束 */

#endif /* HAVE_SELECT_CHANGE_TIMEOUT */  /* 时钟校正分支结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印内核报告的就绪 fd 总数 */
                   "select ready %d", ready);    /* 日志参数 */

    if (err) {    /* select 调用出错 */
#if (WIN32)     /* Windows：错误一律记 ALERT */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, err, "select() failed");   /* Windows：一律 ALERT 级 */
#else      /* 类 Unix：区分 EINTR 与真错误 */
        ngx_log_error((err == NGX_EINTR) ? NGX_LOG_INFO : NGX_LOG_ALERT,    /* Unix：EINTR 是信号打断的正常现象(如 reload/SIGCHLD)降为 INFO，其余真错误记 ALERT */
                      cycle->log, err, "select() failed");    /* 输出 errno 与错误文本 */
#endif     /* 平台分支结束 */
        ngx_accept_mutex_unlock();    /* 释放 accept 互斥锁 */
        return NGX_ERROR;    /* 本轮处理失败(即便 EINTR 也不在此重试，返回框架层处理) */
    }    /* select 错误处理块结束 */


    if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {   /* 加锁保护 posted 事件队列(多线程模式下事件投递与消费互斥) */
        ngx_accept_mutex_unlock();    /* 加锁失败先释放 accept 锁 */
        return NGX_ERROR;    /* 返回错误 */
    }    /* posted 锁加锁失败处理结束 */

    lock = 1;    /* 标记 posted 互斥锁现由本函数持有，最终统一释放 */
    nready = 0;    /* 实际分发的就绪事件计数清零 */

    for (i = 0; i < nevents; i++) {    /* select 的第三次 O(n)：用户态全量扫描登记表逐个 FD_ISSET 找就绪事件(epoll 只需遍历内核返回的就绪链表) */
        ev = event_index[i];    /* 取第 i 个登记事件 */
        c = ev->data;    /* 取其连接 */
        found = 0;    /* 先假设未就绪 */

        if (ev->write) {    /* 该事件登记在写集合 */
            if (FD_ISSET(c->fd, &work_write_fd_set)) {    /* 用户态测试位图对应 bit：内核已把就绪 fd 置 1(未就绪的在返回位图中被清零) */
                found = 1;    /* 写就绪：内核发送缓冲区已有空位，继续 write 不会阻塞 */
                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印写就绪的 fd */
                               "select write %d", c->fd);    /* 日志参数 */
            }    /* 写位图就绪检查结束 */

        } else {    /* 该事件登记在读集合 */
            if (FD_ISSET(c->fd, &work_read_fd_set)) {    /* 测试读位图对应 bit */
                found = 1;    /* 读就绪：监听 socket 意味全连接队列非空(三次握手完成)，普通 socket 意味内核接收缓冲区有数据 */
                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印读就绪的 fd */
                               "select read %d", c->fd);    /* 日志参数 */
            }    /* 读位图就绪检查结束 */
        }    /* 读/写分类检查结束 */

        if (found) {    /* 该事件确实就绪 */
            ev->ready = 1;    /* 置就绪标志，事件处理器据此执行 I/O */

            if (ev->oneshot) {    /* oneshot 语义：只通知一次 */
                if (ev->timer_set) {    /* 若登记时挂了超时定时器 */
                    ngx_del_timer(ev);    /* 先摘除定时器 */
                }    /* 定时器摘除分支结束 */

                if (ev->write) {    /* 写事件 */
                    ngx_select_del_event(ev, NGX_WRITE_EVENT, 0);    /* 从主写位图与索引表摘除，防止下轮重复通知(需处理完重新注册) */
                } else {    /* 读事件 */
                    ngx_select_del_event(ev, NGX_READ_EVENT, 0);    /* 从主读位图摘除 */
                }    /* oneshot 摘除分支结束 */
            }    /* oneshot 处理块结束 */

            if (ev->accept) {    /* 监听 socket 的 accept 事件必须延迟到持锁后处理 */
                ev->next = accept_events;    /* 头插法挂入 accept 事件链表 */
                accept_events = ev;    /* 更新链表头 */
            } else {    /* 普通连接上的事件 */
                ngx_post_event(ev);    /* 投递到 posted 事件队列，稍后统一执行回调(把"发现事件"与"处理事件"解耦成两阶段) */
            }    /* accept 延迟/事件投递分支结束 */

            nready++;    /* 分发计数加一 */

#if 0      /* 备选实现段(未启用) */
            ready_index[nready++] = ev;    /* 备选实现：直接记入就绪数组而非置标志 */
#endif     /* 备选实现段结束 */
        }    /* 就绪事件分发块结束 */
    }    /* event_index 全量扫描循环结束 */

#if 0      /* 备选实现段(未启用) */
    for (i = 0; i < nready; i++) {    /* 备选实现：遍历就绪数组 */
        ev = ready_index[i];    /* 取就绪事件 */
        ready--;    /* 对账：内核报告数递减 */

        if (!ev->active) {    /* 事件在处理中已被摘除则跳过 */
            continue;    /* 跳过处理中已失效的事件 */
        }    /* 事件有效性检查结束 */

        ev->ready = 1;    /* 置就绪标志 */

        if (ev->oneshot) {    /* oneshot 事件 */
            if (ev->timer_set) {    /* 挂有定时器 */
                ngx_del_timer(ev);    /* 摘除定时器 */
            }    /* 定时器摘除分支结束 */

            if (ev->write) {    /* 写事件 */
                ngx_select_del_event(ev, NGX_WRITE_EVENT, 0);    /* 摘除写登记 */
            } else {    /* 读事件 */
                ngx_select_del_event(ev, NGX_READ_EVENT, 0);    /* 摘除读登记 */
            }    /* oneshot 摘除分支结束 */
        }    /* oneshot 处理块结束 */

        ev->event_handler(ev);    /* 直接调用事件处理器 */
    }    /* 就绪数组遍历结束 */
#endif     /* 备选实现段结束 */

    ev = accept_events;    /* 取出本轮收集到的 accept 事件链表头 */

    for ( ;; ) {    /* 逐个处理 accept 事件(处理监听 socket 必须持有 accept 互斥锁) */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug：打印待处理的 accept 事件指针 */
                      "accept event " PTR_FMT, ev);    /* 格式串与参数 */

        if (ev == NULL) {    /* 链表已走完 */
            break;    /* 退出循环 */
        }    /* 链表结束分支 */

        ngx_mutex_unlock(ngx_posted_events_mutex);    /* 执行 accept 前先放开 posted 锁：accept 可能耗时较长，避免持锁阻塞其他线程 */

        ev->event_handler(ev);    /* 调用监听事件处理器 ngx_event_accept：循环 accept 从内核全连接队列取走连接 */

        if (ngx_accept_disabled > 0) {    /* accept 过程中发现本 worker 连接数已超载 */
            lock = 0;    /* 此刻 posted 锁已释放，清掉持有标记 */
            break;    /* 停止继续 accept，把机会让给其他 worker */
        }    /* 超载让渡分支结束 */

        ev = ev->next;    /* 前进到下一个 accept 事件 */

        if (ev == NULL) {    /* 后面没有事件了 */
            lock = 0;    /* posted 锁已释放，修正持有标记 */
            break;    /* 退出 */
        }    /* 链表结束分支 */

        if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {   /* 还有后续 accept 事件，与上轮对称地重新加锁 */
            ngx_accept_mutex_unlock();    /* 失败则先释放 accept 锁 */
            return NGX_ERROR;    /* 返回错误 */
        }    /* 重新加锁失败处理结束 */
    }    /* accept 事件处理循环结束 */

    ngx_accept_mutex_unlock();    /* accept 阶段结束，释放 accept 互斥锁，允许其他 worker 竞争监听 */
    accept_events = NULL;    /* 清空 accept 链表头，下一轮重新收集 */

    if (lock) {    /* 若中途未异常退出且仍持有 posted 锁 */
        ngx_mutex_unlock(ngx_posted_events_mutex);    /* 释放它 */
    }    /* 条件释放 posted 锁结束 */

    if (ready != nready) {    /* 内核报告的就绪数与实际找到的不一致：可能有 fd 已被关闭或状态异常 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0, "select ready != events");   /* ALERT 告警提示事件系统失配 */
    }    /* 就绪数对账块结束 */

    if (expire && delta) {    /* 允许处理定时器且本轮确实阻塞了一段时间 */
        ngx_event_expire_timers((ngx_msec_t) delta);    /* 按 select 实际阻塞时长触发所有到期定时器回调 */
    }    /* 定时器触发块结束 */

    if (!ngx_threaded) {    /* 非线程模式：posted 事件由本进程直接执行 */
        ngx_event_process_posted(cycle);    /* 逐个取出 posted 队列中的事件并调用其处理器 */
    }    /* posted 事件处理块结束 */

    return NGX_OK;    /* 本轮事件处理完成 */
}/* ngx_select_process_events 结束 */


/*
 * ngx_select_init_conf —— select 模块的配置初始化/校验回调。
 * 仅当 nginx.conf 指定 use select 时才真正校验；核心检查是 connections
 * 不得超过 FD_SETSIZE(默认1024)：fd_set 是定长位图，超出上限的 fd 根本
 * 放不进集合，强行 FD_SET 会越界写坏内存。
 */
static char *ngx_select_init_conf(ngx_cycle_t *cycle, void *conf)    /* 配置校验回调：配置解析完成后由框架调用 */
{    /* 校验逻辑开始 */
    ngx_event_conf_t  *ecf;    /* 事件核心模块配置(含 connections 数) */

    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);   /* 从配置上下文取出事件核心模块的配置 */

    if (ecf->use != ngx_select_module.ctx_index) {    /* 配置指定的事件模型不是 select */
        return NGX_CONF_OK;    /* 与本模块无关，直接放行 */
    }    /* 非本模块分支结束 */

    /* disable warning: the default FD_SETSIZE is 1024U in FreeBSD 5.x */
    /* 以上原注释：屏蔽 FreeBSD 5.x 中 FD_SETSIZE 默认为 1024U 的告警 */

#if !(WIN32)     /* 非 Windows 平台校验 */
    if ((unsigned) ecf->connections > FD_SETSIZE) {    /* 配置的 connections 超出 fd_set 位图容量上限 */
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,    /* EMERG 级日志 */
                      "the maximum number of files "    /* 错误信息字符串拼接续行 */
                      "supported by select() is " ngx_value(FD_SETSIZE));   /* 报告 select 支持的最大文件数 */
        return NGX_CONF_ERROR;    /* 配置非法，nginx 拒绝启动 */
    }    /* FD_SETSIZE 容量检查块结束 */
#endif     /* 非 Windows 校验段结束 */

#if (NGX_THREADS)      /* 多线程模式检查分支 */
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,    /* EMERG 级日志 */
                  "select() is not supported in the threaded mode");    /* 多线程模式不支持 select(多线程共享维护 fd_set 位图无法保证一致性) */
    return NGX_CONF_ERROR;    /* 拒绝启动 */
#else      /* 单线程模式：配置合法 */
    return NGX_CONF_OK;    /* 单线程模式下配置合法 */
#endif     /* 线程模式分支结束 */
}/* ngx_select_init_conf 结束 */
