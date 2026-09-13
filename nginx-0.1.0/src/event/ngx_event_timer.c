
/*
 * Copyright (C) Igor Sysoev
 */
/*
 * 文件概述：定时器事件管理。
 * nginx 不用内核的 setitimer/timerfd，而是在用户态维护一棵以"到期时刻"为键的
 * 红黑树(ngx_event_timer_rbtree)统一组织所有定时事件：
 *   - ngx_event_find_timer() 取最小键(最近到期事件)的剩余毫秒数，作为
 *     epoll_wait/select 的阻塞超时上限——把"固定间隔轮询"变成"一次带超时的
 *     阻塞"，无定时器时线程可无限期睡眠，不空耗 CPU；
 *   - ngx_event_expire_timers() 把已到期事件摘树并回调 handler，是 TCP 层
 *     一切超时机制(发送超时/keepalive 空闲超时/请求超时)的底层执行者；
 *   - 时间基准用事件循环每轮缓存一次的 ngx_elapsed_msec，避免每个定时器
 *     操作都陷入内核 gettimeofday。
 * 线程模式下对共享红黑树的访问由 ngx_event_timer_mutex 保护。
 */


#include <ngx_config.h>  /* 平台配置头：编译期特性探测与基础类型 */
#include <ngx_core.h>  /* 核心头：ngx_int_t/红黑树/内存池等基础设施 */
#include <ngx_event.h>  /* 事件模块头：ngx_event_t 与定时器接口声明 */


#if (NGX_THREADS)  /* 多线程构建选项：该互斥量仅线程模式存在 */
ngx_mutex_t  *ngx_event_timer_mutex;  /* 保护定时器红黑树的互斥量：worker 内多线程共享同一棵树 */
#endif  /* NGX_THREADS 结束 */


/* 全局定时器红黑树根指针：全部定时事件以到期时刻为键挂入；volatile 提示编译器其他线程会并发读写，禁止缓存在寄存器 */
ngx_thread_volatile ngx_rbtree_t  *ngx_event_timer_rbtree;
/* NIL 哨兵节点：统一代替红黑树的空指针叶子，令"根指针==哨兵"即空树，免去每次下行的 NULL 判断 */
ngx_rbtree_t                       ngx_event_timer_sentinel;


/*
 * ngx_event_timer_init()：初始化定时器子系统。
 * 红黑树用"惰性初始化"：全局根指针首次为 NULL，此后恒指向哨兵，
 * 以"根指针==哨兵"表达空树，无需额外的初始化标志。
 */
ngx_int_t ngx_event_timer_init(ngx_log_t *log)  /* 建立或复用定时器红黑树与互斥量 */
{  /* 函数体开始 */
    if (ngx_event_timer_rbtree) {  /* 根指针非空说明已初始化过 */
#if (NGX_THREADS)  /* 仅线程模式需要更新互斥量 */
        ngx_event_timer_mutex->log = log;  /* 互斥量换绑新 cycle 的日志对象，便于输出锁相关诊断 */
#endif  /* NGX_THREADS 结束 */
        return NGX_OK;  /* 复用既有红黑树，直接成功返回 */
    }  /* 结束"已初始化"分支 */

    /* 首次初始化：空树用"根指针指向哨兵"表示，后续插入会覆盖根指针 */
    ngx_event_timer_rbtree = &ngx_event_timer_sentinel;

#if (NGX_THREADS)  /* 仅线程模式需要互斥量 */
    if (!(ngx_event_timer_mutex = ngx_mutex_init(log, 0))) {  /* 创建非递归互斥量；失败则定时器子系统不可用 */
        return NGX_ERROR;  /* 错误上抛，事件模块初始化将中止 */
    }  /* 结束互斥量创建失败判断 */
#endif  /* NGX_THREADS 结束 */

    return NGX_OK;  /* 初始化成功 */
}  /* ngx_event_timer_init 结束 */


/*
 * ngx_event_find_timer()：求距最近一个定时事件到期的剩余毫秒数。
 * 返回值直接作为 epoll_wait/select 的 timeout：到期前有 IO 事件仍会立即
 * 唤醒，因此既不丢定时精度又消灭了忙等待；树空时返回"无限"让线程长眠。
 */
ngx_msec_t ngx_event_find_timer(void)  /* 查询最小剩余超时，供事件循环设置阻塞上限 */
{  /* 函数体开始 */
    ngx_msec_t     timer;  /* 最近到期事件的剩余毫秒数 */
    ngx_rbtree_t  *node;  /* 树中最小键节点：键最小者最先到期 */

    if (ngx_event_timer_rbtree == &ngx_event_timer_sentinel) {  /* 根指针等于哨兵即空树 */
        return NGX_TIMER_INFINITE;  /* 无任何定时器：返回"无限等待"，让 IO 多路复用长阻塞 */
    }  /* 结束空树分支 */

    if (ngx_mutex_lock(ngx_event_timer_mutex) == NGX_ERROR) {  /* 线程模式下读取共享树前先加锁 */
        return NGX_TIMER_ERROR;  /* 加锁失败：返回错误码，调用方按异常处理 */
    }  /* 结束加锁失败分支 */

    /* 沿左孩子链下行取最小键节点——BST/红黑树的 minimum 操作，O(log n) */
    node = ngx_rbtree_min((ngx_rbtree_t *) ngx_event_timer_rbtree,
                          &ngx_event_timer_sentinel);  /* 哨兵标志空叶 */

    ngx_mutex_unlock(ngx_event_timer_mutex);  /* 只为读一个最小键进临界区，取完立即放锁，缩小争用窗口 */

    timer = (ngx_msec_t)  /* 剩余超时 = 事件到期时刻 − 对齐后的当前流逝时间 */
         (node->key * NGX_TIMER_RESOLUTION -  /* 键乘以时间分辨率还原成毫秒级到期时刻 */
               /* 当前流逝毫秒也按同一粒度对齐，抵消 tick 之间的相位差 */
               ngx_elapsed_msec / NGX_TIMER_RESOLUTION * NGX_TIMER_RESOLUTION);
#if 0  /* 被禁用的旧实现：直接做毫秒差，未按时间粒度对齐 */
                         /* 未按粒度对齐的旧公式(已停用) */
                         (node->key * NGX_TIMER_RESOLUTION - ngx_elapsed_msec);
#endif  /* #if 0 结束 */

    return timer > 0 ? timer: 0 ;  /* 剩余≤0(已到期)则钳为 0：让 IO 等待立即返回，马上执行到期回调 */
}  /* ngx_event_find_timer 结束 */


/*
 * ngx_event_expire_timers()：批量处理已到期事件。
 * 每轮以树中最小键为线索：到期则摘树并回调，直到最小键"仍在未来"为止，
 * 一轮代价 O(k·log n)(k 为到期事件数)——无需扫描整棵树。
 */
void ngx_event_expire_timers(ngx_msec_t timer)  /* 触发到期事件；timer 为自上轮以来的流逝毫秒数 */
{  /* 函数体开始 */
    ngx_event_t   *ev;  /* 由树节点反推出的定时事件对象 */
    ngx_rbtree_t  *node;  /* 当前树中的最小键节点 */

    if (timer < 0) {  /* 时间倒退防御：系统时钟被拨回时流逝增量可能为负 */
        /* avoid the endless loop if the time goes backward for some reason */
        timer = 0;  /* 钳为 0：全部按"已到期"处理，避免负值让循环条件永真而死循环 */
    }  /* 结束负值修正 */

    for ( ;; ) {  /* 逐个摘取到期事件，直到最小键落在未来 */
        /* 根指针等于哨兵：树已空，所有到期定时器处理完毕 */
        if (ngx_event_timer_rbtree == &ngx_event_timer_sentinel) {
            return;  /* 返回事件循环主流程 */
        }  /* 结束空树判断 */

        if (ngx_mutex_lock(ngx_event_timer_mutex) == NGX_ERROR) {  /* 摘树与增删定时器的线程互斥 */
            return;  /* 加锁失败只能放弃本轮到期处理 */
        }  /* 结束加锁失败判断 */

        /* 每轮重新取最小键：上一轮刚把前一个最小节点摘走 */
        node = ngx_rbtree_min((ngx_rbtree_t *) ngx_event_timer_rbtree,
                              &ngx_event_timer_sentinel);  /* 哨兵标志空叶 */

        if (node->key <= (ngx_msec_t)  /* 到期判定：最小键 ≤ 当前时刻(旧基准+本轮流逝)按粒度对齐 */
                         /* 分子即当前时刻毫秒数(上一基准+流逝增量) */
                         (ngx_old_elapsed_msec + timer) / NGX_TIMER_RESOLUTION)
        {  /* 进入"事件已到期"分支 */
            /* 由内嵌树节点地址反推宿主事件地址——container_of 手法，零开销完成节点→事件映射 */
            ev = (ngx_event_t *)
                           /* offsetof 给出内嵌节点在事件内的字节偏移，相减即得首地址 */
                           ((char *) node - offsetof(ngx_event_t, rbtree_key));

#if (NGX_THREADS)  /* 线程模式：到期事件可能正被其他线程处理 */

            /* trylock 失败说明事件正被其他线程(读/写线程)处理；
             * 不能动它的定时器也不能继续摘树(可能与对方冲突)，只能整体退出 */
            if (ngx_threaded && ngx_trylock(ev->lock) == 0) {

                /*
                 * We can not change the timer of the event that is been
                 * handling by another thread.  And we can not easy walk
                 * the rbtree to find a next expired timer so we exit the loop.
                 * However it should be rare case when the event that is
                 * been handling has expired timer.
                 */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,  /* 调试日志：仅 NGX_DEBUG 编译时生效 */
                               /* 格式串：打印事件指针 */
                               "event " PTR_FMT " is busy in expire timers",
                               ev);  /* 实参：事件地址 */
                break;  /* 跳出循环：无法安全摘树，剩余到期事件留给下一轮 */
            }  /* 结束"事件忙"分支 */
#endif  /* NGX_THREADS 结束 */

            /* 调试输出：报告即将从定时器树摘除的事件(宏在非 DEBUG 编译下为空操作) */
            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                           "event timer del: %d: %d",  /* 格式串：连接 ident 与到期键 */
                            ngx_event_ident(ev->data), ev->rbtree_key);  /* 实参：fd 与键值 */

            /* 从定时器红黑树摘除该事件：O(log n)，含删除后的变色/旋转修复 */
            ngx_rbtree_delete((ngx_rbtree_t **) &ngx_event_timer_rbtree,
                              &ngx_event_timer_sentinel,  /* 哨兵：空叶标志 */
                              (ngx_rbtree_t *) &ev->rbtree_key);  /* 待删节点：事件内嵌的树节点 */

            /* 摘树完成即放锁：回调可能很重(读写 socket/关连接)，绝不能持锁执行 */
            ngx_mutex_unlock(ngx_event_timer_mutex);

#if (NGX_DEBUG)  /* 调试编译：清空节点指针便于暴露 use-after-free */
            ev->rbtree_left = NULL;  /* 断开左孩子引用 */
            ev->rbtree_right = NULL;  /* 断开右孩子引用 */
            ev->rbtree_parent = NULL;  /* 断开父节点引用 */
#endif  /* NGX_DEBUG 结束 */

            ev->timer_set = 0;  /* 清定时器标志：事件已不在定时器树中 */

#if (NGX_THREADS)  /* 线程模式：超时回调改投递到 posted 队列延后执行 */
            if (ngx_threaded) {  /* 仅在多线程下走投递路径 */
                if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {  /* posted 队列同样是共享资源，须加锁 */
                    return;  /* 加锁失败：返回(注意此时已不持定时器锁) */
                }  /* 结束加锁失败判断 */

                ev->posted_timedout = 1;  /* 先记入"挂起态超时"，真实 timedout 延迟到实际处理时再合并 */
                ngx_post_event(ev);  /* 头插挂入 posted 队列：把超时交给所属线程统一处理，避免跨线程直接回调 */

                ngx_mutex_unlock(ngx_posted_events_mutex);  /* 入队完成即放锁 */

                ngx_unlock(ev->lock);  /* 释放事件自旋锁，允许其他线程安全访问该事件 */

                continue;  /* 继续处理下一个到期事件 */
            }  /* 结束线程模式分支 */
#endif  /* NGX_THREADS 结束 */

            ev->timedout = 1;  /* 单线程模式：直接置超时标志，回调据此关闭连接/重传等 */

            ev->event_handler(ev);  /* 执行超时回调：如 keepalive 空闲超时关闭、上游连接超时等 */

            continue;  /* 回调中可能又注册新定时器、改变树结构，回到循环头重取最小键 */
        }  /* 结束"已到期"分支 */

        break;  /* 最小键尚在未来：其余键更大必然未到期，无需继续遍历 */
    }  /* for(;;) 结束 */

    ngx_mutex_unlock(ngx_event_timer_mutex);  /* 释放定时器树互斥量后返回 */
}  /* ngx_event_expire_timers 结束 */
