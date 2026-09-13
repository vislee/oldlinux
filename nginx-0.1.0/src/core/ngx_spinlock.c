
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置头：决定 NGX_HAVE_ATOMIC_OPS 等特性宏 */
#include <ngx_core.h>  /* 核心头：ngx_atomic_t 原子类型与 sched_yield 封装 */


/*
 * ngx_spinlock()：获取自旋锁(spinlock)。
 * 基于 CPU 原子指令(x86 上为 lock cmpxchg)的用户态锁：拿不到锁不睡眠、
 * 不陷入内核，而是忙等；先自旋 spin 次(仅多 CPU 有意义，单核忙等只会
 * 阻止持锁者被调度)，仍失败则 ngx_sched_yield() 让出 CPU。适合临界区
 * 极短的场景(如多 worker 抢 accept 锁)——省去互斥量睡眠/唤醒的两次
 * 上下文切换，代价是忙等本身空耗 CPU 周期。
 */
void ngx_spinlock(ngx_atomic_t *lock, ngx_uint_t spin)  /* 自旋获取锁；spin 为忙等次数上限 */
{  /* 函数体开始 */

#if (NGX_HAVE_ATOMIC_OPS)  /* 平台提供原子 CAS 指令时才可用自旋实现 */

    ngx_uint_t  tries;  /* 本轮已自旋的次数计数 */

    tries = 0;  /* 让步后计数清零，重新允许 spin 次忙等 */

    for ( ;; ) {  /* 无限循环：直到拿到锁；没有超时逻辑，调用方负责时限 */

        if (*lock) {  /* 锁被占用(非 0)：先原地试探，必要时让步 */
            if (ngx_ncpu > 1 && tries++ < spin) {  /* 多 CPU 且未达上限：锁可能马上释放，短忙等比切换线程快 */
                continue;  /* 立即重读锁变量(缓存一致性协议保证看到最新值) */
            }  /* 结束自旋判断 */

            ngx_sched_yield();  /* 让出当前 CPU 时间片：单核下必须如此，否则持锁者永远得不到调度(活锁) */

            tries = 0;  /* 让步一次后重置计数，允许下一轮继续 spin 次自旋 */

        } else {  /* 锁显示空闲：用原子 CAS(0→1) 尝试占有 */
            if (ngx_atomic_cmp_set(lock, 0, 1)) {  /* lock cmpxchg：仅当仍为 0 时置 1；与竞争者比拼原子性 */
                return;  /* 成功持锁返回：临界区应极短，用完立即 ngx_unlock */
            }  /* CAS 失败：锁在检查与比较之间被抢走，回到循环重试 */
        }  /* 结束忙/闲两大分支 */
    }  /* for(;;) 结束 */

#else  /* 无原子指令的退化路径 */

#if (NGX_THREADS)  /* 多线程却又没有原子操作，此锁在原理上无法实现 */

#error ngx_spinlock() or ngx_atomic_cmp_set() are not defined !  /* 编译期报错：逼出配置矛盾，而非留到运行时出竞态 */

#endif  /* NGX_THREADS 结束 */

#endif  /* NGX_HAVE_ATOMIC_OPS 结束 */

}  /* ngx_spinlock 结束 */
