
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>   /* 编译期配置头：HAVE_LOCALTIME_R 等平台探测宏在此定义 */
#include <ngx_core.h>   /* 核心头：ngx_tm_t(struct tm 封装)与缓存时间接口 ngx_time() 的声明 */


/*
 * ngx_localtime：把"缓存的管理进程当前时间"转换为本地时间(struct tm)。
 * 热路径优化理论：时间字符串(日志行、HTTP Date 头)每秒才变一次，
 * nginx 的缓存时间管理器每秒集中刷新一次缓存值，各处调用 ngx_time()
 * 只读内存变量而不必反复陷入 gettimeofday 系统调用（每次系统调用有
 * 上下文切换/陷入内核的固定开销，日志这类高频路径承受不起）；
 * 本函数同样只基于缓存秒数做转换，不再取系统时钟。
 */
void ngx_localtime(ngx_tm_t *tm)   /* tm：调用者提供的结果结构（出参） */
{
#if (HAVE_LOCALTIME_R)   /* 平台提供可重入版本 localtime_r 时优先使用 */

    time_t     now;   /* 缓存的当前秒数（自 Epoch 起的 UTC 秒） */

    now = ngx_time();   /* 读缓存时间而非调用 gettimeofday：避免热路径上的系统调用开销 */
    localtime_r(&now, tm);   /* 可重入版本：结果写入调用者的 tm，不依赖任何静态缓冲，多线程/多模块并发调用安全 */

#else   /* 无 localtime_r 的老平台回退到非重入版本 */

    time_t     now;   /* 缓存的当前秒数 */
    ngx_tm_t  *t;   /* 指向 localtime 返回的静态缓冲——全进程共享一份，绝非线程安全 */

    now = ngx_time();   /* 同样只读缓存时间 */
    t = localtime(&now);   /* 不可重入版本：结果落在库内部 static 缓冲，须立即拷走防止被后续调用覆盖 */
    *tm = *t;   /* 整体拷贝到调用者的结构体，把结果从静态区"抢救"出来 */

#endif   /* 平台分支结束 */

    tm->ngx_tm_mon++;   /* struct tm 约定月份从 0 计（0=1 月），加 1 转成人类习惯的 1~12 */
    tm->ngx_tm_year += 1900;   /* struct tm 约定年份是"自 1900 年起的偏移"，加 1900 得完整公历年份，供日志/Date 头直接格式化 */
}   /* ngx_localtime 结束 */
