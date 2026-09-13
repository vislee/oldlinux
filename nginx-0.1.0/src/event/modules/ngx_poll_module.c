
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>     /* 编译期全局配置头：平台判定与各系统调用可用性宏在此集中定义，决定本文件条件编译分支的取舍 */
#include <ngx_core.h>       /* nginx 核心头：ngx_int_t/ngx_str_t 等基本类型、ngx_alloc/ngx_free 内存封装与日志基础设施 */
#include <ngx_event.h>      /* 事件框架头：ngx_event_t/ngx_connection_t/ngx_event_actions 等抽象接口，本文件是其中 poll 后端的具体实现 */


static ngx_int_t ngx_poll_init(ngx_cycle_t *cycle);    /* 初始化回调：按最大连接数分配/扩容 pollfd 数组并安装 poll 动作集 */
static void ngx_poll_done(ngx_cycle_t *cycle);         /* 销毁回调：释放 pollfd 数组 */
static ngx_int_t ngx_poll_add_event(ngx_event_t *ev, int event, u_int flags);   /* 注册事件：{fd,events} 追加进数组尾部或按位或进同 fd 已有表项 */
static ngx_int_t ngx_poll_del_event(ngx_event_t *ev, int event, u_int flags);   /* 注销事件：按位清除事件位，或整项移除并用尾部表项交换填补 */
static ngx_int_t ngx_poll_process_events(ngx_cycle_t *cycle);   /* 事件处理主入口：阻塞在 poll() 上等内核回填 revents，再 O(n) 扫描分发就绪事件 */
static char *ngx_poll_init_conf(ngx_cycle_t *cycle, void *conf);   /* 配置校验回调：被 use 选中时禁止运行在线程模式 */


static struct pollfd  *event_list;    /* pollfd 数组：每元素 {fd, events, revents}，是用户态持久维护的监视清单，每轮 poll() 整体拷入内核；与 select 的 fd_set 位图不同，无 FD_SETSIZE=1024 硬上限、可按连接数动态扩容 */
static int             nevents;       /* 有效表项数：内核按 [0,nevents) 逐项查询，返回后的就绪扫描也是 O(nevents) */

#if 0     /* 被编译开关关闭的备选实现代码，不参与编译 */
static ngx_event_t   **ready_index;    /* 备选：独立就绪事件数组，可把"扫描找就绪"变成"按序取就绪"(未启用) */
#endif     /* 备选声明代码段结束 */

static ngx_event_t    *accept_events;    /* accept 事件链表头：本轮就绪的监听 socket(全连接队列非空)事件串成单链表，延迟到统一持 accept 互斥锁后处理，多 worker 间防惊群 */


static ngx_str_t    poll_name = ngx_string("poll");   /* 事件模块名，nginx.conf 的 use 指令按此名字选择 poll 事件模型 */

ngx_event_module_t  ngx_poll_module_ctx = {    /* poll 事件模块上下文：向事件框架声明本后端的全部能力 */
    &poll_name,    /* 模块名字符串指针 */
    NULL,                                  /* create configuration */     /* 创建配置回调：poll 无私有配置指令，置空 */
    ngx_poll_init_conf,                    /* init configuration */       /* 配置初始化/校验回调 */

    {    /* actions 动作集：事件框架与具体 IO 多路复用实现之间的虚函数表 */
        ngx_poll_add_event,                /* add an event */             /* 注册事件：写入 event_list 数组 */
        ngx_poll_del_event,                /* delete an event */          /* 注销事件：清位或整项删除 */
        ngx_poll_add_event,                /* enable an event */          /* 启用事件：poll 的 events 位本就增量式，enable 等价于 add */
        ngx_poll_del_event,                /* disable an event */         /* 禁用事件：等价于 del */
        NULL,                              /* add an connection */        /* 连接级独立注册：poll 以事件为登记单位，无需额外动作 */
        NULL,                              /* delete an connection */     /* 同上 */
        NULL,                              /* process the changes */      /* 增量变更集接口(epoll 风格)：poll 每轮全量提交数组，用不到 */
        ngx_poll_process_events,           /* process the events */       /* 事件处理主函数：worker 事件循环每轮经 ngx_process_events 调到这里 */
        ngx_poll_init,                     /* init the events */          /* 事件模型初始化回调 */
        ngx_poll_done                      /* done the events */          /* 事件模型销毁回调 */
    }    /* actions 动作集初始化列表结束 */

};     /* poll 事件模块上下文定义结束 */

ngx_module_t  ngx_poll_module = {    /* nginx 通用模块结构：所有模块以统一形式注册进核心，供配置解析与初始化排序使用 */
    NGX_MODULE,                        /* 版本签名宏：编译期校验模块与核心的二进制兼容性 */
    &ngx_poll_module_ctx,                  /* module context */     /* 指向上面 poll 事件模块上下文 */
    NULL,                                  /* module directives */  /* 本模块无配置指令表 */
    NGX_EVENT_MODULE,                      /* module type */        /* 类型标记：事件模块，参与事件模块 ctx_index 编号分配 */
    NULL,                                  /* init module */        /* 无全局(跨进程)初始化钩子 */
    NULL                                   /* init process */       /* 无每进程初始化钩子 */
};     /* poll 通用模块描述结构定义结束 */



/* ngx_poll_init —— poll 事件模型初始化回调。
 * 首次进入时清零已登记表项数；worker 启动或 reload 后最大连接数变大时，
 * 按 cycle->connection_n 重新分配 event_list 并迁移旧表项(poll 数组没有
 * FD_SETSIZE 这类固定容量，大小只需跟随配置的最大连接数)；
 * 最后把本文件动作集安装进全局 ngx_event_actions。
 * 标志位声明水平触发(level-triggered)+oneshot 能力。 */
static ngx_int_t ngx_poll_init(ngx_cycle_t *cycle)    /* 事件模型初始化回调：框架在启动/reload 时调用 */
{    /* 初始化逻辑开始 */
    struct pollfd   *list;    /* 新数组暂存指针：先分配成功再切换全局变量，失败不破坏旧表 */

    if (event_list == NULL) {    /* 首次初始化：数组尚不存在，登记计数不可信 */
        nevents = 0;    /* 有效表项数归零，从空监视清单开始 */
    }    /* 首次初始化分支结束 */

    if (ngx_process == NGX_PROCESS_WORKER    /* 只有 worker 进程真正跑事件循环，需要维护监视数组 */
        || cycle->old_cycle == NULL    /* 无旧周期对象(启动/reload 首轮)时必须全新分配 */
        || cycle->old_cycle->connection_n < cycle->connection_n)   /* reload 后连接数上限变大，旧数组容量不够 */
    {    /* 满足任一条件即需要(重新)分配 pollfd 数组 */
        ngx_test_null(list,    /* 分配包装宏：失败时记日志并令函数返回 NGX_ERROR */
                      ngx_alloc(sizeof(struct pollfd) * cycle->connection_n,    /* 容量 connection_n 个 pollfd：同一连接读/写事件共用一表项(events 按位或)，不像 select 索引表需 2 倍槽位 */
                                cycle->log),    /* ngx_alloc 的日志参数：分配失败信息记录到此 log */
                      NGX_ERROR);    /* ngx_test_null 的失败返回值：分配失败时本函数返回 NGX_ERROR */

        if (event_list) {    /* reload 场景：旧数组里还有存活连接的登记表项 */
            ngx_memcpy(list, event_list, sizeof(ngx_event_t *) * nevents);    /* 平移 nevents 个旧表项，保住"下标<->事件"对应关系(按指针大小拷贝与 pollfd 同宽，属旧代码取巧写法) */
            ngx_free(event_list);    /* 释放旧数组堆内存 */
        }    /* 旧表迁移分支结束 */

        event_list = list;    /* 全局指针切换到新数组 */

#if 0     /* 未启用的备选就绪表代码 */
        if (ready_index) {    /* 旧就绪表存在则先释放 */
            ngx_free(ready_index);    /* 释放旧就绪表内存 */
        }    /* 就绪表释放分支结束 */

        ngx_test_null(ready_index,    /* 同样按最大连接数分配备选就绪事件表 */
                      ngx_alloc(sizeof(ngx_event_t *) * 2 * cycle->connection_n,    /* 容量 2*connection_n 个指针：读/写事件各占一槽 */
                                cycle->log),    /* 日志参数 */
                      NGX_ERROR);    /* 分配失败返回 NGX_ERROR */
#endif     /* 备选就绪表代码段结束 */
    }    /* 重新分配分支结束 */

    ngx_io = ngx_os_io;    /* 全局 IO 函数表(recv/send 等)指向平台实现，事件层与系统调用层解耦 */

    ngx_event_actions = ngx_poll_module_ctx.actions;    /* 关键一步：把 poll 动作集安装进全局事件接口，框架自此分派到本文件 */

    ngx_event_flags = NGX_USE_LEVEL_EVENT|NGX_USE_ONESHOT_EVENT;   /* 向框架声明能力：水平触发(就绪不处理每轮重报)+oneshot(通知一次自动摘除，防重复处理) */

    return NGX_OK;    /* 初始化成功 */
}    /* ngx_poll_init 结束 */


/* ngx_poll_done —— 事件模型销毁回调：释放 pollfd 数组并把指针置空。 */
static void ngx_poll_done(ngx_cycle_t *cycle)    /* 事件模型销毁回调：进程退出时由框架调用 */
{    /* 清理逻辑开始 */
    ngx_free(event_list);    /* 归还 pollfd 数组占用的堆内存 */
#if 0     /* 未启用的备选代码 */
    ngx_free(ready_index);    /* 释放备选就绪表 */
#endif     /* 备选代码段结束 */

    event_list = NULL;    /* 指针置空：避免悬挂指针被下次 init 误当作旧表 */
#if 0     /* 未启用的备选代码 */
    ready_index = NULL;    /* 备选就绪表指针同样置空 */
#endif     /* 备选代码段结束 */
}    /* ngx_poll_done 结束 */


/* ngx_poll_add_event —— 注册一个读或写事件。
 * 与 select 每轮重建位图不同，poll 在用户态持久维护 event_list：
 * 反方向未登记时把新表项 {fd, events} 追加到数组尾部；反方向已登记
 * (同一 fd 只占一个表项)时仅把新事件位按位或进该表项。
 * ev->index 保存事件在数组中的下标，注销与 oneshot 摘除都靠它定位。 */
static ngx_int_t ngx_poll_add_event(ngx_event_t *ev, int event, u_int flags)    /* 注册事件回调：event 为框架抽象的读/写事件类型 */
{    /* 注册逻辑开始 */
    ngx_event_t       *e;    /* 同一连接反方向的事件指针，用于判断该 fd 是否已有表项 */
    ngx_connection_t  *c;    /* 事件所属连接：从中取 fd 并访问读/写事件对 */

    c = ev->data;    /* 事件建立时绑定的连接对象 */

    ev->active = 1;    /* 标记事件已激活(登记进事件模型)，框架据此防止重复添加 */

    if (ev->index != NGX_INVALID_INDEX) {    /* index 非哨兵值说明该事件已在数组中 */
        ngx_log_error(NGX_LOG_ALERT, ev->log, 0,    /* ALERT 级日志：重复注册属逻辑异常 */
                      "poll event fd:%d ev:%d is already set", c->fd, event);   /* 报出 fd 与事件类型便于定位 */
        return NGX_OK;    /* 幂等处理：视为已成功，不重复写数组 */
    }    /* 防重分支结束 */

    if (event == NGX_READ_EVENT) {    /* 把框架抽象事件位映射为 poll 事件位 */
        e = c->write;    /* 读事件的反方向是同连接的写事件 */
#if (NGX_READ_EVENT != POLLIN)     /* 仅当框架读事件值与 POLLIN 不一致时才需转换 */
        event = POLLIN;    /* 转为 poll 的可读位：内核接收缓冲区有数据(监听 socket 则为全连接队列非空) */
#endif     /* 事件位转换条件编译结束 */

    } else {    /* 写事件分支 */
        e = c->read;    /* 写事件的反方向是读事件 */
#if (NGX_WRITE_EVENT != POLLOUT)     /* 仅当框架写事件值与 POLLOUT 不一致时转换 */
        event = POLLOUT;    /* 转为 poll 的可写位：内核发送缓冲区有空位可写 */
#endif     /* 事件位转换条件编译结束 */
    }    /* 事件位映射结束 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,    /* debug 级日志(调试日志开启才输出) */
                   "poll add event: fd:%d ev:%d", c->fd, event);   /* 记录被注册的 fd 与 poll 事件位 */

    if (e == NULL || e->index == NGX_INVALID_INDEX) {    /* 反方向不存在或未登记：本 fd 在数组中还没有表项 */
        event_list[nevents].fd = c->fd;    /* 新表项填入描述符，poll() 时随整个数组拷入内核逐项查询 */
        event_list[nevents].events = event;    /* events 是"请求监视"位图，与内核回填的 revents 分离——不必像 select 那样每轮重建集合 */
        event_list[nevents].revents = 0;    /* revents 由内核在 poll 返回时回填，登记时清零 */

        ev->index = nevents;    /* 事件记下自己的数组下标，注销/oneshot 时按此定位 */
        nevents++;    /* 有效表项数加一：下一轮内核多查一项，O(n) 的 n 随之增长 */

    } else {    /* 反方向已登记：复用同一表项 */
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,    /* debug 日志 */
                       "poll add index: %d", e->index);   /* 记录被复用的表项下标 */

        event_list[e->index].events |= event;    /* 仅按位或进新事件位：一次 poll 同时监视同一 fd 的读与写 */
        ev->index = e->index;    /* 两个方向的事件共享下标，任一方向的注销都会牵动该表项 */
    }    /* 新增/合并表项分支结束 */

    ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;    /* 依注册标志设置 oneshot：就绪通知一次后自动摘除，处理完须重新注册 */

    return NGX_OK;    /* 注册成功 */
}    /* ngx_poll_add_event 结束 */


/* ngx_poll_del_event —— 注销一个读或写事件。
 * 反方向仍在登记时只从表项 events 中按位清除；否则整项删除：
 * 用数组最后一项覆盖被删位置(交换填补，O(1))，再把被搬家事件的
 * index 修正到新位置。由此保证每轮提交给内核的 [0,nevents) 区间内
 * 没有无效表项，无需搬移整段数组或周期性压缩。 */
static ngx_int_t ngx_poll_del_event(ngx_event_t *ev, int event, u_int flags)    /* 注销事件回调：event 为框架抽象的读/写事件类型 */
{    /* 注销逻辑开始 */
    ngx_uint_t          i;    /* 遍历旧周期数组的循环下标 */
    ngx_cycle_t       **cycle;    /* 指向各旧 cycle 的指针数组首地址，跨 reload 查找连接用 */
    ngx_event_t        *e;    /* 反方向事件指针，判断表项是否还有其他事件位 */
    ngx_connection_t   *c;    /* 事件所属连接 */

    c = ev->data;    /* 取事件绑定的连接对象 */

    ev->active = 0;    /* 清除活跃标记：事件已不在(或将不在)事件模型中 */

    if (ev->index == NGX_INVALID_INDEX) {    /* index 为哨兵值说明该事件本就未登记 */
        ngx_log_error(NGX_LOG_ALERT, ev->log, 0,    /* ALERT 级日志：重复注销属逻辑异常 */
                      "poll event fd:%d ev:%d is already deleted",    /* 提示已注销 */
                      c->fd, event);    /* 附 fd 与事件类型 */
        return NGX_OK;    /* 幂等返回，不做数组操作 */
    }    /* 防重分支结束 */

    if (event == NGX_READ_EVENT) {    /* 与 add 对称：抽象事件位映射为 poll 事件位 */
        e = c->write;    /* 读事件的反方向是写事件 */
#if (NGX_READ_EVENT != POLLIN)     /* 值不一致才转换 */
        event = POLLIN;    /* 转成 POLLIN 位 */
#endif     /* 事件位转换条件编译结束 */

    } else {    /* 写事件分支 */
        e = c->read;    /* 写事件的反方向是读事件 */
#if (NGX_WRITE_EVENT != POLLOUT)     /* 值不一致才转换 */
        event = POLLOUT;    /* 转成 POLLOUT 位 */
#endif     /* 事件位转换条件编译结束 */
    }    /* 事件位映射结束 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,    /* debug 日志 */
                   "poll del event: fd:%d ev:%d", c->fd, event);   /* 记录被注销的 fd 与事件位 */

    if (e == NULL || e->index == NGX_INVALID_INDEX) {    /* 反方向未登记：整个表项都要移除 */
        nevents--;    /* 有效表项数减一：nevents 现在指向"原最后一项"的下标 */

        if (ev->index < (u_int) nevents) {    /* 被删项不是最后一项才需要填补：否则数组自然收缩 */

            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,    /* debug 日志 */
                           "index: copy event %d to %d", nevents, ev->index);   /* 记录最后一项搬家到哪个下标 */

            event_list[ev->index] = event_list[nevents];    /* 尾部表项整体覆盖被删位置：O(1) 交换填补，避免 O(n) 搬移或留下空洞全量提交 */

            c = &ngx_cycle->connections[event_list[nevents].fd];    /* 反查搬家表项所属连接：fd 即当前周期连接数组的下标 */

            if (c->fd == -1) {    /* 当前周期里该槽位连接已关闭(fd=-1)：可能属于旧周期 */
                cycle = ngx_old_cycles.elts;    /* 取旧周期指针数组，逐个尝试 */
                for (i = 0; i < ngx_old_cycles.nelts; i++) {    /* 线性扫描所有旧 cycle */
                    if (cycle[i] == NULL) {    /* 空槽位跳过 */
                        continue;    /* 继续下一个 */
                    }    /* 空槽判断结束 */
                    c = &cycle[i]->connections[event_list[nevents].fd];    /* 在旧周期连接表里按同一 fd 下标取连接 */
                    if (c->fd != -1) {    /* 找到仍存活的连接 */
                        break;    /* 结束查找 */
                    }    /* 存活判断结束 */
                }    /* 旧周期扫描结束 */
            }    /* 跨周期查找分支结束 */

            if (c->fd == -1) {    /* 所有周期里都找不到存活连接：数据不一致 */
                ngx_log_error(NGX_LOG_ALERT, ev->log, 0,    /* ALERT 级日志：预期之外的状态 */
                              "unexpected last event");   /* 提示搬家的最后一项找不到归属 */

            } else {    /* 找到了搬家表项所属连接：修正其事件 index */
                if (c->read->index == (u_int) nevents) {    /* 读事件若正指向旧尾部下标 */
                    c->read->index = ev->index;    /* 更新为搬家后的新下标，保证后续注销/oneshot 定位正确 */
                }    /* 读事件修正结束 */

                if (c->write->index == (u_int) nevents) {    /* 写事件同理 */
                    c->write->index = ev->index;    /* 同步更新到新下标 */
                }    /* 写事件修正结束 */
            }    /* index 修正分支结束 */
        }    /* 非末项填补分支结束 */

    } else {    /* 反方向仍在登记：只摘除本方向的事件位 */
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,    /* debug 日志 */
                       "poll del index: %d", e->index);   /* 记录共享表项的下标 */

        event_list[e->index].events &= ~event;    /* 按位清除请求位：fd 与另一方向事件保留，表项位置不动 */
    }    /* 整项删除/按位清除分支结束 */

    ev->index = NGX_INVALID_INDEX;    /* 事件打回"未登记"哨兵值，此后 add/del 的防重检查以此为据 */

    return NGX_OK;    /* 注销成功 */
}    /* ngx_poll_del_event 结束 */


/* ngx_poll_process_events —— poll 事件模型主处理函数(worker 每轮事件循环调用一次)。
 * 流程：先从定时器红黑树取最小超时，把整个 pollfd 数组拷入内核；
 * 内核对每个 fd 查询就绪状态并回填 revents，返回就绪个数；随后用户态
 * O(nevents) 全量扫描找出就绪表项——poll 与 select 同为 O(n) 模型，
 * 这正是 epoll(红黑树+就绪链表+回调，O(就绪数)) 出现的动机。
 * 读/写就绪按 oneshot 自动摘除；监听事件进 accept 链表延迟持锁处理；
 * 普通事件进延投递队列统一执行。 */
static ngx_int_t ngx_poll_process_events(ngx_cycle_t *cycle)    /* 事件处理主入口：返回值计入事务统计 */
{    /* 事件处理逻辑开始 */
    int                 ready;    /* poll() 返回值：>0 就绪表项个数、0 超时、-1 出错 */
    ngx_int_t           i, nready;    /* i 为数组扫描下标；nready 为备选实现的就绪计数(现行实现恒为 0) */
    ngx_uint_t          n, found, lock, expire;    /* n 旧周期扫描下标；found 本表项是否有就绪位；lock 表示延投递锁仍持有；expire 表示是否执行定时器过期回调 */
    ngx_msec_t          timer;    /* 本轮 poll 的超时毫秒数：取定时器红黑树最小值，NGX_TIMER_INFINITE(-1) 表示无限阻塞 */
    ngx_err_t           err;    /* poll() 失败时的错误码，用于区分 EINTR 等情形 */
    ngx_cycle_t       **old_cycle;    /* 旧周期指针数组首地址，跨 reload 查找连接用 */
    ngx_event_t        *ev;    /* 就绪事件工作指针 */
    ngx_epoch_msec_t    delta;    /* 进入本函数时刻的流逝毫秒值(后复用为 poll 阻塞耗时)，供定时器过期补偿 */
    ngx_connection_t   *c;    /* 就绪表项对应的连接 */
    struct timeval      tv;    /* poll 返回后的墙上时间，用于刷新缓存时间与耗时统计 */

    for ( ;; ) {    /* 先把已到期定时器全部触发：否则 poll 会拿着过期的 timer 白白阻塞 */
        timer = ngx_event_find_timer();    /* 查定时器红黑树最小节点：返回距最近到期的毫秒数，作为本轮 poll 的 timeout */

        if (timer != 0) {    /* 非 0 说明最近定时器尚未到期，可直接用作 poll 超时 */
            break;    /* 跳出，去阻塞等待 IO 事件 */
        }    /* 未到期判断结束 */

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug 日志 */
                       "poll expired timer");   /* 提示存在已到期定时器 */

        ngx_event_expire_timers((ngx_msec_t)    /* 触发所有已到期定时器的回调 */
                                (ngx_elapsed_msec - ngx_old_elapsed_msec));   /* 传入自上轮以来流逝的毫秒数 */
    }    /* 到期定时器清空循环结束 */

    /* NGX_TIMER_INFINITE == INFTIM */    /* 原注释：nginx 的无限超时值与 poll 的 INFTIM(-1) 同义 */

    if (timer == NGX_TIMER_INFINITE) {    /* 无任何定时器：poll 可以无限阻塞 */
        expire = 0;    /* 标记无需执行定时器过期回调 */

    } else {    /* 有定时器 */
        expire = 1;    /* poll 返回后按实际耗时触发到期回调 */
    }    /* expire 标志设置结束 */

    ngx_old_elapsed_msec = ngx_elapsed_msec;     /* 记下进入前的毫秒基准：本函数开头的 expire_timers 靠新旧差值算时间增量 */

#if (NGX_DEBUG0)     /* 调试开关：全量逐项打印代价高，仅深度调试时编译 */
    if (cycle->log->log_level & NGX_LOG_DEBUG_ALL) {    /* 日志级别开了任一 debug 才执行 */
        for (i = 0; i < nevents; i++) {    /* 遍历整个监视数组 */
            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* 逐项打印 */
                           "poll: %d: fd:%d ev:%04X",    /* 下标、fd、请求事件位 */
                           i, event_list[i].fd, event_list[i].events);   /* 三个参数 */
        }    /* 数组遍历结束 */
    }    /* 日志级别判断结束 */
#endif     /* 调试打印代码段结束 */

    if (ngx_accept_mutex) {    /* 配置启用 accept 互斥锁(多 worker 防惊群)时才参与竞争 */
        if (ngx_accept_disabled > 0) {    /* 本 worker 连接数超配额(建立连接时累加 disabled) */
            ngx_accept_disabled--;    /* 消耗一次配额：本轮放弃抢锁，把接入机会让给其他 worker */

        } else {    /* 未超载：尝试抢锁 */
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) {    /* 非阻塞尝试获取 accept 互斥锁，ERROR 属内部错误 */
                return NGX_ERROR;    /* 放弃本轮事件处理 */
            }    /* 抢锁错误判断结束 */

            if (ngx_accept_mutex_held == 0    /* 没抢到锁 */
                && (timer == NGX_TIMER_INFINITE    /* 且原定无限等待 */
                    || timer > ngx_accept_mutex_delay))   /* 或原定超时比锁重试周期还长 */
            {     /* 需要限制本轮阻塞时长 */
                timer = ngx_accept_mutex_delay;    /* 把 poll 超时压到 accept_mutex_delay：尽快醒来重试抢锁 */
                expire = 0;    /* 这段人为缩短的等待不算真实定时器流逝，不触发超时回调 */
            }     /* 等待时长调整结束 */
        }    /* 抢锁/让锁分支结束 */
    }    /* accept 互斥锁处理结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "poll timer: %d", timer);   /* debug 日志：记录本轮 poll 阻塞超时 */

    /* 核心系统调用：整个 event_list 拷入内核，内核对每个 fd 查询就绪状态并回填 revents；
     * timeout 以毫秒计，-1(NGX_TIMER_INFINITE) 无限阻塞。返回 -1 出错、0 超时、>0 就绪个数。 */
    ready = poll(event_list, (u_int) nevents, (int) timer);    /* 连接数大时这里是 O(n) 全量拷贝+O(n) 内核逐项查询——epoll 正为消除此开销而生 */

    if (ready == -1) {    /* -1 表示调用出错(最常见 EINTR：被信号打断) */
        err = ngx_errno;    /* 立即取出错误码，防止被后续调用覆盖 */
    } else {    /* 正常返回(含 0 超时) */
        err = 0;    /* 错误码清零 */
    }    /* 错误码获取结束 */

    ngx_gettimeofday(&tv);    /* 取 poll 返回后的墙上时间：内核可能阻塞了很久，缓存时间必须刷新 */
    ngx_time_update(tv.tv_sec);    /* 更新 nginx 缓存的秒级时间(日志时间戳等使用) */

    delta = ngx_elapsed_msec;    /* 先记下进入时的流逝毫秒值，稍后作差得到 poll 阻塞耗时 */
    ngx_elapsed_msec = (ngx_epoch_msec_t) tv.tv_sec * 1000    /* 用墙上时间重算自进程启动以来的毫秒数 */
                                          + tv.tv_usec / 1000 - ngx_start_msec;   /* 微秒折算成毫秒后减去启动基准 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug 日志 */
                   "poll ready %d of %d", ready, nevents);   /* 就绪数/监视总数：直观体现 O(n) 扫描的投入产出比 */

    if (err) {    /* poll 确实出错 */
        ngx_log_error((err == NGX_EINTR) ? NGX_LOG_INFO : NGX_LOG_ALERT,    /* EINTR 是信号打断、属正常现象记 INFO，其余记 ALERT */
                      cycle->log, err, "poll() failed");   /* 带错误码记录失败 */
        ngx_accept_mutex_unlock();    /* 释放本轮持有的 accept 互斥锁(若持有) */
        return NGX_ERROR;    /* 错误上抛：旧版实现连 EINTR 也整轮放弃，不在本层重试 */
    }    /* 出错分支结束 */

    if (timer != NGX_TIMER_INFINITE) {    /* 有限超时：统计真实阻塞耗时 */
        delta = ngx_elapsed_msec - delta;    /* 差值即 poll 实际阻塞的毫秒数，供定时器过期判定 */

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug 日志 */
                       "poll timer: %d, delta: %d", timer, (int) delta);   /* 对比期望超时与实际耗时 */
    } else {    /* 无限阻塞分支 */
        if (ready == 0) {    /* 无限等待却空手而归：内核行为异常 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,    /* ALERT 级日志 */
                          "poll() returned no events without timeout");   /* 明示异常情形 */
            ngx_accept_mutex_unlock();    /* 释放 accept 锁 */
            return NGX_ERROR;    /* 视为错误退出本轮 */
        }    /* 空返回判断结束 */
    }    /* 超时统计分支结束 */

    if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {    /* 多线程模式：先锁住延投递事件队列 */
        ngx_accept_mutex_unlock();    /* 加锁失败也要释放 accept 锁 */
        return NGX_ERROR;    /* 错误上抛 */
    }    /* 加锁判断结束 */

    lock = 1;    /* 记录"延投递锁已持有"，函数尾部按此决定是否解锁 */
    nready = 0;    /* 就绪事件计数清零：仅备选实现会累加，现行实现恒为 0 */

    /* 用户态 O(nevents) 全量扫描：poll 只返回就绪个数、不指明具体哪些表项，
     * 必须逐项检查 revents；ready 减到 0 即提前结束，省去尾部无事件表项的扫描。 */
    for (i = 0; i < nevents && ready; i++) {    /* 比 select 少了重建位图与多次扫描，但复杂度同为 O(n)——连接数大时性能线性下降 */

#if 0     /* 被关闭的调试打印：无条件逐项输出 */
        ngx_log_debug4(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* 四参数日志 */
                       "poll: %d: fd:%d ev:%04X rev:%04X",    /* 下标、fd、请求位、返回位 */
                       i, event_list[i].fd,    /* 下标与 fd */
                       event_list[i].events, event_list[i].revents);   /* 请求事件位与内核回填位 */
#else     /* 现行版本：仅打印就绪表项，避免每轮刷屏 */
        if (event_list[i].revents) {    /* 仅内核报告了事件(revents 非零)才打印 */
            ngx_log_debug4(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* 四参数日志 */
                           "poll: %d: fd:%d ev:%04X rev:%04X",    /* 格式串 */
                           i, event_list[i].fd,    /* 下标与 fd */
                           event_list[i].events, event_list[i].revents);   /* 请求位与回填位 */
        }    /* 就绪打印判断结束 */
#endif     /* 调试打印二选一结束 */

        if (event_list[i].revents & POLLNVAL) {    /* POLLNVAL：fd 无效(如已 close 却未从数组移除)——内核不查询直接置位，属 bug 防御 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,    /* ALERT 级日志 */
                          "poll() error fd:%d ev:%04X rev:%04X",    /* 打印现场 */
                          event_list[i].fd,    /* 无效 fd */
                          event_list[i].events, event_list[i].revents);   /* 请求位与返回位 */
        }    /* POLLNVAL 检查结束 */

        /* revents 中出现 nginx 不认识的比特：打日志留痕，便于发现内核 poll 语义变化 */
        if (event_list[i].revents & ~(POLLIN|POLLOUT|POLLERR|POLLHUP|POLLNVAL))
        {    /* 进入异常处理 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,    /* ALERT 级日志 */
                          "strange poll() events fd:%d ev:%04X rev:%04X",    /* 打印奇怪的事件位 */
                          event_list[i].fd,    /* 涉事 fd */
                          event_list[i].events, event_list[i].revents);   /* 请求位与返回位 */
        }    /* 未知事件位检查结束 */

        if (event_list[i].fd == -1) {    /* fd=-1 的表项是"异常事件"防御机制打上的禁用标记(见下) */
            /*
             * the disabled event, a workaround for our possible bug,
             * see the comment below
             */
            continue;    /* 原注释意为"被禁用的事件、规避潜在 bug 的权宜之计"；poll 忽略 fd<0 项，跳过 */
        }    /* 禁用表项判断结束 */

        c = &ngx_cycle->connections[event_list[i].fd];    /* fd 即连接数组下标，反查当前周期的连接对象 */

        if (c->fd == -1) {    /* 当前周期该槽位已关闭：连接可能属于 reload 前的旧周期 */
            old_cycle = ngx_old_cycles.elts;    /* 取旧周期指针数组首地址 */
            for (n = 0; n < ngx_old_cycles.nelts; n++) {    /* 逐个旧周期查找 */
                if (old_cycle[n] == NULL) {    /* 空槽位跳过 */
                    continue;    /* 下一个 */
                }    /* 空槽判断结束 */
                c = &old_cycle[n]->connections[event_list[i].fd];    /* 在旧周期连接表按同一 fd 下标取连接 */
                if (c->fd != -1) {    /* 找到仍存活的连接 */
                    break;    /* 结束查找 */
                }    /* 存活判断结束 */
            }    /* 旧周期扫描结束 */
        }    /* 跨周期查找结束 */

        if (c->fd == -1) {    /* 所有周期都找不到存活连接：事件已无归属 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0, "unexpected event");   /* ALERT 级日志：预期之外的事件 */

            /*
             * it is certainly our fault and it should be investigated,
             * in the meantime we disable this event to avoid a CPU spinning
             */

            if (i == nevents - 1) {    /* 原注释意为"确系我方 bug 应排查，先禁用免得 CPU 空转"；判断就绪项是否数组末项 */
                nevents--;    /* 是末项则直接收缩数组：下轮不再提交该表项 */
            } else {    /* 中间项不能破坏数组结构 */
                event_list[i].fd = -1;    /* 打 fd=-1 禁用标记(见上)：poll 忽略之，避免每轮 CPU 空转 */
            }    /* 禁用方式分支结束 */

            continue;    /* 该表项已处置，继续扫描 */
        }    /* 无归属事件防御结束 */

        found = 0;    /* 先假设本表项无 nginx 关心的就绪位 */

        /* 可读侧就绪：POLLIN=内核接收缓冲区有数据(监听 socket 为全连接队列非空)；
         * POLLERR/POLLHUP/POLLNVAL 即使未在 events 注册，内核也会在 revents 强制报告，一并按可读上报交给处理层 */
        if (event_list[i].revents & (POLLIN|POLLERR|POLLHUP|POLLNVAL)) {    /* 进入可读就绪分支 */
            found = 1;    /* 标记本表项有就绪位，稍后消耗 ready 计数 */

            ev = c->read;    /* 取该连接的读事件 */
            ev->ready = 1;    /* 置就绪标志，事件循环随后调用其 handler */

            if (ev->oneshot) {    /* oneshot 语义：通知一次后自动摘除 */
                if (ev->timer_set) {    /* 若挂着读写超时定时器 */
                    ngx_del_timer(ev);    /* 先摘除，防止悬空触发 */
                }    /* 定时器判断结束 */
                ngx_poll_del_event(ev, NGX_READ_EVENT, 0);    /* 从 poll 数组注销读事件，处理完须重新注册 */
            }    /* oneshot 处理结束 */

            if (ev->accept) {    /* 监听 socket：可读即全连接队列非空，可 accept 新连接 */
                ev->next = accept_events;    /* 头插进 accept 事件链表 */
                accept_events = ev;    /* 延迟到下方持锁统一处理，多 worker 间防惊群 */
            } else {    /* 普通连接事件 */
                ngx_post_event(ev);    /* 进延投递队列，统一持锁按序执行 */
            }    /* accept/普通事件分流结束 */

#if 0     /* 备选实现：登记进就绪索引表 */
            ready_index[nready++] = c->read;    /* 记录就绪读事件(未启用) */
#endif     /* 备选代码段结束 */
        }    /* 可读就绪分支结束 */

        /* 可写侧就绪：POLLOUT=内核发送缓冲区有空位(此前写不进多半因对端接收窗口小/网络慢)；
         * 错误类标志同样按可写上报，让 write 返回错误以驱动关闭连接 */
        if (event_list[i].revents & (POLLOUT|POLLERR|POLLHUP|POLLNVAL)) {    /* 进入可写就绪分支 */
            found = 1;    /* 标记就绪 */
            ev = c->write;    /* 取该连接的写事件 */
            ev->ready = 1;    /* 置就绪标志 */

            if (ev->oneshot) {    /* oneshot：通知一次即摘除 */
                if (ev->timer_set) {    /* 挂有定时器 */
                    ngx_del_timer(ev);    /* 先摘除 */
                }    /* 定时器判断结束 */
                ngx_poll_del_event(ev, NGX_WRITE_EVENT, 0);    /* 注销写事件 */
            }    /* oneshot 处理结束 */

            ngx_post_event(ev);    /* 写事件进延投递队列(无 accept 语义，不需单独链表) */
#if 0     /* 备选实现：登记进就绪索引表 */
            ready_index[nready++] = c->write;    /* 记录就绪写事件(未启用) */
#endif     /* 备选代码段结束 */
        }    /* 可写就绪分支结束 */

        if (found) {    /* 本表项确实报告了 nginx 关心的事件 */
            ready--;    /* 消耗一个就绪计数：减到 0 后外层循环提前结束 */
            continue;    /* 继续下一表项 */
        }    /* 就绪计数消耗判断结束 */
    }    /* O(n) 就绪扫描循环结束 */

#if 0     /* 备选实现：先收集就绪索引再统一派发(未启用) */
    for (i = 0; i < nready; i++) {    /* 遍历就绪事件表 */
        ev = ready_index[i];    /* 取出就绪事件 */

        if (!ev->active) {    /* 事件可能在处理中途被注销 */
            continue;    /* 跳过已失效事件 */
        }    /* 活跃判断结束 */

        ev->ready = 1;    /* 置就绪标志 */

        if (ev->oneshot) {    /* oneshot 语义 */
            if (ev->timer_set) {    /* 挂有定时器 */
                ngx_del_timer(ev);    /* 摘除定时器 */
            }    /* 定时器判断结束 */

            if (ev->write) {    /* 写事件 */
                ngx_poll_del_event(ev, NGX_WRITE_EVENT, 0);    /* 注销写事件 */
            } else {    /* 读事件 */
                ngx_poll_del_event(ev, NGX_READ_EVENT, 0);    /* 注销读事件 */
            }    /* 方向判断结束 */
        }    /* oneshot 处理结束 */

        ev->event_handler(ev);    /* 直接调用事件处理器 */
    }    /* 备选派发循环结束 */
#endif     /* 备选派发代码段结束 */

    ev = accept_events;    /* 取 accept 事件链表头，开始逐一处理监听事件 */

    for ( ;; ) {    /* 遍历链表直到取空或中途放弃 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,    /* debug 日志 */
                      "accept event " PTR_FMT, ev);   /* 打印当前 accept 事件地址 */

        if (ev == NULL) {    /* 链表走完 */
            break;    /* 结束处理 */
        }    /* 空判断结束 */

        ngx_mutex_unlock(ngx_posted_events_mutex);    /* 执行 handler 前先放开延投递锁：accept 回调可能再入事件接口，避免自锁 */

        ev->event_handler(ev);    /* 执行监听事件处理器(ngx_event_accept)：循环 accept 直到 EAGAIN 排空全连接队列 */

        if (ngx_accept_disabled > 0) {    /* accept 过程中发现本 worker 连接数超限 */
            lock = 0;    /* 锁已在执行前放开，尾部无需再解锁 */
            break;    /* 立即停止，剩余监听事件让给其他 worker */
        }    /* 超载判断结束 */

        ev = ev->next;    /* 前进到下一个监听事件 */

        if (ev == NULL) {    /* 已是最后一个 */
            lock = 0;    /* 标记尾部无需再解锁 */
            break;    /* 结束循环 */
        }    /* 末节点判断结束 */

        if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {    /* 处理下一个事件前重新拿回延投递锁 */
            ngx_accept_mutex_unlock();    /* 加锁失败则释放 accept 锁 */
            return NGX_ERROR;    /* 错误上抛 */
        }    /* 加锁判断结束 */

    }    /* accept 链表遍历结束 */

    ngx_accept_mutex_unlock();    /* 归还 accept 互斥锁：其他 worker 可开始抢接新连接 */
    accept_events = NULL;    /* 清空本轮链表，下轮重建 */

    if (lock) {    /* 中途未提前退出时锁仍在手里 */
        ngx_mutex_unlock(ngx_posted_events_mutex);    /* 释放延投递队列锁 */
    }    /* 解锁判断结束 */

    if (ready != 0) {    /* 扫描结束后 ready 未清零：有就绪计数没对应到关心的事件位 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0, "poll ready != events");   /* ALERT 级日志：内核报告与用户解析不匹配，值得排查 */
    }    /* 计数核对判断结束 */

    if (expire && delta) {    /* 允许触发定时器且 poll 确实消耗了时间 */
        ngx_event_expire_timers((ngx_msec_t) delta);    /* 按阻塞毫秒数触发到期定时器回调 */
    }    /* 定时器触发判断结束 */

    if (!ngx_threaded) {    /* 非线程模式：延投递队列由本函数自己执行 */
        ngx_event_process_posted(cycle);    /* 逐个执行延投递队列中事件的 handler */
    }    /* 线程模式判断结束 */

    return nready;    /* 返回本轮处理的事件数(现行实现恒为 0，负载统计走 posted 队列) */
}    /* ngx_poll_process_events 结束 */


/* ngx_poll_init_conf —— 配置初始化/校验回调。
 * 仅当 nginx.conf 的 use 指令确实选中 poll 时才检查：
 * 线程模式与 poll 后端不兼容，直接报致命错误拒绝启动。 */
static char *ngx_poll_init_conf(ngx_cycle_t *cycle, void *conf)    /* 配置回调：conf 为本模块配置对象(本模块未使用) */
{    /* 配置校验逻辑开始 */
    ngx_event_conf_t  *ecf;    /* 事件核心模块的配置：含 use 选型等全局事件设置 */

    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);    /* 从配置上下文取出事件核心模块的配置 */

    if (ecf->use != ngx_poll_module.ctx_index) {    /* 用户没有选用 poll 事件模型 */
        return NGX_CONF_OK;    /* 无需校验，直接放行 */
    }    /* 选型判断结束 */

#if (NGX_THREADS)     /* 编译启用线程支持时才存在该限制 */

    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,    /* EMERG 级日志：致命错误 */
                  "poll() is not supported in the threaded mode");   /* poll 后端未适配多线程，拒绝启动 */
    return NGX_CONF_ERROR;    /* 返回错误：配置解析失败，nginx 退出 */

#else      /* 未启用线程：正常放行 */

    return NGX_CONF_OK;    /* 校验通过 */

#endif     /* 线程条件编译结束 */
}    /* ngx_poll_init_conf 结束 */
