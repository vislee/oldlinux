
/*
 * Copyright (C) Igor Sysoev
 */
/*
 * 文件概述：posted(挂起)事件队列。
 * 事件被 IO 多路复用判定就绪后并不立即执行 handler，而是先挂入以
 * ngx_posted_events 为头的单向链表，等事件循环走到统一位置再逐个处理——
 * "先收集、后处理"的两阶段设计。价值在于 use-after-free 防御：处理事件 A
 * 时可能关闭连接 B，若 B 的就绪事件在队外被直接回调就会引用已释放内存；
 * 延迟执行使"连接是否仍然存活"的判定集中到实际处理时刻。
 */


#include <ngx_config.h>  /* 平台配置头：编译期特性探测与基础类型 */
#include <ngx_core.h>  /* 核心头：日志/锁/基础类型 */
#include <ngx_event.h>  /* 事件模块头：ngx_event_t 与 posted 事件接口 */


/* posted 事件单向链表头：就绪事件先挂入此队列延后统一处理；volatile 防止编译器把共享链表头缓存进寄存器 */
ngx_thread_volatile ngx_event_t  *ngx_posted_events;

#if (NGX_THREADS)  /* 多线程构建选项：该互斥量仅线程模式存在 */
/* 保护 posted 队列的互斥量：挂队列/出队/遍历都必须持锁进行 */
ngx_mutex_t                      *ngx_posted_events_mutex;
#endif  /* NGX_THREADS 结束 */


/*
 * ngx_event_process_posted()：单线程模式的 posted 事件处理。
 * 不断取链表头、先出队再回调：出队在前，保证即使 handler 内又触发挂队或
 * 关连接，也不会与本事件仍滞留队列形成悬空引用或自引用死循环。
 */
void ngx_event_process_posted(ngx_cycle_t *cycle)  /* 统一处理本轮累积的全部 posted 事件 */
{  /* 函数体开始 */
    ngx_event_t  *ev;  /* 当前待处理的事件 */

    for ( ;; ) {  /* 循环取队头直到队列空：处理中可能又有新事件入队，故用无限循环 */

        ev = (ngx_event_t *) ngx_posted_events;  /* 读共享链表头快照：此后他人入队只影响新队头 */

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,  /* 调试日志：非 DEBUG 编译为空操作 */
                      "posted event " PTR_FMT, ev);  /* 格式串与事件指针 */

        if (ev == NULL) {  /* 队列为空：本轮 posted 事件全部处理完 */
            return;  /* 返回事件循环主流程 */
        }  /* 结束空队列判断 */

        ngx_delete_posted_event(ev);  /* 先摘出队列再执行：handler 重入或关连接时不会碰到已失效的队列项 */

        ev->event_handler(ev);  /* 真正执行回调：此刻再做"连接是否已关"等状态判定是安全的 */
    }  /* for(;;) 结束 */
}  /* ngx_event_process_posted 结束 */


#if (NGX_THREADS)  /* 以下为线程模式：队列被多线程共享，消费逻辑更复杂 */

/*
 * ngx_wakeup_worker_thread()：唤醒空闲线程去消费 posted 队列。
 * 只对一个空闲线程发条件变量信号即可——多唤醒只会造成对同一事件的
 * 无效竞争(用户态的"惊群"效应)。
 */
void ngx_wakeup_worker_thread(ngx_cycle_t *cycle)  /* 唤醒一个空闲线程处理 posted 事件 */
{  /* 函数体开始 */
    ngx_int_t     i;  /* 线程池数组下标 */
    ngx_uint_t    busy;  /* 标志：事件是否全被锁定(在 #if 0 的旧方案中使用) */
    ngx_event_t  *ev;  /* 遍历 posted 队列的游标(旧方案用) */

#if 0  /* 被禁用的优化方案：先探测是否存在可认领事件，全忙则不唤醒 */
    busy = 1;  /* 乐观假设全部线程忙 */

    /* 加锁遍历共享队列；加锁失败则直接放弃唤醒 */
    if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {
        return;  /* 出错返回，不做唤醒 */
    }  /* 结束加锁失败判断 */

    /* 顺序扫描队列：只要存在未被锁定的事件，就说明有活可干 */
    for (ev = (ngx_event_t *) ngx_posted_events; ev; ev = ev->next) {
        if (*(ev->lock) == 0) {  /* 事件锁为 0：没有线程正在处理它 */
            busy = 0;  /* 存在可认领事件，唤醒不会空转 */
            break;  /* 结论已得，提前结束扫描 */
        }  /* 结束锁定判断 */
    }  /* 队列扫描结束 */

    ngx_mutex_unlock(ngx_posted_events_mutex);  /* 遍历完毕释放队列锁 */

    if (busy) {  /* 所有事件都被锁定：线程醒了也无活可干 */
        return;  /* 省去一次无谓的线程唤醒与上下文切换 */
    }  /* 结束全忙判断 */
#endif  /* #if 0 结束 */

    for (i = 0; i < ngx_threads_n; i++) {  /* 线性扫描线程池数组找空闲线程 */
        if (ngx_threads[i].state == NGX_THREAD_FREE) {  /* 命中空闲线程 */
            ngx_cond_signal(ngx_threads[i].cv);  /* 条件变量信号：唤醒其从等待中返回去领事件 */
            return;  /* 只唤醒一个：避免多线程抢同一批事件的锁 */
        }  /* 结束空闲判断 */
    }  /* 未找到空闲线程则静默返回 */
}  /* ngx_wakeup_worker_thread 结束 */


/*
 * ngx_event_thread_process_posted()：线程模式的 posted 事件处理。
 * 每个事件自带一把自旋锁(ev->lock)：线程 trylock 成功才可处理，失败则跳到
 * 下一个事件——多线程从同一条队列"认领"工作；锁粒度是事件而非队列，
 * 最大化并行度。回调执行前放开队列互斥量，防止长回调阻塞整个线程池。
 */
ngx_int_t ngx_event_thread_process_posted(ngx_cycle_t *cycle)  /* 线程安全地消费 posted 队列 */
{  /* 函数体开始 */
    ngx_event_t  *ev;  /* 当前考察的事件 */

    for ( ;; ) {  /* 外层循环：不断从队头重新开始，直到队列清空 */

        ev = (ngx_event_t *) ngx_posted_events;  /* 取当前队头快照 */

        for ( ;; ) {  /* 内层循环：从头向尾寻找一个未被锁定的事件 */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,  /* 调试日志：非 DEBUG 编译为空 */
                          "posted event " PTR_FMT, ev);  /* 打印当前考察的事件指针 */

            if (ev == NULL) {  /* 走到链表尾：队列中已无可处理事件 */
                return NGX_OK;  /* 正常返回，posted 队列清空 */
            }  /* 结束尾判断 */

            if (ngx_trylock(ev->lock) == 0) {  /* 原子 trylock 失败：事件已被其他线程认领 */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,  /* 调试日志：报告事件忙 */
                               "posted event " PTR_FMT " is busy", ev);  /* 格式串与实参 */

                ev = ev->next;  /* 跳过忙事件，考察下一个 */
                continue;  /* 回到内层循环头 */
            }  /* 结束"事件忙"分支 */

            if (ev->lock != ev->own_lock) {  /* 事件锁与"专属锁"不同：还需 own_lock 串行化关联资源 */
                if (*(ev->own_lock)) {  /* own_lock 已被占用：本线程内部已有同类操作在进行 */
                    /* 记录告警：own_lock 冲突意味着潜在的自死锁风险 */
                    ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                                  /* 格式串(拆成两段字面量拼接) */
                                  "the own lock of the posted event "
                                  PTR_FMT " is busy", ev);  /* 实参：事件地址 */
                    ngx_unlock(ev->lock);  /* 归还刚 trylock 到的事件锁 */
                    ev = ev->next;  /* 放弃该事件，继续找下一个 */
                    continue;  /* 回到内层循环头 */
                }  /* 结束 own_lock 忙判断 */
                *(ev->own_lock) = 1;  /* 占住专属锁：保证本线程内对关联资源的独占 */
            }  /* 结束 own_lock 判断 */

            ngx_delete_posted_event(ev);  /* 出队：锁已在手，先摘链再处理，防重入时引用失效节点 */

            ev->locked = 1;  /* 标记"本线程处理中"，回调完成后再统一解锁 */

            ev->ready |= ev->posted_ready;  /* 把入队时暂存的就绪标志合并进真实事件状态 */
            ev->timedout |= ev->posted_timedout;  /* 合并挂起期间发生的超时标志 */
            ev->pending_eof |= ev->posted_eof;  /* 合并对端关闭(EOF)标志：TCP FIN 导致的状态迁移 */
#if (HAVE_KQUEUE)  /* kqueue 特有错误通道 */
            ev->kq_errno |= ev->posted_errno;  /* 合并 kqueue 暂存的 errno */
#endif  /* HAVE_KQUEUE 结束 */
            if (ev->posted_available) {  /* 挂起期间记录过可读写字节数 */
                ev->available = ev->posted_available;  /* 合并之：供读写 handler 判断本次可处理的数据量 */
            }  /* 结束可用字节合并 */

            ev->posted_ready = 0;  /* 以下逐项清零 posted_* 暂存字段，防止旧状态串入下一轮 */
            ev->posted_timedout = 0;  /* 清暂存超时标志 */
            ev->posted_eof = 0;  /* 清暂存 EOF 标志 */
#if (HAVE_KQUEUE)  /* kqueue 特有错误通道 */
            ev->posted_errno = 0;  /* 清暂存 kqueue 错误码 */
#endif  /* HAVE_KQUEUE 结束 */
            ev->posted_available = 0;  /* 清暂存可用字节数 */

            /* 关键设计：执行回调前放开队列互斥量——回调耗时不可控，
             * 若持锁执行，其他线程将全部阻塞在取事件上，并发能力归零 */
            ngx_mutex_unlock(ngx_posted_events_mutex);

            ev->event_handler(ev);  /* 回调期间只持事件锁与专属锁，队列对其他线程保持开放 */

            if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {  /* 回调完成重新加锁，才能安全收尾并继续遍历 */
                return NGX_ERROR;  /* 加锁失败：上报错误，交由事件循环处置 */
            }  /* 结束加锁失败判断 */

            if (ev->locked) {  /* handler 未自行接管锁定状态：由本函数负责解锁收尾 */
                ngx_unlock(ev->lock);  /* 释放事件锁：该事件此后可被其他线程重新处理(若再次入队) */

                if (ev->lock != ev->own_lock) {  /* 若动用了独立的专属锁也要释放 */
                    ngx_unlock(ev->own_lock);  /* 释放专属锁，恢复资源可竞争状态 */
                }  /* 结束专属锁判断 */
            }  /* 结束解锁收尾 */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,  /* 调试日志：非 DEBUG 编译为空 */
                           "posted event " PTR_FMT " is done", ev);  /* 报告事件处理完毕 */

            break;  /* 本事件完成：回到外层循环重新取队头 */
        }  /* 内层 for(;;) 结束 */
    }  /* 外层 for(;;) 结束 */
}  /* ngx_event_thread_process_posted 结束 */

#else  /* 非线程模式：唤醒函数退化为空操作 */

void ngx_wakeup_worker_thread(ngx_cycle_t *cycle)  /* 单线程构建没有 worker 线程，保留空函数维持接口一致 */
{  /* 函数体开始 */
}  /* 空函数体 */

#endif  /* NGX_THREADS 结束 */
