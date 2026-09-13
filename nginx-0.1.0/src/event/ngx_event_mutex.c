
/*
 * Copyright (C) Igor Sysoev
 */
/*
 * 文件概述：基于事件机制的"事件互斥锁"(event mutex)。
 * 与 pthread_mutex 不同：拿不到锁时不睡眠，而是把等待者串成事件链表并
 * 为其注册定时器，锁释放时把队头移交 posted 队列完成用户态"唤醒"——
 * 把锁等待也纳入统一的事件循环。典型用途是多 worker 进程争抢 accept
 * 互斥量：同一时刻只有一个进程阻塞在监听 socket 上，化解 accept 惊群。
 */


#include <ngx_config.h>  /* 平台配置头：编译期特性探测与基础类型 */
#include <ngx_core.h>  /* 核心头：日志/定时器/基础类型 */
#include <ngx_event.h>  /* 事件模块头：ngx_event_t 与事件互斥锁接口 */


/*
 * ngx_event_mutex_timedlock()：对事件互斥锁做带超时的加锁。
 * 返回 NGX_OK 表示立即得锁；返回 NGX_AGAIN 表示锁被占用、事件 ev 已排入
 * 等待队列并注册了超时——超时前锁被释放则事件经 posted 队列"唤醒"领锁。
 */
ngx_int_t ngx_event_mutex_timedlock(ngx_event_mutex_t *m, ngx_msec_t timer,  /* 锁对象与等待超时毫秒数 */
                                    ngx_event_t *ev)  /* 得不到锁时用于排队与超时的事件 */
{  /* 函数体开始 */
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,  /* 调试日志：非 DEBUG 编译为空操作 */
                   "lock event mutex " PTR_FMT " lock:%X", m, m->lock);  /* 打印锁地址与当前锁值 */

    if (m->lock) {  /* 锁已被占用(非 0)：走排队等待路径 */

        if (m->events == NULL) {  /* 等待队列还是空的：本事件成为队头 */
            m->events = ev;  /* 设置队头指针 */

        } else {  /* 队列非空：接到队尾 */
            m->last->next = ev;  /* 尾插保持 FIFO：先等先得，避免饥饿 */
        }  /* 结束队头/队尾判断 */

        m->last = ev;  /* 更新队尾指针，供下一个等待者链接 */
        ev->next = NULL;  /* 新队尾的 next 置空，标志链表结束 */

#if (NGX_THREADS0)  /* 注意：NGX_THREADS0 恒为假——此段 light 标记实际被禁用 */
        ev->light = 1;  /* 轻量锁标记(预留字段，当前未启用) */
#endif  /* NGX_THREADS0 结束 */

        ngx_add_timer(ev, timer);  /* 为等待注册定时器：超时仍未得锁则由 expire_timers 放弃本次加锁 */

        return NGX_AGAIN;  /* 返回"稍后再试"：锁未得，但事件已在等待队列中 */
    }  /* 结束锁被占用分支 */

    m->lock = 1;  /* 锁空闲：直接占有。单个字标志位赋值天然原子，无需 CAS */

    return NGX_OK;  /* 成功获得锁 */
}  /* ngx_event_mutex_timedlock 结束 */


/*
 * ngx_event_mutex_unlock()：释放事件互斥锁。
 * 若有等待者，直接把队头事件移交 posted 队列——它将在下一轮事件处理中被
 * 当作"已获得锁"来执行，等价于一次用户态唤醒(思想类似内核 futex 唤醒)。
 */
ngx_int_t ngx_event_mutex_unlock(ngx_event_mutex_t *m, ngx_log_t *log)  /* 释放锁并把使用权移交队头等待者 */
{  /* 函数体开始 */
    ngx_event_t  *ev;  /* 即将被唤醒的队头等待事件 */

    if (m->lock == 0) {  /* 对空闲锁执行 unlock：调用序错误，属严重异常 */
        ngx_log_error(NGX_LOG_ALERT, log, 0,  /* ALERT 级日志：提示锁状态已被破坏 */
                      "tring to unlock the free event mutex " PTR_FMT, m);  /* 打印出错锁地址(原文拼写遗留) */
        return NGX_ERROR;  /* 返回错误，交调用方处置 */
    }  /* 结束异常判断 */

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, log, 0,  /* 调试日志：非 DEBUG 编译为空 */
                   "unlock event mutex " PTR_FMT ", next event: " PTR_FMT,  /* 格式串 */
                   m, m->events);  /* 实参：锁与下一个等待事件 */

    m->lock = 0;  /* 先清锁标志：释放临界区，争用者立即可见 */

    if (m->events) {  /* 存在等待者：把锁使用权直接移交给队头 */
        ev = m->events;  /* 取队头事件(FIFO 最早等待者，保证公平) */
        m->events = ev->next;  /* 队头后移一位；队列清空则变为 NULL */

        ev->next = (ngx_event_t *) ngx_posted_events;  /* 头插法挂入 posted 队列 */
        ngx_posted_events = ev;  /* 新队头指向 ev：下一轮 process_posted 即"唤醒"该等待者 */
    }  /* 结束等待者移交分支 */

    return NGX_OK;  /* 释放成功 */
}  /* ngx_event_mutex_unlock 结束 */
