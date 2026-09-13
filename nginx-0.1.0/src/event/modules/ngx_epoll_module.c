
/*
 * Copyright (C) Igor Sysoev
 */

/*
 * ============================================================================
 * ngx_epoll_module.c —— nginx 0.1.0 的 epoll 事件驱动模块（逐行中文注释版）
 *
 * 【epoll 三大系统调用与内核数据结构】
 *   epoll_create(size)：内核创建 eventpoll 对象并返回 epfd。对象内含一棵红黑树
 *   （管理所有被监听的 fd，增删改查 O(log n)）和一条就绪链表 rdllist（存放已发
 *   生事件的 fd）。
 *   epoll_ctl(ADD/MOD/DEL)：在红黑树上插入/修改/摘除节点，同时把回调
 *   ep_poll_callback 注册到该 socket 的内核等待队列上。此后当网卡数据到达 →
 *   触发硬件中断 → 内核协议栈收包 → socket 变为可读时，该回调把 fd 挂入
 *   rdllist 并唤醒阻塞在 epoll_wait 上的进程。
 *   epoll_wait：只检查 rdllist，把就绪事件连同用户数据拷回用户态数组，开销
 *   O(就绪个数)；不像 select/poll 每次都要全量传 fd 集合并 O(n) 扫描、也不必
 *   每次重新注册——这正是 epoll 解决 C10K 问题的答案：一次注册、回调驱动、
 *   按需返回。
 *
 * 【事件位含义】EPOLLIN=可读（接收缓冲区有数据/监听 socket 的全连接队列非
 *   空）；EPOLLOUT=可写（发送缓冲区有空闲空间）；EPOLLERR/EPOLLHUP=错误/挂
 *   断（对端发 RST 或关闭连接）；EPOLLPRI=带外数据（TCP URG）。
 *
 * 【LT 与 ET】水平触发 LT：只要条件满足每次 epoll_wait 都上报该 fd，编程简
 *   单、不易丢事件；边缘触发 ET：状态从无→有只通知一次，必须一次读写到
 *   EAGAIN。本模块对客户端连接使用 EPOLLET（见 ngx_epoll_add_connection），
 *   整体 LT/ET 语义由 HAVE_CLEAR_EVENT 宏在 ngx_epoll_init 中决定
 *   （NGX_USE_CLEAR_EVENT 或 NGX_USE_LEVEL_EVENT）。
 * ============================================================================
 */


#include <ngx_config.h>   /* nginx 平台配置头：auto/configure 探测出的平台宏与基础类型定义 */
#include <ngx_core.h>     /* nginx 核心头：内存池、字符串、日志及 ngx_cycle_t/ngx_connection_t 等核心声明 */
#include <ngx_event.h>    /* 事件框架头：ngx_event_t、事件模块接口、定时器与 accept 互斥锁等声明 */


#if (TEST_BUILD_EPOLL)    /* 条件编译：仅测试构建时在用户态伪造一份 epoll 声明，便于在无 epoll 的系统上做编译检查 */

/* epoll declarations */

#define EPOLLIN        0x001   /* 可读：接收缓冲区有数据，或监听 socket 的全连接队列非空（三次握手完成已入队） */
#define EPOLLPRI       0x002   /* 带外数据可读（紧急）：TCP 首部 URG 置位，优先级带上有紧急字节 */
#define EPOLLOUT       0x004   /* 可写：发送缓冲区有空闲空间（对端通告窗口打开），write 不会被阻塞 */
#define EPOLLRDNORM    0x040   /* 普通数据可读：与 poll 的 POLLIN 掩码对应的 revents 位，内核 poll 机制内部使用 */
#define EPOLLRDBAND    0x080   /* 优先级带数据可读：接收优先级不低于低水位标记 */
#define EPOLLWRNORM    0x100   /* 普通数据可写：发送缓冲区可用空间不低于低水位标记 */
#define EPOLLWRBAND    0x200   /* 优先级带数据可写：发送优先级空间不低于低水位标记 */
#define EPOLLMSG       0x400   /* 保留的消息事件位：内核基本未用，仅为与 poll 掩码集合对齐 */
#define EPOLLERR       0x008   /* 错误：socket 异常（典型为收到对端 RST 复位报文），无需注册内核也会强制上报 */
#define EPOLLHUP       0x010   /* 挂断：对端关闭连接（收到 FIN 后半/全关闭），读写在协议层面均不再有意义 */

#define EPOLLET        0x80000000   /* 边缘触发 ET 标志：状态从无→有只通知一次，必须一次读写到 EAGAIN，否则丢事件 */
#define EPOLLONESHOT   0x40000000   /* 一次性触发：事件上报一次后该 fd 被内核屏蔽，需再次 epoll_ctl 才能重新收事件 */

#define EPOLL_CTL_ADD  1            /* epoll_ctl 操作码：向 eventpoll 红黑树插入一个被监听 fd 节点并挂接等待队列回调 */
#define EPOLL_CTL_DEL  2            /* epoll_ctl 操作码：从红黑树摘除该 fd，同时解除其 socket 等待队列上的回调 */
#define EPOLL_CTL_MOD  3            /* epoll_ctl 操作码：修改红黑树节点的事件掩码（nginx 用它实现"覆盖式"重注册） */

typedef union epoll_data {          /* epoll 事件携带的用户数据联合体：四种视图共享 8 字节，让事件结构尽量小 */
    void         *ptr;              /* 指针视图：nginx 存放 ngx_connection_t 地址，最低位复用作 instance 代际标记 */
    int           fd;               /* 整数视图：通用场景下直接存放文件描述符编号 */
    uint32_t      u32;              /* 32 位无符号视图：存放小整数或 32 位句柄 */
    uint64_t      u64;              /* 64 位无符号视图：完整原始 8 字节 */
} epoll_data_t;                     /* epoll_data_t 联合体定义结束 */

struct epoll_event {                /* epoll_ctl 注册 / epoll_wait 返回所用的事件结构 */
    uint32_t      events;           /* 事件位掩码：EPOLLIN/EPOLLOUT/EPOLLERR 等按位或 */
    epoll_data_t  data;             /* 用户数据：随节点保存在内核，事件就绪时原样拷回用户态 */
};   /* struct epoll_event 结构定义结束 */

int epoll_create(int size);         /* 原型：创建 eventpoll 对象返回 epfd；size 仅为内核初始容量的提示值 */

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);   /* 原型：增/删/改红黑树中被监听 fd 并维护等待队列回调 */

int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout);   /* 原型：检查就绪链表并把事件拷回用户态数组，timeout 为阻塞毫秒数 */

int epoll_create(int size)          /* 测试桩：非 epoll 平台的空实现，保证链接通过 */
{   /* 函数体开始 */
    return -1;                      /* 返回 -1 表示创建失败（errno 置 ENOSYS，系统不支持该调用） */
}   /* 函数体结束：恒定返回失败 */

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)   /* 测试桩实现 */
{   /* 函数体开始 */
    return -1;                      /* 返回 -1 表示控制操作失败 */
}   /* 函数体结束：恒定返回失败 */

int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout)   /* 测试桩实现 */
{   /* 函数体开始 */
    return -1;                      /* 返回 -1 表示等待失败 */
}   /* 函数体结束：恒定返回失败 */

#endif                              /* 结束 TEST_BUILD_EPOLL 条件编译 */


typedef struct {                    /* epoll 模块配置结构：保存 nginx.conf 中 epoll_events 指令的解析结果 */
    u_int  events;                  /* 单次 epoll_wait 的事件数组容量（即 nevents），决定一次最多收割多少就绪事件 */
} ngx_epoll_conf_t;                 /* epoll 模块配置结构定义结束 */


static int ngx_epoll_init(ngx_cycle_t *cycle);   /* 前置声明：创建内核 eventpoll、分配事件表并安装事件驱动接口 */
static void ngx_epoll_done(ngx_cycle_t *cycle);   /* 前置声明：close(epfd) 释放内核 eventpoll 并释放事件表内存 */
static int ngx_epoll_add_event(ngx_event_t *ev, int event, u_int flags);   /* 前置声明：注册/启用单个读或写事件（ADD 或 MOD） */
static int ngx_epoll_del_event(ngx_event_t *ev, int event, u_int flags);   /* 前置声明：注销/禁用单个读或写事件（DEL 或 MOD） */
static int ngx_epoll_add_connection(ngx_connection_t *c);   /* 前置声明：把整条连接（读+写+ET）一次注册进 epoll */
static int ngx_epoll_del_connection(ngx_connection_t *c, u_int flags);   /* 前置声明：把整条连接从 epoll 注销 */
static int ngx_epoll_process_events(ngx_cycle_t *cycle);   /* 前置声明：事件处理主入口（epoll_wait + 分发就绪事件） */

static void *ngx_epoll_create_conf(ngx_cycle_t *cycle);   /* 前置声明：配置解析前分配模块配置结构 */
static char *ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf);   /* 前置声明：解析完成后填充配置默认值 */

static int                  ep = -1;   /* epoll 实例描述符 epfd：用户态句柄指向内核 eventpoll 对象；-1 表示尚未创建 */
static struct epoll_event  *event_list;   /* 就绪事件接收数组：epoll_wait 把内核 rdllist 上的事件批量拷贝到这里 */
static u_int                nevents;   /* 事件数组容量上限：作为 epoll_wait 的 maxevents 参数 */


static ngx_str_t      epoll_name = ngx_string("epoll");   /* 模块名字符串：nginx.conf 的 use epoll; 据此名字选中本事件模块 */

static ngx_command_t  ngx_epoll_commands[] = {   /* 本模块可识别的配置指令表：配置解析器逐项匹配 */

    {ngx_string("epoll_events"),   /* 指令名：epoll_events，设置单次 epoll_wait 的事件数组大小 */
     NGX_EVENT_CONF|NGX_CONF_TAKE1,   /* 指令属性：仅事件块上下文有效，且必须且只能带 1 个参数 */
     ngx_conf_set_num_slot,   /* 解析回调：通用数字槽位函数，把参数直接写入配置结构体字段 */
     0,   /* conf 偏移修正：事件模块直接取自身 conf，无需额外偏移 */
     offsetof(ngx_epoll_conf_t, events),   /* 目标字段偏移：写入 ngx_epoll_conf_t 的 events 成员 */
     NULL},   /* post 指针：解析后的二次处理回调，此处为空 */

    ngx_null_command   /* 指令表结束哨兵：解析器据此停止遍历 */
};   /* ngx_epoll_commands[] 指令表定义结束 */


ngx_event_module_t  ngx_epoll_module_ctx = {   /* epoll 事件模块上下文：模块名 + 配置钩子 + 事件动作表 */
    &epoll_name,   /* 指向模块名 "epoll"：供 use 指令按名查找 */
    ngx_epoll_create_conf,               /* create configuration */   /* 配置创建钩子：解析前分配 ngx_epoll_conf_t */
    ngx_epoll_init_conf,                 /* init configuration */     /* 配置初始化钩子：解析后补默认值 */

    {   /* 事件动作表开始：框架通过这张表以统一接口驱动各种事件机制 */
        ngx_epoll_add_event,             /* add an event */   /* 动作：注册单个读/写事件（内核中 ADD 或 MOD 掩码） */
        ngx_epoll_del_event,             /* delete an event */   /* 动作：注销单个读/写事件 */
        ngx_epoll_add_event,             /* enable an event */   /* 动作：启用事件：复用 add 语义（MOD 覆盖掩码） */
        ngx_epoll_del_event,             /* disable an event */   /* 动作：禁用事件：复用 del 语义 */
        ngx_epoll_add_connection,        /* add an connection */   /* 动作：整条连接注册进 epoll（读写+ET 一次完成） */
        ngx_epoll_del_connection,        /* delete an connection */   /* 动作：整条连接从 epoll 注销 */
        NULL,                            /* process the changes */   /* 动作：批量提交变更；epoll_ctl 即时生效无需此步，置空 */
        ngx_epoll_process_events,        /* process the events */   /* 动作：事件处理主入口，阻塞于 epoll_wait */
        ngx_epoll_init,                  /* init the events */   /* 动作：worker 初始化时创建 eventpoll 并安装接口 */
        ngx_epoll_done,                  /* done the events */   /* 动作：进程退出时释放 epoll 资源 */
    }   /* 事件动作表结束 */
};   /* ngx_epoll_module_ctx 上下文定义结束 */

ngx_module_t  ngx_epoll_module = {   /* nginx 通用模块描述结构：框架据此加载并调用本模块 */
    NGX_MODULE,   /* 模块版本/签名宏：与编译的 nginx 版本绑定，不匹配则拒绝加载 */
    &ngx_epoll_module_ctx,               /* module context */   /* 指向事件模块上下文（NGX_EVENT_MODULE 类型时按此解释） */
    ngx_epoll_commands,                  /* module directives */   /* 指令表：配置解析时匹配 epoll_events */
    NGX_EVENT_MODULE,                    /* module type */   /* 模块类型：事件模块，参与 use 事件机制选择 */
    NULL,                                /* init module */   /* 配置解析后的模块级初始化钩子：epoll 无需 */
    NULL                                 /* init process */   /* worker fork 后的进程级初始化钩子：epoll 在 init events 时做 */
};   /* ngx_epoll_module 模块描述定义结束 */


/*
 * ngx_epoll_init：worker 进程初始化事件机制时被调用。
 * 创建内核 eventpoll（红黑树 + 就绪链表 rdllist）、按配置分配用户态事件
 * 接收数组，并把本模块动作表安装进 ngx_event_actions，此后框架的所有事件
 * 操作都路由到 epoll 实现。
 */
static int ngx_epoll_init(ngx_cycle_t *cycle)   /* 初始化入口 */
{   /* 函数体开始 */
    size_t             n;   /* 尺寸临时变量（本版本流程中未实际使用） */
    ngx_event_conf_t  *ecf;   /* 事件核心模块配置：内含全局 connections 数量 */
    ngx_epoll_conf_t  *epcf;   /* 本模块配置：内含 epoll_events 大小 */

    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);   /* 从配置上下文取出事件核心模块的配置指针 */

    epcf = ngx_event_get_conf(cycle->conf_ctx, ngx_epoll_module);   /* 从配置上下文取出本模块自身的配置指针 */

    if (ep == -1) {   /* 尚未创建 epoll 实例时才创建（reload 后 epfd 可复用） */
        ep = epoll_create(ecf->connections / 2);   /* 陷入内核：分配 eventpoll 对象（内含红黑树与 rdllist）；size 仅为容量提示，返回 epfd */

        if (ep == -1) {   /* 创建失败：内核无 epoll 支持或内存不足 */
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,   /* 以 EMERG 级别记录内核返回的 errno */
                          "epoll_create() failed");   /* 日志消息体：epoll_create 失败 */
            return NGX_ERROR;   /* 返回错误：worker 无法以 epoll 机制启动 */
        }   /* if 结束：epoll_create 出错分支结束 */
    }   /* if (ep == -1) 结束：eventpoll 创建完成 */

    if (nevents < epcf->events) {   /* 现有事件表容量小于配置值时才重新分配（支持 reload 扩容） */
        if (event_list) {   /* 已有旧数组则先释放，避免内存泄漏 */
            ngx_free(event_list);   /* 归还旧的事件数组内存 */
        }   /* if 结束：旧数组释放完毕 */

        event_list = ngx_alloc(sizeof(struct epoll_event) * epcf->events,   /* 按配置容量分配新数组：它是 epoll_wait 就绪事件的拷贝目的地 */
                               cycle->log);   /* 传入日志句柄：分配失败时可记录原因 */
        if (event_list == NULL) {   /* 分配失败检查 */
            return NGX_ERROR;   /* 放弃初始化 */
        }   /* if 结束：分配失败分支结束 */
    }   /* if (nevents < epcf->events) 结束：事件表就绪 */

    nevents = epcf->events;   /* 记录事件表容量：作为 epoll_wait 的 maxevents 参数 */

    ngx_io = ngx_os_io;   /* 挂载 OS 层收发函数表（recv/send 等）：连接层经此读写 socket */

    ngx_event_actions = ngx_epoll_module_ctx.actions;   /* 安装事件动作表：此后框架的 add/del/process 全部路由到 epoll 实现 */

#if (HAVE_CLEAR_EVENT)   /* 平台支持 ET（Linux 上为真）时 */
    ngx_event_flags = NGX_USE_CLEAR_EVENT   /* 声明边缘触发语义：就绪上报一次即被"清除"，必须一次读写到 EAGAIN */
#else   /* 否则退化为水平触发模式 */
    ngx_event_flags = NGX_USE_LEVEL_EVENT   /* 声明水平触发语义：条件满足期间每次 wait 都上报该 fd */
#endif   /* 结束 HAVE_CLEAR_EVENT 条件编译 */
                      |NGX_HAVE_GREEDY_EVENT   /* 附加标志：支持贪心读——一次尽量吃光接收缓冲区数据 */
                      |NGX_USE_EPOLL_EVENT;   /* 附加标志：标记当前事件机制为 epoll */

    return NGX_OK;   /* 初始化成功 */
}   /* ngx_epoll_init 结束 */


/*
 * ngx_epoll_done：进程退出/模块卸载时释放事件资源。
 * close(epfd) 使内核回收 eventpoll 对象：其红黑树上的全部监听节点、各
 * socket 等待队列上的回调一并解除；随后释放用户态就绪事件数组。
 */
static void ngx_epoll_done(ngx_cycle_t *cycle)   /* 资源释放入口 */
{   /* 函数体开始 */
    if (close(ep) == -1) {   /* 关闭 epfd：内核递减引用并回收 eventpoll 及其全部节点 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 关闭失败（如 EBADF）以 ALERT 级别记录 errno */
                      "epoll close() failed");   /* 日志消息体 */
    }   /* if 结束：close 出错分支结束 */

    ep = -1;   /* epfd 复位为 -1：防止悬空描述符被再次使用 */

    ngx_free(event_list);   /* 释放用户态就绪事件数组 */

    event_list = NULL;   /* 指针置空防悬挂 */
    nevents = 0;   /* 容量归零：与指针状态保持一致 */
}   /* ngx_epoll_done 结束 */


/*
 * ngx_epoll_add_event：注册/启用单个方向的读或写事件。
 * 关键点：epoll_ctl 的粒度是"整个 fd"而非单个事件——若另一方向已在监听
 * （对侧 active），必须用 EPOLL_CTL_MOD 合并掩码后覆盖注册，否则重复 ADD
 * 会返回 EEXIST；这正是"MOD 重新 ADD 覆盖"的实现方式。
 */
static int ngx_epoll_add_event(ngx_event_t *ev, int event, u_int flags)   /* 注册入口：ev 事件对象、event 读/写类型、flags 附加标志 */
{   /* 函数体开始 */
    int                  op, prev;   /* op：ADD/MOD 操作码；prev：对侧方向对应的内核事件位 */
    ngx_event_t         *e;   /* 对侧事件对象：用于判断该 fd 是否已有方向注册过 */
    ngx_connection_t    *c;   /* 事件所属连接 */
    struct epoll_event   ee;   /* 传给内核的事件描述：关心掩码 + 用户数据 */

    c = ev->data;   /* 取回注册时绑定的连接对象（事件与连接互相引用） */

    if (event == NGX_READ_EVENT) {   /* 本次注册的是读事件 */
        e = c->write;   /* 对侧取写事件：检查写方向是否已在 epoll 中 */
        prev = EPOLLOUT;   /* 记录对侧（写）的内核事件位：MOD 合并时必须保留 */
#if (NGX_READ_EVENT != EPOLLIN)   /* 若框架读事件常量与内核位不一致才需要换算 */
        event = EPOLLIN;   /* 换算成内核可识别的 EPOLLIN 位 */
#endif   /* 结束读事件常量换算的条件编译 */

    } else {   /* 否则注册的是写事件 */
        e = c->read;   /* 对侧取读事件 */
        prev = EPOLLIN;   /* 记录对侧（读）的内核事件位 */
#if (NGX_WRITE_EVENT != EPOLLOUT)   /* 若框架写事件常量与内核位不一致才需要换算 */
        event = EPOLLOUT;   /* 换算成内核可识别的 EPOLLOUT 位 */
#endif   /* 结束写事件常量换算的条件编译 */
    }   /* if/else 结束：event 已换算为内核事件位 */

    if (e->active) {   /* 对侧方向已注册：该 fd 节点已在红黑树中，不能重复 ADD */
        op = EPOLL_CTL_MOD;   /* 改用 MOD：内核替换节点事件掩码并重挂等待队列回调（覆盖式重注册） */
        event |= prev;   /* 新掩码并入对侧事件位：避免 MOD 覆盖后丢失另一方向的事件 */

    } else {   /* 对侧未注册：该 fd 尚不在监听集合中 */
        op = EPOLL_CTL_ADD;   /* 用 ADD：红黑树插入节点，并在 socket 等待队列挂 ep_poll_callback */
    }   /* if/else 结束：操作码 op 确定 */

    ee.events = event | flags;   /* 关心掩码 = 方向事件位 | 附加标志位 */
    ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);   /* 用户数据：连接指针低位并入 instance 代际标记，供事件返回时甄别陈旧事件 */

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,   /* 调试日志：输出 fd、操作码与事件掩码 */
                   "epoll add event: fd:%d op:%d ev:%08X",   /* 日志格式串 */
                   c->fd, op, ee.events);   /* 日志参数：fd、op、掩码十六进制值 */

    if (epoll_ctl(ep, op, c->fd, &ee) == -1) {   /* 陷入内核：在 eventpoll 红黑树上执行 ADD/MOD，失败返回 -1 */
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,   /* 失败时以 ALERT 级别记录 errno */
                      "epoll_ctl(%d, %d) failed", op, c->fd);   /* 日志参数：操作码与目标 fd */
        return NGX_ERROR;   /* 向框架上报注册失败 */
    }   /* if 结束：epoll_ctl 出错分支结束 */

    ev->active = 1;   /* 本事件置为活跃：用户态标记与内核红黑树的注册状态同步 */
#if 0   /* 被禁用的历史调试代码 */
    ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;   /* 原意：把 ONESHOT 标志记录到事件对象 */
#endif   /* 结束 #if 0 禁用代码块 */

    return NGX_OK;   /* 注册成功 */
}   /* ngx_epoll_add_event 结束 */


/*
 * ngx_epoll_del_event：注销/禁用单个方向的读或写事件。
 * 与 add 对称：若对侧方向仍活跃则用 MOD 仅保留对侧掩码；否则 DEL 摘除
 * 整个节点。另利用"close(fd) 时内核自动清理红黑树节点"的特性免系统调用。
 */
static int ngx_epoll_del_event(ngx_event_t *ev, int event, u_int flags)   /* 注销入口：参数含义同 add_event */
{   /* 函数体开始 */
    int                  op, prev;   /* op：DEL/MOD 操作码；prev：需保留的对侧事件位 */
    ngx_event_t         *e;   /* 对侧事件对象 */
    ngx_connection_t    *c;   /* 所属连接 */
    struct epoll_event   ee;   /* epoll_ctl 参数：新掩码与用户数据 */

    /*
     * when the file descriptor is closed the epoll automatically deletes
     * it from its queue so we do not need to delete explicity the event
     * before the closing the file descriptor
     */

    if (flags & NGX_CLOSE_EVENT) {   /* 因关闭连接而删除：无需内核调用 */
        ev->active = 0;   /* 仅清用户态活跃标记：close(fd) 时内核自动把节点从红黑树摘除并解除回调 */
        return NGX_OK;   /* 直接成功返回 */
    }   /* if 结束：close 场景免内核调用 */

    c = ev->data;   /* 取回所属连接 */

    if (event == NGX_READ_EVENT) {   /* 删除的是读事件 */
        e = c->write;   /* 检查对侧写事件是否仍活跃 */
        prev = EPOLLOUT;   /* 对侧保留位：EPOLLOUT */

    } else {   /* 删除的是写事件 */
        e = c->read;   /* 检查对侧读事件是否仍活跃 */
        prev = EPOLLIN;   /* 对侧保留位：EPOLLIN */
    }   /* if/else 结束：prev 记录对侧保留位 */

    if (e->active) {   /* 对侧仍活跃：不能 DEL 整个节点，只能改掩码 */
        op = EPOLL_CTL_MOD;   /* MOD：新掩码仅含对侧事件位，等效移除本方向的监听 */
        ee.events = prev | flags;   /* 新掩码 = 对侧事件位 | 附加标志 */
        ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);   /* 用户数据：连接指针 + instance 代际标记 */

    } else {   /* 对侧也不活跃：节点可整体摘除 */
        op = EPOLL_CTL_DEL;   /* DEL：从红黑树删除节点，socket 等待队列回调一并解除 */
        ee.events = 0;   /* DEL 时内核忽略掩码：清零即可 */
        ee.data.ptr = NULL;   /* DEL 时内核忽略用户数据：置空 */
    }   /* if/else 结束：确定 DEL 或 MOD */

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,   /* 调试日志：输出 fd、操作码与掩码 */
                   "epoll del event: fd:%d op:%d ev:%08X",   /* 日志格式串 */
                   c->fd, op, ee.events);   /* 日志参数：fd、op、掩码 */

    if (epoll_ctl(ep, op, c->fd, &ee) == -1) {   /* 陷入内核执行 DEL/MOD */
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,   /* 失败记录 errno */
                      "epoll_ctl(%d, %d) failed", op, c->fd);   /* 日志参数：操作码与 fd */
        return NGX_ERROR;   /* 上报失败 */
    }   /* if 结束：epoll_ctl 出错分支结束 */

    ev->active = 0;   /* 本事件活跃标记清零：与内核状态保持一致 */

    return NGX_OK;   /* 注销成功 */
}   /* ngx_epoll_del_event 结束 */


/*
 * ngx_epoll_add_connection：整条连接一次性注册进 epoll。
 * 同时关心读写并加 EPOLLET（边缘触发）：就绪只通知一次，读必须读到
 * EAGAIN；data 里放"连接指针+instance"，epoll_wait 返回时无需查找即可
 * 直接还原连接对象——这是 epoll 高效处理海量连接的关键。
 */
static int ngx_epoll_add_connection(ngx_connection_t *c)   /* 连接注册入口 */
{   /* 函数体开始 */
    struct epoll_event  ee;   /* 传给内核的事件描述 */

    ee.events = EPOLLIN|EPOLLOUT|EPOLLET;   /* 同时关心读写；EPOLLET 边缘触发：状态跳变只报一次，须一次读/写到 EAGAIN */
    ee.data.ptr = (void *) ((uintptr_t) c | c->read->instance);   /* 用户数据：连接指针低位并入读事件 instance 代际标记 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,   /* 调试日志：输出 fd 与事件掩码 */
                   "epoll add connection: fd:%d ev:%08X", c->fd, ee.events);   /* 日志格式串与参数：fd、掩码 */

    if (epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ee) == -1) {   /* ADD：红黑树插入节点并在 socket 等待队列挂 ep_poll_callback */
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,   /* 失败记录 errno */
                      "epoll_ctl(EPOLL_CTL_ADD, %d) failed", c->fd);   /* 日志参数：目标 fd */
        return NGX_ERROR;   /* 上报失败 */
    }   /* if 结束：epoll_ctl 出错分支结束 */

    c->read->active = 1;   /* 读事件标记为已注册 */
    c->write->active = 1;   /* 写事件标记为已注册 */

    return NGX_OK;   /* 注册成功 */
}   /* ngx_epoll_add_connection 结束 */


/*
 * ngx_epoll_del_connection：整条连接从 epoll 注销。
 * 同样利用"close(fd) 内核自动清理"特性：关闭场景免 epoll_ctl 系统调用。
 */
static int ngx_epoll_del_connection(ngx_connection_t *c, u_int flags)   /* 连接注销入口 */
{   /* 函数体开始 */
    int                  op;   /* epoll_ctl 操作码 */
    struct epoll_event   ee;   /* epoll_ctl 参数 */

    /*
     * when the file descriptor is closed the epoll automatically deletes
     * it from its queue so we do not need to delete explicity the event
     * before the closing the file descriptor
     */

    if (flags & NGX_CLOSE_EVENT) {   /* 因关闭连接而注销 */
        c->read->active = 0;   /* 读事件活跃标记清零：内核随 close 自动摘除红黑树节点 */
        c->write->active = 0;   /* 写事件活跃标记清零 */
        return NGX_OK;   /* 直接返回 */
    }   /* if 结束：close 场景免内核调用 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,   /* 调试日志：输出被注销连接的 fd */
                   "epoll del connection: fd:%d", c->fd);   /* 日志格式串与参数：fd */

    op = EPOLL_CTL_DEL;   /* DEL：整个节点从红黑树摘除并解除等待队列回调 */
    ee.events = 0;   /* DEL 时内核忽略掩码 */
    ee.data.ptr = NULL;   /* DEL 时内核忽略用户数据 */

    if (epoll_ctl(ep, op, c->fd, &ee) == -1) {   /* 陷入内核执行删除 */
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,   /* 失败记录 errno */
                      "epoll_ctl(%d, %d) failed", op, c->fd);   /* 日志参数：操作码与 fd */
        return NGX_ERROR;   /* 上报失败 */
    }   /* if 结束：epoll_ctl 出错分支结束 */

    c->read->active = 0;   /* 读事件标记为未注册 */
    c->write->active = 0;   /* 写事件标记为未注册 */

    return NGX_OK;   /* 注销成功 */
}   /* ngx_epoll_del_connection 结束 */


/*
 * ngx_epoll_process_events：事件驱动核心，每个 worker 的主循环调用一次。
 * 流程：计算定时器超时 →（可选）竞争 accept 互斥锁防"惊群" → epoll_wait
 * 阻塞等待（内核只查就绪链表 rdllist，把就绪事件连同 data.ptr 拷回
 * event_list）→ 逐个还原连接、甄别陈旧事件 → 先写后读分发就绪事件 →
 * 触发到期定时器 → 处理 posted（延迟）事件。
 */
int ngx_epoll_process_events(ngx_cycle_t *cycle)   /* 事件处理主入口 */
{   /* 函数体开始 */
    int                events;   /* epoll_wait 返回值：本次实际就绪的事件个数 */
    size_t             n;   /* 尺寸临时变量（本版本流程中未实际使用） */
    ngx_int_t          instance, i;   /* instance：从指针低位还原的代际标记；i：事件数组下标 */
    ngx_uint_t         lock, accept_lock, expire;   /* lock：是否持有 posted_events 锁；accept_lock：是否持有 accept 锁；expire：是否触发定时器 */
    ngx_err_t          err;   /* epoll_wait 的错误码 */
    ngx_log_t         *log;   /* 本轮使用的日志对象 */
    ngx_msec_t         timer;   /* 传给 epoll_wait 的超时毫秒数（阻塞语义：期间无事件则到时返回 0） */
    ngx_event_t       *rev, *wev;   /* 从连接解出的读/写事件对象 */
    struct timeval     tv;   /* gettimeofday 结果：用于刷新缓存时间 */
    ngx_connection_t  *c;   /* 由事件 data.ptr 还原出的连接对象 */
    ngx_epoch_msec_t   delta;   /* epoll_wait 实际阻塞流逝的毫秒数 */

    for ( ;; ) {   /* 循环：先把已到期定时器耗尽，直到得到可用的非 0 超时 */
        timer = ngx_event_find_timer();   /* 查定时器红黑树：返回最近到期剩余毫秒数；0 表示已有定时器到期 */

#if (NGX_THREADS)   /* 多线程构建：限制单次阻塞时长，保证线程及时调度收割 posted 事件 */

        if (timer == NGX_TIMER_ERROR) {   /* 定时器查询出错 */
            return NGX_ERROR;   /* 终止本轮事件处理 */
        }   /* if 结束：定时器错误分支结束 */

        if (timer == NGX_TIMER_INFINITE || timer > 500) {   /* 无定时器或距到期太远：把阻塞上限压到 500ms */
            timer = 500;   /* 500ms：多线程下不能久睡，需周期性醒来调度线程 */
            break;   /* 得到可用超时，跳出循环 */
        }   /* if 结束：超时上限压缩完毕 */
#endif   /* 结束 NGX_THREADS 条件编译 */

        if (timer != 0) {   /* 有未到期定时器（>0）或无限等待：该超时值可用 */
            break;   /* 跳出循环去执行 epoll_wait */
        }   /* if 结束：超时值可用 */

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,   /* 调试日志：提示有定时器已到期 */
                       "epoll expired timer");   /* 日志消息体 */

        ngx_event_expire_timers((ngx_msec_t)   /* 执行所有已到期定时器的回调（如 keepalive 超时关闭连接） */
                                    (ngx_elapsed_msec - ngx_old_elapsed_msec));   /* 以两轮事件处理之间流逝的毫秒数为时钟增量 */

        if (ngx_posted_events && ngx_threaded) {   /* 多线程且存在待处理的 posted 事件 */
            ngx_wakeup_worker_thread(cycle);   /* 唤醒 worker 线程去消化 posted 事件 */
        }   /* if 结束：posted 事件已唤醒线程 */
    }   /* for(;;) 结束：timer 已确定为非 0 的可用超时 */

    /* NGX_TIMER_INFINITE == INFTIM */

    if (timer == NGX_TIMER_INFINITE) {   /* 超时为无限大 */
        expire = 0;   /* 无限阻塞不可能"超时返回"，无需触发定时器 */

    } else {   /* else 分支：有限超时 */
        expire = 1;   /* 有限超时：wait 返回后需按流逝时间触发到期定时器 */
    }   /* if/else 结束：expire 标志就绪 */

    ngx_old_elapsed_msec = ngx_elapsed_msec;   /* 保存阻塞前的时间戳：供下一轮定时器差值计算 */
    accept_lock = 0;   /* 清零：本轮尚未持有 accept 互斥锁 */

    if (ngx_accept_mutex) {   /* 启用了 accept 互斥锁：多 worker 间防"惊群"，同一时刻仅一个进程 accept */
        if (ngx_accept_disabled > 0) {   /* 本进程连接数超配额（disabled>0）：本轮放弃抢锁 */
            ngx_accept_disabled--;   /* 每轮递减：先冷却再恢复抢锁资格 */

        } else {   /* else 分支：本轮允许尝试抢 accept 锁 */
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) {   /* 尝试非阻塞抢锁：抢到则把监听 socket 注册进本进程 epoll */
                return NGX_ERROR;   /* 锁操作异常则终止本轮 */
            }   /* if 结束：抢锁失败处理完毕 */

            if (ngx_accept_mutex_held) {   /* 抢到锁：监听 fd 已注册进本进程 epoll */
                accept_lock = 1;   /* 标记持有：事件处理完后统一解锁 */

            } else if (timer == NGX_TIMER_INFINITE   /* 未抢到锁且超时为无限 */
                       || timer > ngx_accept_mutex_delay)   /* 或超时大于锁释放周期 */
            {   /* 压缩超时的分支开始 */
                timer = ngx_accept_mutex_delay;   /* 把超时压缩到 accept_mutex_delay：确保按周期重试抢锁 */
                expire = 0;   /* 这次缩短的等待并非定时器到期：不触发定时器 */
            }   /* if/else if 结束：未抢到锁时的重试等待已安排 */
        }   /* else 结束：本轮抢锁流程完毕 */
    }   /* if (ngx_accept_mutex) 结束：accept 锁处理完毕 */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,   /* 调试日志：输出最终超时值 */
                   "epoll timer: %d", timer);   /* 日志格式串与参数：timer 毫秒数 */

    events = epoll_wait(ep, event_list, nevents, timer);   /* 核心：陷入内核阻塞等待。只检查 rdllist 就绪链表并把事件拷回 event_list，开销 O(就绪数) 与总连接数无关——C10K 的答案 */

    if (events == -1) {   /* 返回 -1：出错（典型为被信号打断的 EINTR） */
        err = ngx_errno;   /* 保存内核错误码 */
    } else {   /* else 分支：epoll_wait 正常返回 */
        err = 0;   /* 正常返回则清零错误码 */
    }   /* if/else 结束：错误码就绪 */

    ngx_gettimeofday(&tv);   /* 取墙钟时间：wait 返回后立即刷新 */
    ngx_time_update(tv.tv_sec);   /* 更新 nginx 缓存的秒级时间与日志时间串 */

    delta = ngx_elapsed_msec;   /* 先记下刚刷新的时间戳：用于计算本轮实际阻塞时长 */
    ngx_elapsed_msec = (ngx_epoch_msec_t) tv.tv_sec * 1000   /* 把当前时刻换算为相对进程启动的毫秒数 */
                                          + tv.tv_usec / 1000 - ngx_start_msec;   /* 微秒折算成毫秒后减去进程启动基准 */

    if (timer != NGX_TIMER_INFINITE) {   /* 有限超时才需要核对流逝时间 */
        delta = ngx_elapsed_msec - delta;   /* delta 即 epoll_wait 真实阻塞的毫秒数 */

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,   /* 调试日志：输出设定超时与实际流逝 */
                       "epoll timer: %d, delta: %d", timer, (int) delta);   /* 日志格式串与参数：timer、delta */
    } else {   /* else 分支：无限超时 */
        if (events == 0) {   /* 无限等待却返回 0 事件：违反 epoll_wait 语义，必为内核异常 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,   /* ALERT 级别报告异常 */
                          "epoll_wait() returned no events without timeout");   /* 日志消息体 */
            ngx_accept_mutex_unlock();   /* 释放可能持有的 accept 锁 */
            return NGX_ERROR;   /* 终止本轮 */
        }   /* if 结束：events==0 异常分支结束 */
    }   /* if (timer != NGX_TIMER_INFINITE) 结束：流逝时间核对完毕 */

    if (err) {   /* epoll_wait 返回了错误 */
        ngx_log_error((err == NGX_EINTR) ? NGX_LOG_INFO : NGX_LOG_ALERT,   /* EINTR（信号打断，属正常现象）降为 INFO，其余为 ALERT */
                      cycle->log, err, "epoll_wait() failed");   /* 日志参数与消息体 */
        ngx_accept_mutex_unlock();   /* 释放 accept 锁 */
        return NGX_ERROR;   /* 终止本轮 */
    }   /* if (err) 结束：错误处理完毕 */

    if (events > 0) {   /* 有就绪事件：遍历中要向共享队列投递事件，需先加锁 */
        if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {   /* 锁 posted 事件互斥锁：保护多线程共享的事件队列 */
            ngx_accept_mutex_unlock();   /* 加锁失败先释放 accept 锁 */
            return NGX_ERROR;   /* 终止本轮 */
        }   /* if 结束：posted 互斥锁加锁失败分支结束 */

        lock = 1;   /* 标记已持有 posted_events 锁：结尾统一释放 */

    } else {   /* else 分支：无就绪事件 */
        lock =0;   /* 无事件无需加锁 */
    }   /* if/else 结束：lock 标志就绪 */

    log = cycle->log;   /* 默认使用全局 cycle 日志 */

    for (i = 0; i < events; i++) {   /* 逐个处理 epoll_wait 拷回的就绪事件 */
        c = event_list[i].data.ptr;   /* 取出注册时存入的"连接指针|instance"原始值 */

        instance = (uintptr_t) c & 1;   /* 最低位是 instance 代际标记：识别同一内存被新连接复用前的陈旧事件 */
        c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);   /* 清掉最低位：还原真实连接指针 */

        rev = c->read;   /* 取该连接的读事件对象 */

        if (c->fd == -1 || rev->instance != instance) {   /* fd 已关闭或代际不符：这是刚被 close 的 fd 遗留的陈旧事件 */

            /*
             * the stale event from a file descriptor
             * that was just closed in this iteration
             */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,   /* 调试日志：报告陈旧事件的连接指针 */
                           "epoll: stale event " PTR_FMT, c);   /* 日志格式串与参数：连接指针 */
            continue;   /* 丢弃该事件，继续处理下一个 */
        }   /* if 结束：陈旧事件已跳过 */

#if (NGX_DEBUG0)   /* 仅在特定调试构建下 */
        log = c->log ? c->log : cycle->log;   /* 优先使用连接自身日志，未设置则退回全局日志 */
#endif   /* 结束 NGX_DEBUG0 条件编译 */

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, log, 0,   /* 调试日志：输出 fd、事件掩码与用户数据 */
                       "epoll: fd:%d ev:%04X d:" PTR_FMT,   /* 日志格式串 */
                       c->fd, event_list[i].events, event_list[i].data);   /* 日志参数：fd、掩码、data */

        if (event_list[i].events & (EPOLLERR|EPOLLHUP)) {   /* 掩码含错误/挂断位：对端发来 RST 或已关闭连接 */
            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, log, 0,   /* 调试日志：记录出错 fd 与原始掩码 */
                           "epoll_wait() error on fd:%d ev:%04X",   /* 日志格式串 */
                           c->fd, event_list[i].events);   /* 日志参数：fd、掩码 */
        }   /* if 结束：ERR/HUP 已记录日志 */

        if (event_list[i].events & ~(EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {   /* 出现了预期之外的事件位 */

            ngx_log_error(NGX_LOG_ALERT, log, 0,   /* ALERT 报告"陌生"事件：帮助发现内核行为异常 */
                          "strange epoll_wait() events fd:%d ev:%04X",   /* 日志消息体 */
                          c->fd, event_list[i].events);   /* 日志参数：fd、掩码 */
        }   /* if 结束：陌生事件位已报告 */

        wev = c->write;   /* 取写事件对象：先处理可写 */

        if ((event_list[i].events & (EPOLLOUT|EPOLLERR|EPOLLHUP))   /* 可写或错误/挂断（错误也视作状态变化以唤醒写处理） */
            && wev->active)   /* 且写事件处于注册状态 */
        {   /* 写就绪处理块开始 */
            if (ngx_threaded) {   /* 多线程模式：写事件不由本线程立即处理 */
                wev->posted_ready = 1;   /* 标记写已就绪 */
                ngx_post_event(wev);   /* 挂入 posted 事件队列交线程池处理 */

            } else {   /* else 分支：单线程模式 */
                wev->ready = 1;   /* 单线程：置写就绪标志，处理函数可放心 write 而不必再探测 */

                if (!ngx_accept_mutex_held) {   /* 未持有 accept 锁：事件归属本进程，直接处理 */
                    wev->event_handler(wev);   /* 调用写事件处理器（如继续发送响应数据） */

                } else {   /* else 分支：持有 accept 锁，延迟处理 */
                    ngx_post_event(wev);   /* 持有 accept 锁：为缩短锁持有时间，延迟到锁外统一处理 */
                }   /* if/else 结束：写事件处理器已调用或已入队 */
            }   /* if/else 结束：单线程/多线程路径分岔完毕 */
        }   /* 写就绪处理块结束 */

        /*
         * EPOLLIN must be handled after EPOLLOUT because we use
         * the optimization to avoid the unnecessary mutex locking/unlocking
         * if the accept event is the last one.
         */

        if ((event_list[i].events & (EPOLLIN|EPOLLERR|EPOLLHUP))   /* 可读或错误/挂断（对端关闭也表现为可读：read 返回 0 或 ECONNRESET） */
            && rev->active)   /* 且读事件处于注册状态 */
        {   /* 读就绪处理块开始 */
            if (ngx_threaded && !rev->accept) {   /* 多线程且非监听事件：转交线程处理 */
                rev->posted_ready = 1;   /* 标记读已就绪 */

                ngx_post_event(rev);   /* 挂入 posted 事件队列 */

                continue;   /* 本事件转交完毕：处理下一个就绪事件 */
            }   /* if 结束：多线程读事件已转交 */

            rev->ready = 1;   /* 置读就绪标志：接收缓冲区确有数据/监听全连接队列非空 */

            if (!ngx_threaded && !ngx_accept_mutex_held) {   /* 单线程且未持 accept 锁：事件全归本进程 */
                rev->event_handler(rev);   /* 直接调用读事件处理器（读数据或 accept 新连接） */

            } else if (!rev->accept) {   /* 普通连接的读事件（非监听 socket） */
                ngx_post_event(rev);   /* 延迟到锁外/posted 阶段处理，减少锁持有时间 */

            } else if (ngx_accept_disabled <= 0) {   /* 是监听读事件（accept）且本进程尚未超载 */

                ngx_mutex_unlock(ngx_posted_events_mutex);   /* 先释放 posted 互斥锁：accept 建新连接可能再触发加锁，避免嵌套死锁 */

                rev->event_handler(rev);   /* 立即执行 accept：把内核全连接队列中已完成三次握手的连接取入本进程 */

                if (ngx_accept_disabled > 0) {   /* accept 后发现连接数超载 */
                    ngx_accept_mutex_unlock();   /* 主动释放 accept 锁：把接新连接的机会让给其他 worker */
                    accept_lock = 0;   /* 同步本地标记：锁已不在手 */
                }   /* if 结束：超载后已让出 accept 锁 */

                if (i + 1 == events) {   /* 这已是本轮最后一个事件 */
                    lock = 0;   /* 结尾无需再释放 posted 锁（上方已释放） */
                    break;   /* 提前结束遍历：配合"accept 事件放最后"的锁优化（见上方英文注释） */
                }   /* if 结束：最后一个事件的提前退出已处理 */

                if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {   /* accept 完毕重新拿回 posted 互斥锁 */
                    if (accept_lock) {   /* 若仍持有 accept 锁 */
                        ngx_accept_mutex_unlock();   /* 先释放 accept 锁，避免其他 worker 长期抢不到 */
                    }   /* if 结束：accept 锁已释放 */
                    return NGX_ERROR;   /* 终止本轮 */
                }   /* if 结束：重新加锁失败分支结束 */
            }   /* else if 结束：accept 监听事件处理完毕 */
        }   /* 读就绪处理块结束 */
    }   /* for 结束：全部就绪事件处理完毕 */

    if (accept_lock) {   /* 本轮抢到过 accept 锁 */
        ngx_accept_mutex_unlock();   /* 处理完监听事件后释放：让其他 worker 也能周期性 accept */
    }   /* if 结束：accept 锁释放完毕 */

    if (lock) {   /* 遍历前加过 posted 互斥锁 */
        ngx_mutex_unlock(ngx_posted_events_mutex);   /* 统一释放 */
    }   /* if 结束：posted 互斥锁释放完毕 */

    if (expire && delta) {   /* 有限超时且确有流逝时间 */
        ngx_event_expire_timers((ngx_msec_t) delta);   /* 按流逝毫秒数触发到期定时器回调 */
    }   /* if 结束：到期定时器已触发 */

    if (ngx_posted_events) {   /* 存在被延迟处理的事件 */
        if (ngx_threaded) {   /* 多线程模式 */
            ngx_wakeup_worker_thread(cycle);   /* 唤醒 worker 线程取走处理 */

        } else {   /* else 分支：单线程就地消化 posted 队列 */
            ngx_event_process_posted(cycle);   /* 单线程：就地依次执行 posted 队列中的事件 */
        }   /* if/else 结束：posted 事件处理方式已选定 */
    }   /* if (ngx_posted_events) 结束：延迟事件处理完毕 */

    return NGX_OK;   /* 本轮事件处理完成 */
}   /* ngx_epoll_process_events 结束 */


/*
 * ngx_epoll_create_conf：配置解析前的钩子——从内存池分配模块配置结构。
 */
static void *ngx_epoll_create_conf(ngx_cycle_t *cycle)   /* 配置创建入口 */
{   /* 函数体开始 */
    ngx_epoll_conf_t  *epcf;   /* 本模块配置结构指针 */

    ngx_test_null(epcf, ngx_palloc(cycle->pool, sizeof(ngx_epoll_conf_t)),   /* 从进程内存池分配配置结构，失败则返回配置错误 */
                  NGX_CONF_ERROR);   /* 宏的第二参数：分配失败时宏返回此值 */

    epcf->events = NGX_CONF_UNSET;   /* events 置为 UNSET：标记未被指令赋值，供 init 阶段识别并补默认值 */

    return epcf;   /* 返回配置结构：框架将其挂入配置上下文树 */
}   /* ngx_epoll_create_conf 结束 */


/*
 * ngx_epoll_init_conf：配置解析完成后的钩子——为未设置的字段补默认值。
 */
static char *ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf)   /* 配置初始化入口 */
{   /* 函数体开始 */
    ngx_epoll_conf_t *epcf = conf;   /* 把 void* 还原为本模块配置指针 */

    ngx_conf_init_unsigned_value(epcf->events, 512);   /* 未配置时默认 512：单次 epoll_wait 最多拷回 512 个就绪事件 */

    return NGX_CONF_OK;   /* 返回 OK 表示配置合法 */
}   /* ngx_epoll_init_conf 结束 */
