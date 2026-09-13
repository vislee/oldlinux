
/*
 * Copyright (C) Igor Sysoev
 */


#ifndef _NGX_EVENT_H_INCLUDED_   /* 头文件保护宏开始：防止本头文件被重复包含而引发类型重定义 */
#define _NGX_EVENT_H_INCLUDED_   /* 首次包含时定义保护宏，供上面的 #ifndef 在后续包含时短路 */


#include <ngx_config.h>   /* 编译期平台配置头：提供 HAVE_EPOLL/HAVE_KQUEUE 等特性宏，决定下面的平台裁剪 */
#include <ngx_core.h>   /* nginx 核心头：ngx_log_t/ngx_cycle_t 等基础类型定义 */


#define NGX_INVALID_INDEX  0xd0d0d0d0   /* 非法下标哨兵值(0xd0d0d0d0)：标记事件未挂入事件模块的表，便于识别误用 */


#if (HAVE_IOCP)   /* 条件编译：仅 Windows IOCP(完成端口)平台编译以下结构 */

    /* IOCP 重叠 I/O 辅助结构：把 nginx 事件挂到 Win32 OVERLAPPED 上，
       内核完成端口完成 I/O 后凭它找回对应的 ngx_event_t */
typedef struct {   /* 结构定义开始 */
    WSAOVERLAPPED    ovlp;   /* Win32 重叠 I/O 头：内核异步操作的凭证，完成包携带它返回 */
    ngx_event_t     *event;   /* 反向指针：由 OVERLAPPED 地址回推所属事件(容器偏移思路) */
    int              error;   /* 本次异步操作的完成错误码 */
} ngx_event_ovlp_t;   /* 结构类型名 ngx_event_ovlp_t */

#endif   /* 条件编译结束 */


    /* 事件互斥锁(线程模式)：保护多线程共享的事件队列(posted 队列)；
       lock 非零即被占用，配合 events/last 实现 O(1) 追加的链表 */
typedef struct {   /* 结构定义开始 */
    ngx_uint_t       lock;   /* 锁标志：0 空闲、非 0 持有(自旋等待语义) */

    ngx_event_t     *events;   /* 被保护事件队列的头指针 */
    ngx_event_t     *last;   /* 队列尾指针：入队无需遍历 */
} ngx_event_mutex_t;   /* 结构类型名 ngx_event_mutex_t */


/*
 * ngx_event_t —— 事件驱动模型的核心对象：每个 fd 的每个方向(读/写)各一个。
 * active=已注册进 epoll/select 关注集；ready=内核已报告就绪；
 * 超时事件经 rbtree_key 挂入定时器红黑树(基于缓存时钟 ngx_current_msec)；
 * 就绪事件先链入 posted 队列延迟处理——先收集完本轮全部就绪事件再统一执行，
 * 防止处理 A 事件时关闭了 B 连接、队列中残留悬空指针(事件驱动经典设计)。
 */
struct ngx_event_s {   /* 事件结构定义 */
    void            *data;   /* 上下文指针：通常指向所属连接，handler 回调中由此取回操作对象 */

    unsigned         write:1;   /* 事件方向：1=写事件，0=读事件(同一 fd 的读/写是两个独立事件) */

    unsigned         accept:1;   /* 监听 socket 读事件标记：其 handler 最终调用 accept() 而非 recv() */

    unsigned         oneshot:1;   /* 一次性事件：内核通知一次后自动解除关注，需重新注册(防重复处理/惊群) */

    /* used to detect the stale events in kqueue, rt signals and epoll */
    unsigned         instance:1;   /* 代际标记：注册时写入与内核返回值互补的位；对不上即陈旧事件(fd 已复用/事件过期)，直接丢弃 */

    /*
     * the event was passed or would be passed to a kernel;
     * in aio mode - operation was posted.
     */
    unsigned         active:1;   /* 事件已注册进内核关注集(已执行 epoll_ctl ADD)，防止重复注册 */

    unsigned         disabled:1;   /* 事件被暂时屏蔽(仍在关注集但不通知)，恢复比删除+重注册便宜 */

    /* the ready event; in aio mode 0 means that no operation can be posted */
    unsigned         ready:1;   /* 就绪标志：本轮事件收集中内核已报告可读/可写，处理完清零 */

    /* aio operation is complete */
    unsigned         complete:1;   /* AIO 操作已完成：数据已落用户缓冲区，等待业务消费 */

    unsigned         eof:1;   /* 对端已正常关闭(收到 FIN)：读完残余数据后走关闭流程 */
    unsigned         error:1;   /* 事件上发生错误(收到 RST/内核报错)，处理时走错误路径 */

    unsigned         timedout:1;   /* 定时器已超时：到期置位，驱动连接超时处理(防慢速连接占满资源) */
    unsigned         timer_set:1;   /* 已挂入定时器红黑树：删除/重设前需先摘除，避免二次删除破坏树 */

    unsigned         delayed:1;   /* 处理被延迟(如限速等待)，事件挂起待后续轮次执行 */

    unsigned         read_discarded:1;   /* 丢弃读模式：只把数据读走丢掉、不交给协议层(决定关闭前的排水) */

    unsigned         unexpected_eof:1;   /* 非预期 EOF：协议未结束对端就关闭，与正常 keepalive 关闭相区分 */

    unsigned         deferred_accept:1;   /* 已启用 TCP_DEFER_ACCEPT：accept 到的连接必已携带首包数据 */

    /* the pending eof reported by kqueue or in aio chain operation */
    unsigned         pending_eof:1;   /* kqueue/IOCP 链式操作报告的"EOF 待处理"：数据与 FIN 一起返回，读完再关 */

#if !(NGX_THREADS)   /* 条件编译：单线程模式(无线程时才用该标志) */
    unsigned         posted_ready:1;   /* 就绪标志的 post 延迟位：先收集后统一处理，减少反复入队 */
#endif   /* 条件编译结束 */

#if (WIN32)   /* 条件编译：仅 Windows */
    /* setsockopt(SO_UPDATE_ACCEPT_CONTEXT) was succesfull */
    unsigned         accept_context_updated:1;   /* AcceptEx 建立的 socket 已更新 accept 上下文，此后才能正常收发 */
#endif   /* 条件编译结束 */

#if (HAVE_KQUEUE)   /* 条件编译：kqueue(BSD) 平台 */
    unsigned         kq_vnode:1;   /* kqueue vnode 事件：fd 为文件(非 socket)时的文件变动通知 */

    /* the pending errno reported by kqueue */
    int              kq_errno;   /* kqueue 随事件返回的错误码(EV_ERROR 场景) */
#endif   /* 条件编译结束 */

    /*
     * kqueue only:
     *   accept:     number of sockets that wait to be accepted
     *   read:       bytes to read when event is ready
     *               or lowat when event is set with NGX_LOWAT_EVENT flag
     *   write:      available space in buffer when event is ready
     *               or lowat when event is set with NGX_LOWAT_EVENT flag
     *
     * iocp: TODO
     *
     * otherwise:
     *   accept:     1 if accept many, 0 otherwise
     */

#if (HAVE_KQUEUE) || (HAVE_IOCP)   /* 条件编译：kqueue/IOCP 能提供整型附加信息 */
    int              available;   /* kqueue 附加信息：可读字节数/待 accept 连接数(内核直接告知，省一次 ioctl) */
#else   /* 否则分支 */
    unsigned         available:1;   /* 其他平台退化为 1 位标志：仅表示"可以连续 accept 多个连接" */
#endif   /* 条件编译结束 */

    /* TODO rename to handler */
    ngx_event_handler_pt  event_handler;   /* 事件回调函数指针：读事件→accept/recv，写事件→writev/sendfile，事件分发的总入口 */


#if (HAVE_AIO)   /* 条件编译：启用异步 I/O */

#if (HAVE_IOCP)   /* 条件编译：Windows IOCP */
    ngx_event_ovlp_t ovlp;   /* IOCP 重叠结构(见 ngx_event_ovlp_t) */
#else   /* 否则分支 */
    struct aiocb     aiocb;   /* POSIX AIO 控制块：描述一次异步读(缓冲区/偏移/通知方式) */
#endif   /* HAVE_IOCP 条件结束 */

#endif   /* HAVE_AIO 条件结束 */

    u_int            index;   /* 事件在事件模块私有数组(如 select 的 event_index)中的下标，便于 O(1) 定位 */

    ngx_log_t       *log;   /* 事件日志：连接销毁后事件可能仍在队列，自带日志以免悬空引用 */

    /* TODO: threads: padding to cache line */

    /*
     * STUB: The inline of "ngx_rbtree_t  rbtree;"
     */

    ngx_int_t        rbtree_key;   /* 定时器红黑树的键：绝对超时时刻(缓存时钟的毫秒值)；树最左节点即最近到期事件 */
    void            *rbtree_left;   /* 红黑树左孩子指针(把 rbtree 字段内联进事件，避免类型依赖) */
    void            *rbtree_right;   /* 红黑树右孩子指针 */
    void            *rbtree_parent;   /* 红黑树父节点指针 */
    char             rbtree_color;   /* 节点颜色(红/黑)：维护黑高平衡，保证插入删除 O(log n) */


    unsigned         closed:1;   /* 所属连接已关闭：队列中残留的本事件处理时直接丢弃，防 use-after-free */

#if (NGX_THREADS)   /* 条件编译：多线程模式 */

    unsigned         locked:1;   /* 事件已被某工作线程认领 */

    unsigned         posted_ready:1;   /* post 阶段传递的就绪标志(对应无线程版的 posted_ready) */
    unsigned         posted_timedout:1;   /* post 阶段传递的超时标志 */
    unsigned         posted_eof:1;   /* post 阶段传递的 EOF 标志 */

#if (HAVE_KQUEUE)   /* 条件编译：kqueue */
    /* the pending errno reported by kqueue */
    int              posted_errno;   /* post 阶段传递的 kqueue 错误码 */
#endif   /* 条件编译结束 */

#if (HAVE_KQUEUE) || (HAVE_IOCP)   /* 条件编译：kqueue/IOCP 整型 */
    int              posted_available;   /* post 阶段传递的整型 available */
#else   /* 否则分支 */
    unsigned         posted_available:1;   /* post 阶段传递的 1 位 available */
#endif   /* 条件编译结束 */

    ngx_atomic_t    *lock;   /* 指向共享内存原子锁：多线程/多进程同步对本事件的访问 */
    ngx_atomic_t    *own_lock;   /* 事件私有的锁存储位置 */

#endif   /* NGX_THREADS 条件结束 */

    /* the links of the posted queue */
    ngx_event_t     *next;   /* posted 延迟队列的后驱指针：先收集全部就绪事件再统一执行 */
    ngx_event_t    **prev;   /* 前驱指针的指针(二级指针)：O(1) 摘链且无需特判队头 */


#if 0   /* 条件编译：整块停用(#if 0) */

    /* the threads support */

    /*
     * the event thread context, we store it here
     * if $(CC) does not understand __thread declaration
     * and pthread_getspecific() is too costly
     */

    void            *thr_ctx;   /* (停用)线程上下文：编译器不支持 __thread 且 pthread_getspecific 太贵时的替代存储 */

#if (NGX_EVENT_T_PADDING)   /* 条件编译：需要缓存行填充 */

    /* event should not cross cache line in SMP */

    int              padding[NGX_EVENT_T_PADDING];   /* (停用)缓存行填充：SMP 下防止事件跨缓存行造成伪共享(false sharing) */
#endif   /* NGX_EVENT_T_PADDING 条件结束 */
#endif   /* #if 0 块结束 */
};   /* ngx_event_s 结构定义结束 */


/*
 * ngx_event_actions_t —— 事件模块动作函数表：I/O 多路复用的可插拔多态。
 * epoll/select/poll/kqueue 等事件模块各实现一份本表，模块初始化时写入全局
 * ngx_event_actions；框架层只面向本接口编程，事件循环完全同构。
 */
typedef struct {   /* 事件动作接口表定义 */
    ngx_int_t  (*add)(ngx_event_t *ev, int event, u_int flags);   /* add：把事件注册进内核关注集(如 epoll_ctl 的 EPOLL_CTL_ADD) */
    ngx_int_t  (*del)(ngx_event_t *ev, int event, u_int flags);   /* del：从内核关注集移除事件 */

    ngx_int_t  (*enable)(ngx_event_t *ev, int event, u_int flags);   /* enable：恢复被屏蔽的事件 */
    ngx_int_t  (*disable)(ngx_event_t *ev, int event, u_int flags);   /* disable：暂时屏蔽事件(如释放 accept 锁前禁监听读事件) */

    ngx_int_t  (*add_conn)(ngx_connection_t *c);   /* add_conn：以连接为单位同时注册读+写两个事件 */
    ngx_int_t  (*del_conn)(ngx_connection_t *c, u_int flags);   /* del_conn：以连接为单位注销读+写两个事件 */

    ngx_int_t  (*process_changes)(ngx_cycle_t *cycle, ngx_uint_t try);   /* process_changes：批量提交积攒的事件变更(kqueue 的 kevent 批处理)；try 表示尽力而为 */
    ngx_int_t  (*process_events)(ngx_cycle_t *cycle);   /* process_events：一轮事件循环主体：等待内核就绪队列并分发就绪事件 */

    ngx_int_t  (*init)(ngx_cycle_t *cycle);   /* init：模块初始化(如 epoll_create 创建内核事件表) */
    void       (*done)(ngx_cycle_t *cycle);   /* done：模块清理：关闭事件表并释放资源 */
} ngx_event_actions_t;   /* 接口表类型名 ngx_event_actions_t */


extern ngx_event_actions_t   ngx_event_actions;   /* 全局动作表实例：被选中的事件模块初始化时填充，框架经它调用具体后端 */


/*
 * The event filter requires to read/write the whole data -
 * select, poll, /dev/poll, kqueue, epoll.
 */
#define NGX_USE_LEVEL_EVENT      0x00000001   /* 水平触发(LT)语义标志：select/poll/epoll-LT——条件满足时每轮都报告，未读完下轮继续提醒 */

/*
 * The event filter is deleted after a notification without an additional
 * syscall - select, poll, kqueue, epoll, Solaris 10's event ports.
 */
#define NGX_USE_ONESHOT_EVENT    0x00000002   /* 一次性通知语义标志：报告一次后自动解除关注(需重新注册)，防同一事件重复处理 */

/*
 * The event filter notifies only the changes and an initial level -
 * kqueue, epoll.
 */
#define NGX_USE_CLEAR_EVENT      0x00000004   /* 边缘触发(ET)语义标志：仅状态变化通知一次(EV_CLEAR/EPOLLET)，必须循环读写到 EAGAIN */

/*
 * The event filter has kqueue features - the eof flag, errno,
 * available data, etc.
 */
#define NGX_HAVE_KQUEUE_EVENT    0x00000008   /* kqueue 增强信息标志：事件附带 EOF 标志、errno、可读字节数 */

/*
 * The event filter supports low water mark - kqueue's NOTE_LOWAT.
 * kqueue in FreeBSD 4.1-4.2 has no NOTE_LOWAT so we need a separate flag.
 */
#define NGX_HAVE_LOWAT_EVENT     0x00000010   /* 支持低水位标记(NOTE_LOWAT)：内核缓冲积累到阈值才唤醒，减少无效唤醒 */

/*
 * The event filter requires to do i/o operation until EAGAIN -
 * epoll, rt signals.
 */
#define NGX_HAVE_GREEDY_EVENT    0x00000020   /* 贪婪 I/O 标志：必须读写到 EAGAIN，否则丢失后续通知(epoll-ET/rt signals) */

/*
 * The event filter is epoll,
 */
#define NGX_USE_EPOLL_EVENT      0x00000040   /* 事件模型为 epoll：Linux 大规模连接下的高效就绪通知机制 */

/*
 * No need to add or delete the event filters - rt signals.
 */
#define NGX_USE_RTSIG_EVENT      0x00000080   /* 实时信号(SIGIO)模型：信号携带 fd，无需关注集的注册/注销 */

/*
 * No need to add or delete the event filters - overlapped, aio_read,
 * aioread, io_submit.
 */
#define NGX_USE_AIO_EVENT        0x00000100   /* 异步 I/O 模型：提交即返回、完成时通知，fd 无需进入事件表 */

/*
 * Need to add socket or handle only once - i/o completion port.
 * It also requires HAVE_AIO and NGX_USE_AIO_EVENT to be set.
 */
#define NGX_USE_IOCP_EVENT       0x00000200   /* Windows 完成端口模型：句柄一次性关联，完成包入队由线程池消费 */



/*
 * The event filter is deleted before the closing file.
 * Has no meaning for select, poll, epoll.
 *
 * kqueue:     kqueue deletes event filters for file that closed
 *             so we need only to delete filters in user-level batch array
 * /dev/poll:  we need to flush POLLREMOVE event before closing file
 */

#define NGX_CLOSE_EVENT    1   /* 传给 del 接口的标志：关闭 fd 前需显式移除关注(kqueue/dev/poll 需要) */
#define NGX_DISABLE_EVENT  2   /* 传给 del 接口的标志：仅屏蔽事件而不真正删除 */


/* these flags have a meaning only for kqueue */
#define NGX_LOWAT_EVENT    0   /* (仅 kqueue 有效)按 NOTE_LOWAT 低水位注册；其他平台为 0 占位 */
#define NGX_VNODE_EVENT    0   /* (仅 kqueue 有效)vnode 文件事件过滤器；其他平台为 0 占位 */


#if (HAVE_KQUEUE)   /* 条件编译：kqueue 平台——事件类型直接映射 kevent 的 filter 值 */

#define NGX_READ_EVENT     EVFILT_READ   /* 读事件 = kqueue 读过滤器 EVFILT_READ */
#define NGX_WRITE_EVENT    EVFILT_WRITE   /* 写事件 = kqueue 写过滤器 EVFILT_WRITE */

#undef  NGX_VNODE_EVENT   /* 取消前面的占位定义 */
#define NGX_VNODE_EVENT    EVFILT_VNODE   /* vnode 事件 = kqueue 文件系统过滤器 EVFILT_VNODE */

/*
 * NGX_CLOSE_EVENT and NGX_LOWAT_EVENT are the module flags and they would
 * not go into a kernel so we need to choose the value that would not interfere
 * with any existent and future kqueue flags.  kqueue has such values -
 * EV_FLAG1, EV_EOF and EV_ERROR.  They are reserved and cleared on a kernel
 * entrance.
 */
#undef  NGX_CLOSE_EVENT   /* 取消占位值，改借 kqueue 保留位 */
#define NGX_CLOSE_EVENT    EV_EOF   /* 借用 EV_EOF 位作模块内部标志：这些位在内核入口被清除，不干扰真实 kevent 调用 */

#undef  NGX_LOWAT_EVENT   /* 取消占位值 */
#define NGX_LOWAT_EVENT    EV_FLAG1   /* 借用 EV_FLAG1 位作为低水位标志 */

#define NGX_LEVEL_EVENT    0   /* kqueue 默认水平触发(不加 EV_CLEAR 即 LT) */
#define NGX_ONESHOT_EVENT  EV_ONESHOT   /* 一次性 = EV_ONESHOT：通知一次自动移除 */
#define NGX_CLEAR_EVENT    EV_CLEAR   /* 边缘触发 = EV_CLEAR：状态清空型通知 */

#undef  NGX_DISABLE_EVENT   /* 取消占位值 */
#define NGX_DISABLE_EVENT  EV_DISABLE   /* 屏蔽事件 = EV_DISABLE：保留注册但不通知 */


#elif (HAVE_DEVPOLL)   /* 分支：Solaris /dev/poll 平台 */

#define NGX_READ_EVENT     POLLIN   /* 读事件 = poll 读就绪位 POLLIN */
#define NGX_WRITE_EVENT    POLLOUT   /* 写事件 = poll 写就绪位 POLLOUT */

#define NGX_LEVEL_EVENT    0   /* /dev/poll 仅有水平触发语义 */
#define NGX_ONESHOT_EVENT  1   /* 一次性标志(用户态模拟) */


#elif (HAVE_EPOLL)   /* 分支：Linux epoll 平台 */

#define NGX_READ_EVENT     EPOLLIN   /* 读事件 = EPOLLIN：对端数据到达/FIN */
#define NGX_WRITE_EVENT    EPOLLOUT   /* 写事件 = EPOLLOUT：发送缓冲区有空间 */

#define NGX_LEVEL_EVENT    0   /* epoll 默认 LT(不设 EPOLLET 即水平触发) */
#define NGX_CLEAR_EVENT    EPOLLET   /* 边缘触发 = EPOLLET：仅状态跳变通知一次，高并发下的低开销模式 */
#define NGX_ONESHOT_EVENT  0x70000000   /* 一次性标志取自定义值 0x70000000：避开内核已占用的 epoll 标志位 */
#if 0   /* 条件编译(停用) */
#define NGX_ONESHOT_EVENT  EPOLLONESHOT   /* (停用)内核 EPOLLONESHOT：其语义与 nginx 的处理流程不合 */
#endif   /* 条件编译结束 */


#elif (HAVE_POLL)   /* 分支：POSIX poll 平台 */

#define NGX_READ_EVENT     POLLIN   /* 读事件 = POLLIN */
#define NGX_WRITE_EVENT    POLLOUT   /* 写事件 = POLLOUT */

#define NGX_LEVEL_EVENT    0   /* poll 天然水平触发 */
#define NGX_ONESHOT_EVENT  1   /* 一次性标志(用户态模拟) */


#else /* select */   /* 兜底分支：select——所有 Unix 皆备，但受 FD_SETSIZE(1024) 限制 */

#define NGX_READ_EVENT     0   /* 读事件用编号 0(仅作内部数组下标区分) */
#define NGX_WRITE_EVENT    1   /* 写事件用编号 1 */

#define NGX_LEVEL_EVENT    0   /* select 每轮全量扫描 fd 集，天然水平触发 */
#define NGX_ONESHOT_EVENT  1   /* 一次性标志(用户态模拟) */

#endif /* HAVE_KQUEUE */   /* 事件类型宏平台映射结束 */


#if (HAVE_IOCP)   /* 条件编译：Windows IOCP */
#define NGX_IOCP_ACCEPT      0   /* 完成包类型：AcceptEx 建连完成 */
#define NGX_IOCP_IO          1   /* 完成包类型：普通读/写完成 */
#define NGX_IOCP_CONNECT     2   /* 完成包类型：ConnectEx 主动建连完成 */
#endif   /* 条件编译结束 */


#ifndef NGX_CLEAR_EVENT   /* 若前面的平台映射未定义 NGX_CLEAR_EVENT… */
#define NGX_CLEAR_EVENT    0    /* dummy declaration */   /* …定义为 0 占位：无 ET 概念的平台传入 0 无副作用 */
#endif   /* 防重定义保护结束 */


#define ngx_process_changes  ngx_event_actions.process_changes   /* 别名宏：提交批量事件变更，转调当前事件模块的实现 */
#define ngx_process_events   ngx_event_actions.process_events   /* 别名宏：执行一轮事件等待与分发(事件循环的心脏) */
#define ngx_done_events      ngx_event_actions.done   /* 别名宏：事件模块收尾清理 */

#define ngx_add_event        ngx_event_actions.add   /* 别名宏：注册单个事件 */
#define ngx_del_event        ngx_event_actions.del   /* 别名宏：注销单个事件 */
#define ngx_add_conn         ngx_event_actions.add_conn   /* 别名宏：按连接注册读写事件 */
#define ngx_del_conn         ngx_event_actions.del_conn   /* 别名宏：按连接注销读写事件 */

#define ngx_add_timer        ngx_event_add_timer   /* 定时器别名：事件按超时时刻挂入定时器红黑树 */
#define ngx_del_timer        ngx_event_del_timer   /* 定时器别名：从定时器红黑树摘除 */


#define ngx_recv             ngx_io.recv   /* 收包别名：经全局 I/O 表调用当前平台的读实现 */
#define ngx_recv_chain       ngx_io.recv_chain   /* 聚集读别名：一次读入多块缓冲(iovec) */
#define ngx_send             ngx_io.send   /* 发包别名：当前平台的写实现 */
#define ngx_send_chain       ngx_io.send_chain   /* 聚集写别名：writev/sendfile 式多块发送，减少系统调用次数 */



#define NGX_EVENT_MODULE      0x544E5645  /* "EVNT" */   /* 事件模块类型签名(ASCII "EVNT")：模块注册时声明所属类别 */
#define NGX_EVENT_CONF        0x02000000   /* 事件模块配置块标志：标识 events{} 内的配置指针 */


/*
 * ngx_event_conf_t —— 事件核心模块配置：nginx.conf 中 events{} 指令的
 * 解析结果，控制连接池规模、事件模型选择与 accept 惊群互斥策略。
 */
typedef struct {   /* 配置结构定义 */
    ngx_uint_t    connections;   /* connections 指令：每 worker 连接池上限(对应内核 fd 消耗与事件表规模) */
    ngx_uint_t    use;   /* use 指令：选用的事件模块在模块数组中的序号 */

    ngx_flag_t    multi_accept;   /* multi_accept 指令：一次通知把全连接队列 accept 尽(减少唤醒次数，增大单次时延) */
    ngx_flag_t    accept_mutex;   /* accept_mutex 指令：启用 accept 互斥锁(默认开)——多 worker 间消除惊群 */

    ngx_msec_t    accept_mutex_delay;   /* accept_mutex_delay 指令：抢锁失败的 worker 延迟多久重试(默认 500ms) */

    u_char       *name;   /* 当前事件模型名("epoll"/"select" 等)，与 use 指令的取值比较 */

#if (NGX_DEBUG)   /* 条件编译：调试版 */
    ngx_array_t   debug_connection;   /* debug_connection 指令：仅对指定客户端 IP 开启 debug 日志 */
#endif   /* 条件编译结束 */
} ngx_event_conf_t;   /* 配置结构类型名 ngx_event_conf_t */


/*
 * ngx_event_module_t —— 事件模块接口：每个 I/O 多路复用实现(epoll 等)
 * 提供一份，含配置钩子与动作函数表，以"插件"方式向框架注册。
 */
typedef struct {   /* 事件模块接口定义 */
    ngx_str_t              *name;   /* 模块名："epoll" 等，供 use 指令匹配 */

    void                 *(*create_conf)(ngx_cycle_t *cycle);   /* create_conf：解析 events{} 前创建本模块的配置块 */
    char                 *(*init_conf)(ngx_cycle_t *cycle, void *conf);   /* init_conf：解析完成后校验配置并补默认值 */

    ngx_event_actions_t     actions;   /* 本模块的动作函数表：初始化时整体拷入全局 ngx_event_actions */
} ngx_event_module_t;   /* 接口类型名 ngx_event_module_t */


extern ngx_atomic_t          *ngx_connection_counter;   /* 全局连接计数器(共享内存原子变量)：统计 worker 集群累计接受的连接数 */

extern ngx_atomic_t          *ngx_accept_mutex_ptr;   /* 指向共享内存中 accept 锁本体：所有 worker 映射同一物理内存 */
extern ngx_atomic_t          *ngx_accept_mutex;   /* 本 worker 视角的锁指针：可对其做原子 CAS 抢锁 */
extern ngx_uint_t             ngx_accept_mutex_held;   /* 本 worker 当前是否持有 accept 锁(持锁者才响应监听读事件) */
extern ngx_msec_t             ngx_accept_mutex_delay;   /* 抢锁失败后的重试延迟毫秒数(来自配置 accept_mutex_delay) */
extern ngx_int_t              ngx_accept_disabled;   /* 大于 0 表示本 worker 连接数接近上限：主动禁抢锁，把 accept 机会让给空闲进程(负载均衡) */


#if (NGX_STAT_STUB)   /* 条件编译：状态统计桩 */

extern ngx_atomic_t  *ngx_stat_accepted;   /* 累计接受连接数(stub_status 的 Accepted) */
extern ngx_atomic_t  *ngx_stat_requests;   /* 累计请求数(Requests) */
extern ngx_atomic_t  *ngx_stat_active;   /* 当前活跃连接数(Active) */
extern ngx_atomic_t  *ngx_stat_reading;   /* 正在读请求的连接数(Reading) */
extern ngx_atomic_t  *ngx_stat_writing;   /* 正在写响应的连接数(Writing) */

#endif   /* 条件编译结束 */



/*
 * 释放 accept 互斥锁的宏(共 4 个续行，逐行对应)：
 *   第 1 行：宏名，无参数；
 *   第 2 行：仅当本 worker 持有锁时才执行释放；
 *   第 3 行：把共享内存中的锁字清零(由锁的使用协议保证安全)；
 *   第 4 行：if 语句闭合，宏定义结束。
 */
#define ngx_accept_mutex_unlock()                                             \
           if (ngx_accept_mutex_held) {                                       \
               *ngx_accept_mutex = 0;                                         \
           }   /* 宏定义结束 */


extern ngx_uint_t             ngx_event_flags;   /* 事件模型特性标志集合(NGX_USE_xxx 与 NGX_HAVE_xxx 按位或)：事件循环据此适配触发语义 */
extern ngx_module_t           ngx_events_module;   /* 顶层 events{} 模块：负责保存各事件模块的配置数组 */
extern ngx_module_t           ngx_event_core_module;   /* 事件核心模块：管理连接池、accept 互斥与事件系统初始化次序 */


/*
 * ngx_event_get_conf 宏(共 2 个续行，逐行对应)：
 *   第 1 行：宏名与参数(conf_ctx 全局配置上下文、module 目标模块)；
 *   第 2 行：先经 ngx_events_module 取出事件配置数组，再按 ctx_index 索引该模块配置块。
 */
#define ngx_event_get_conf(conf_ctx, module)                                  \
             (*(ngx_get_conf(conf_ctx, ngx_events_module))) [module.ctx_index];   /* 取指定事件模块在 events{} 中的配置块 */



void ngx_event_accept(ngx_event_t *ev);   /* accept 事件处理器原型：从内核全连接队列取出连接并创建 ngx_connection_t */
ngx_int_t ngx_trylock_accept_mutex(ngx_cycle_t *cycle);   /* 尝试原子抢占 accept 锁：抢到才注册监听读事件并 accept——多进程惊群的解法 */
ngx_int_t ngx_disable_accept_events(ngx_cycle_t *cycle);   /* 释放锁前禁用全部监听读事件：本 worker 不再参与 accept */
ngx_int_t ngx_enable_accept_events(ngx_cycle_t *cycle);   /* 抢到锁后启用全部监听读事件：本 worker 开始 accept */


#if (WIN32)   /* 条件编译：Windows */
void ngx_event_acceptex(ngx_event_t *ev);   /* AcceptEx 完成回调：建连与首块数据一次返回，省去单独的 accept 调用 */
int ngx_event_post_acceptex(ngx_listening_t *ls, int n);   /* 向完成端口预投递 n 个 AcceptEx 请求：连接到达前提前备好接收 */
#endif   /* 条件编译结束 */


/* used in ngx_log_debugX() */
#define ngx_event_ident(p)  ((ngx_connection_t *) (p))->fd   /* 调试日志标识宏：从事件/连接指针取 fd，作为日志前缀标识 */


#include <ngx_event_timer.h>   /* 定时器头：定时器红黑树与缓存时钟(ngx_current_msec)的超时管理 */
#include <ngx_event_posted.h>   /* post 队列头：就绪事件延迟收集、统一执行的接口 */
#include <ngx_event_busy_lock.h>   /* 忙锁头：多请求互斥访问上游连接的 busy lock 机制 */

#if (WIN32)   /* 条件编译：Windows */
#include <ngx_iocp_module.h>   /* IOCP 事件模块头 */
#endif   /* 条件编译结束 */



/*
 * 按当前事件模型的触发语义维护读事件状态：业务读完数据后调用，决定
 * "重新注册"(ET 一次通知后解除关注)还是"保持/摘除"(LT 需手工管理)，
 * 使上层业务代码与具体 I/O 多路复用机制解耦。
 */
ngx_inline static int ngx_handle_read_event(ngx_event_t *rev, u_int flags)   /* 通用读事件状态维护(内联，ET/LT 双分支) */
{   /* 函数体开始 */
    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {   /* 边缘触发分支：kqueue(EV_CLEAR)与 epoll(EPOLLET) */

        /* kqueue */

        if (!rev->active && !rev->ready) {   /* 未注册且未就绪：上一轮数据已读完，需重新关注读事件 */
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_CLEAR_EVENT)   /* 以边缘触发方式注册读事件(下一行判断返回值) */
                                                                == NGX_ERROR) {   /* 注册失败 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* 内层 if 结束 */
        }   /* "重新注册"分支结束 */

        return NGX_OK;   /* ET 模式注册后常驻关注集，直接成功返回 */

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {   /* 水平触发分支：select/poll/dev-poll */

        /* select, poll, /dev/poll */

        if (!rev->active && !rev->ready) {   /* 未注册且未就绪：加入关注集 */
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT)   /* 以水平触发注册读事件(LT 下数据未读完内核会持续通知) */
                                                                  == NGX_ERROR)   /* 注册失败判定(换行书写) */
            {   /* if 语句体开始 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 注册成功 */
        }   /* "未注册"分支结束 */

        if (rev->active && (rev->ready || (flags & NGX_CLOSE_EVENT))) {   /* 已注册且已就绪(或正关闭 fd)：LT 必须摘除关注，否则下轮空转唤醒 */
            if (ngx_del_event(rev, NGX_READ_EVENT, flags) == NGX_ERROR) {   /* 从关注集删除读事件 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 删除成功 */
        }   /* "摘除"分支结束 */
    }   /* LT 分支结束 */

    /* aio, iocp, epoll, rtsig */

    return NGX_OK;   /* 其余模型(aio/iocp/epoll-ET/rtsig)注册后常驻关注集，无需维护 */
}   /* 函数结束 */


/* 仅水平触发模型的读事件维护简化版：ET 等模型直接跳过，路径更轻量 */
ngx_inline static int ngx_handle_level_read_event(ngx_event_t *rev)   /* LT 专用读事件维护(内联) */
{   /* 函数体开始 */
    if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {   /* 仅当事件模型为水平触发才动作 */
        if (!rev->active && !rev->ready) {   /* 未注册且未就绪：加入关注集 */
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT)   /* 以水平触发注册读事件 */
                                                                  == NGX_ERROR)   /* 注册失败判定(换行书写) */
            {   /* if 语句体开始 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 注册成功 */
        }   /* "未注册"分支结束 */

        if (rev->active && rev->ready) {   /* 已注册且就绪：LT 模型读完后摘除，避免下轮重复唤醒 */
            if (ngx_del_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {   /* 从关注集删除读事件(无附加标志) */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 删除成功 */
        }   /* "摘除"分支结束 */
    }   /* LT 分支结束 */

    return NGX_OK;   /* 非 LT 模型无需处理，直接成功 */
}   /* 函数结束 */


/*
 * 按当前事件模型的触发语义维护写事件状态：发送缓冲写满(EAGAIN)后调用，
 * 决定重新注册写关注(ET)还是摘除/保持(LT)；flags 可携带 NGX_LOWAT_EVENT
 * 令 kqueue 按低水位注册。
 */
ngx_inline static int ngx_handle_write_event(ngx_event_t *wev, u_int flags)   /* 通用写事件状态维护(内联，ET/LT 双分支) */
{   /* 函数体开始 */
    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {   /* 边缘触发分支：kqueue(EV_CLEAR)与 epoll(EPOLLET) */

        /* kqueue */

        if (!wev->active && !wev->ready) {   /* 未注册且未就绪：需重新关注写事件(上轮缓冲满已处理完) */
            if (ngx_add_event(wev, NGX_WRITE_EVENT, NGX_CLEAR_EVENT|flags)   /* 以边缘触发注册写事件，flags 可带低水位标志(下一行判断返回值) */
                                                                  == NGX_ERROR)   /* 注册失败判定(换行书写) */
            {   /* if 语句体开始 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */
        }   /* "重新注册"分支结束 */

        return NGX_OK;   /* ET 模式注册后常驻关注集，直接成功 */

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {   /* 水平触发分支：select/poll/dev-poll */

        /* select, poll, /dev/poll */

        if (!wev->active && !wev->ready) {   /* 未注册且未就绪：加入关注集 */
            if (ngx_add_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)   /* 以水平触发注册写事件 */
                                                                  == NGX_ERROR)   /* 注册失败判定(换行书写) */
            {   /* if 语句体开始 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 注册成功 */
        }   /* "未注册"分支结束 */

        if (wev->active && wev->ready) {   /* 已注册且就绪：本轮可写已处理完，LT 下摘除防止空转 */
            if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {   /* 从关注集删除写事件 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 删除成功 */
        }   /* "摘除"分支结束 */
    }   /* LT 分支结束 */

    /* aio, iocp, epoll, rtsig */

    return NGX_OK;   /* 其余模型无需维护，直接成功 */
}   /* 函数结束 */


/* 仅水平触发模型的写事件维护简化版：行为同上但不含 ET 分支 */
ngx_inline static int ngx_handle_level_write_event(ngx_event_t *wev)   /* LT 专用写事件维护(内联) */
{   /* 函数体开始 */
    if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {   /* 仅当事件模型为水平触发才动作 */
        if (!wev->active && !wev->ready) {   /* 未注册且未就绪：加入关注集 */
            if (ngx_add_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)   /* 以水平触发注册写事件 */
                                                                  == NGX_ERROR)   /* 注册失败判定(换行书写) */
            {   /* if 语句体开始 */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 注册成功 */
        }   /* "未注册"分支结束 */

        if (wev->active && wev->ready) {   /* 已注册且就绪：处理完后摘除防止空转 */
            if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {   /* 从关注集删除写事件(无附加标志) */
                return NGX_ERROR;   /* 返回错误 */
            }   /* if 结束 */

            return NGX_OK;   /* 删除成功 */
        }   /* "摘除"分支结束 */
    }   /* LT 分支结束 */

    return NGX_OK;   /* 非 LT 模型直接成功 */
}   /* 函数结束 */


#endif /* _NGX_EVENT_H_INCLUDED_ */   /* 头文件保护宏结束：_NGX_EVENT_H_INCLUDED_ */
