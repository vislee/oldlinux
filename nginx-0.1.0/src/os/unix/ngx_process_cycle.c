
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 引入平台相关配置宏：根据编译探测结果决定特性开关与类型定义 */
#include <ngx_core.h>  /* 引入核心头：cycle/内存池/进程表(ngx_processes)等核心结构与 API */
#include <ngx_event.h>  /* 引入事件框架头：ngx_process_events、定时器红黑树、监听 socket 管理等 */
#include <ngx_channel.h>  /* 引入通道头：master/worker 间 socketpair 消息格式与读写接口 */


/* 本文件静态函数前向声明（static 限定文件内可见，避免污染全局符号表） */
static void ngx_start_worker_processes(ngx_cycle_t *cycle, ngx_int_t n,  /* 声明：按配置数量 fork 出 n 个 worker 进程 */
                                       ngx_int_t type);  /* 声明续行：type 指定创建类型（是否崩溃自动重生等） */
static void ngx_signal_worker_processes(ngx_cycle_t *cycle, int signo);  /* 声明：向所有受管 worker 投递指定信号/命令 */
static ngx_uint_t ngx_reap_childs(ngx_cycle_t *cycle);  /* 声明：收割退出子进程并按 respawn 策略重生，返回是否仍有存活者 */
static void ngx_master_exit(ngx_cycle_t *cycle, ngx_master_ctx_t *ctx);  /* 声明：master 清理 pid 文件与内存池后退出 */
static void ngx_worker_process_cycle(ngx_cycle_t *cycle, void *data);  /* 声明：worker 进程的主循环入口（spawn 时作为入口传入） */
static void ngx_channel_handler(ngx_event_t *ev);  /* 声明：通道可读事件回调，解析 master 发来的控制命令 */
#if (NGX_THREADS)  /* 条件编译：仅当开启线程支持时声明以下线程相关函数 */
static void ngx_wakeup_worker_threads(ngx_cycle_t *cycle);  /* 声明：唤醒并 join 回收全部工作线程 */
static void *ngx_worker_thread_cycle(void *data);  /* 声明：worker 内工作线程的主循环（处理 posted 事件） */
#endif  /* 结束 NGX_THREADS 条件编译 */


ngx_uint_t    ngx_process;  /* 当前进程角色标识（master/worker），各模块据此切换行为 */
ngx_pid_t     ngx_pid;      /* 当前进程 PID：fork 时由内核分配，写日志与 pid 文件使用 */
ngx_uint_t    ngx_threaded; /* 是否线程模式：NGX_THREADS 下事件可能由线程池分担处理 */

/* 以下 sig_atomic_t 标志是"信号处理函数 ↔ 主循环"的通信媒介：
 * C 标准保证该类型的读写相对信号处理是原子的，主循环醒来后逐个消费置位标志 */
sig_atomic_t  ngx_reap;         /* SIGCHLD 到位：有子进程退出，需收割进程表并决定是否重生 */
sig_atomic_t  ngx_timer;        /* SIGALRM 到位：worker 生命周期定时器到期，触发 worker 换代 */
sig_atomic_t  ngx_sigio;        /* SIGIO 到位：异步 I/O 通知（本版本 master 主循环的占位处理） */
sig_atomic_t  ngx_terminate;    /* SIGTERM/SIGINT 到位：要求快速终止（不等待连接排空） */
sig_atomic_t  ngx_quit;         /* SIGQUIT 到位：要求优雅退出（先停收新连接、等存量连接处理完） */
ngx_uint_t    ngx_exiting;      /* worker"退出中"标志：监听 socket 已关闭，等现有连接排空 */
sig_atomic_t  ngx_reconfigure;  /* SIGHUP 到位：重读配置并平滑重启 worker（不中断服务的热更新） */
sig_atomic_t  ngx_reopen;       /* SIGUSR1 到位：重开日志文件（配合 logrotate 的日志切割） */

sig_atomic_t  ngx_change_binary;  /* SIGUSR2 到位：exec 新二进制实现平滑升级 */
ngx_pid_t     ngx_new_binary;     /* 新二进制 master 的 PID：平滑升级期间新老 master 进程共存 */
ngx_uint_t    ngx_inherited;      /* 监听 fd 是否继承自老 master（升级时经环境变量移交，fd 复制=共享打开文件表项） */
ngx_uint_t    ngx_daemonized;     /* 是否已守护进程化：daemon 模式下脱离控制终端在后台运行 */

sig_atomic_t  ngx_noaccept;    /* "不再接受新连接"指令到位：只退 worker 不退 master（为升级/回退留后路） */
ngx_uint_t    ngx_noaccepting; /* 记录当前正处于"无 worker 接受连接"状态 */
ngx_uint_t    ngx_restart;     /* 升级回退后要求重新拉起 worker 恢复服务的标志 */


#if (NGX_THREADS)  /* 条件编译：开启线程支持时才定义线程描述表 */
volatile ngx_thread_t  ngx_threads[NGX_MAX_THREADS];  /* worker 内线程池描述表；volatile 禁止编译器缓存跨线程可见的状态字段 */
ngx_int_t              ngx_threads_n;  /* 实际创建的工作线程数（0 表示不启用线程池） */
#endif  /* 结束 NGX_THREADS 条件编译 */


u_char  master_process[] = "master process";  /* 进程标题前缀：setproctitle 时与命令行拼接，便于 ps/top 识别 master 进程 */


/* master 进程主循环入口：master 自身不处理任何连接，只阻塞在 sigsuspend() 等待信号，
 * 醒来后按各 sig_atomic_t 标志驱动管理动作（拉起/通知/收割 worker、重读配置、重开日志、平滑升级）。
 * 这是"信号驱动的监督进程(supervisor)"模型：并发全部下放 worker，master 逻辑天然单线程无锁。 */
void ngx_master_process_cycle(ngx_cycle_t *cycle, ngx_master_ctx_t *ctx)  /* ctx 携带原始 argc/argv：改写进程标题与 exec 新二进制时要用 */
{
    char              *title;  /* 拼装完成的进程标题缓冲区 */
    u_char            *p;      /* 写入标题的游标指针 */
    size_t             size;   /* 标题所需总字节数 */
    ngx_int_t          i;      /* 通用循环下标 */
    sigset_t           set;    /* 信号集：先用于阻塞信号，后作为 sigsuspend 的等待掩码 */
    struct timeval     tv;     /* 接收 gettimeofday 结果，刷新 nginx 缓存的日历时间 */
    struct itimerval   itv;    /* 实时定时器结构：实现优雅退出的指数退避等待 */
    ngx_uint_t         live;   /* 是否还有存活 worker：决定 master 能否安全退出 */
    ngx_msec_t         delay;  /* 终止流程的等待延时(毫秒)，按 2 的幂指数增长，超限升级为 SIGKILL */
    ngx_core_conf_t   *ccf;    /* ngx_core_module 配置：worker_processes 数量、user 等 */

    sigemptyset(&set);  /* 清空信号集，随后逐个加入 master 需要自行消费的管理信号 */
    sigaddset(&set, SIGCHLD);  /* 子进程退出信号：worker 崩溃/正常退出后触发收割与重生 */
    sigaddset(&set, SIGALRM);  /* 实时定时器信号：setitimer 到期唤醒 sigsuspend，驱动超时重试 */
    sigaddset(&set, SIGIO);    /* 异步 I/O 信号：加入掩码保证管理信号集完整 */
    sigaddset(&set, SIGINT);   /* 终端中断(Ctrl-C)：等价 SIGTERM 的快速终止 */
    sigaddset(&set, ngx_signal_value(NGX_RECONFIGURE_SIGNAL));  /* SIGHUP：重读配置、平滑重启 worker */
    sigaddset(&set, ngx_signal_value(NGX_REOPEN_SIGNAL));       /* SIGUSR1：重开日志文件（日志切割） */
    sigaddset(&set, ngx_signal_value(NGX_NOACCEPT_SIGNAL));     /* SIGQUIT：停止接受新连接（worker 优雅退出） */
    sigaddset(&set, ngx_signal_value(NGX_TERMINATE_SIGNAL));    /* SIGTERM：所有进程快速终止 */
    sigaddset(&set, ngx_signal_value(NGX_SHUTDOWN_SIGNAL));     /* 优雅退出信号：等 worker 处理完连接再退出 */
    sigaddset(&set, ngx_signal_value(NGX_CHANGEBIN_SIGNAL));    /* SIGUSR2：平滑升级二进制（新老 master 共存） */

    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) {  /* 阻塞上述信号：信号先挂起，由主循环 siguspend 同步消费，避免处理例程与主循环数据竞态 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,  /* 阻塞失败仅告警不退出：服务仍可运行，但存在竞态风险 */
                      "sigprocmask() failed");  /* 日志文本：失败的系统调用名 */
    }  /* sigprocmask 阻塞失败告警块结束 */

    sigemptyset(&set);  /* 清空为空掩码：sigsuspend 期间临时放行全部（含刚被阻塞的）信号 */


    size = sizeof(master_process);  /* 标题长度先计入前缀 "master process"（含结尾 '\0'） */

    for (i = 0; i < ctx->argc; i++) {  /* 遍历原始命令行参数，累计标题总长 */
        size += ngx_strlen(ctx->argv[i]) + 1;  /* 每个参数长度 +1 个分隔空格 */
    }  /* 累计标题长度 for 循环结束 */

    title = ngx_palloc(cycle->pool, size);  /* 从 cycle 内存池分配标题缓冲区，随池统一释放无需 free */

    p = ngx_cpymem(title, master_process, sizeof(master_process) - 1);  /* 先拷入前缀，sizeof-1 去掉字面量自带的 '\0' */
    for (i = 0; i < ctx->argc; i++) {  /* 逐个拼接命令行参数 */
        *p++ = ' ';  /* 参数之间以空格分隔 */
        p = ngx_cpystrn(p, (u_char *) ctx->argv[i], size);  /* 拷贝参数（限制在 size 内，自动补 '\0'） */
    }  /* 拼接命令行参数 for 循环结束 */

    ngx_setproctitle(title);  /* 改写进程标题（覆写 argv 所在内存），ps 显示 "master process: <启动参数>" */


    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);  /* 从配置上下文取出核心模块配置 */

    ngx_start_worker_processes(cycle, ccf->worker_processes,  /* 按配置 fork N 个 worker：fd 继承使其可直接 accept 监听 socket */
                               NGX_PROCESS_RESPAWN);  /* RESPAWN：worker 意外死亡后 master 自动重生 */

    ngx_new_binary = 0;  /* 清零：本进程是原始启动的 master，而非平滑升级出的新 master */
    delay = 0;   /* 终止等待延时初值 0：尚未进入终止流程 */
    live = 1;    /* 先假定有存活 worker，防止误判导致提前退出 */

    for ( ;; ) {  /* master 无限主循环：一次 sigsuspend 睡眠 + 一次标志消费即一个周期 */
        if (delay) {  /* 处于"终止等待"状态时，安排下一次醒来的超时 */
            delay *= 2;  /* 延时翻倍（指数退避）：给 worker 越来越长的退出宽限期 */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,  /* 调试日志：输出本轮延时 */
                           "temination cycle: %d", delay);  /* 日志格式串 */

            itv.it_interval.tv_sec = 0;  /* 周期间隔秒清零：一次性定时器 */
            itv.it_interval.tv_usec = 0;  /* 周期间隔微秒清零 */
            itv.it_value.tv_sec = delay / 1000;  /* 定时器到期值的秒部分 */
            itv.it_value.tv_usec = (delay % 1000 ) * 1000;  /* 微秒部分：毫秒换算为微秒 */

            if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {  /* 启动实时定时器：到期内核向本进程发 SIGALRM 以唤醒 sigsuspend */
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,  /* 定时器失败仅告警 */
                              "setitimer() failed");  /* 日志文本 */
            }  /* setitimer 失败告警 if 块结束 */
        }  /* 终止等待延时处理 if 块结束 */

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "sigsuspend");  /* 调试日志：即将进入睡眠等信号 */

        sigsuspend(&set);  /* 原子地"换掩码+睡眠"：任一未阻塞信号到达即返回；这是 master 唯一阻塞点，不消耗 CPU */

        ngx_gettimeofday(&tv);  /* 醒来后取当前时刻 */
        ngx_time_update(tv.tv_sec);  /* 刷新 nginx 缓存时间：日志时间戳与定时器计算依赖它 */

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "wake up");  /* 调试日志：已被信号唤醒 */

        if (ngx_reap) {  /* SIGCHLD 到位：有子进程状态变化待收割 */
            ngx_reap = 0;  /* 消费标志：sig_atomic_t 保证信号函数与主循环间读写的原子性 */
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "reap childs");  /* 调试日志 */

            live = ngx_reap_childs(cycle);  /* waitpid 收割僵尸进程、按 respawn 策略重生，返回是否仍有存活 worker */
        }  /* ngx_reap 判断结束 */

        if (!live && (ngx_terminate || ngx_quit)) {  /* 所有 worker 均已退出且收到终止/退出指令 */
            ngx_master_exit(cycle, ctx);  /* 删 pid 文件、销毁内存池后正常退出 */
        }  /* 全部 worker 已退出判断结束 */

        if (ngx_terminate) {  /* SIGTERM/SIGINT：快速终止流程 */
            if (delay == 0) {  /* 首轮进入终止流程 */
                delay = 50;  /* 起始宽限 50ms，之后逐轮翻倍 */
            }  /* 首轮终止流程判断结束 */

            if (delay > 1000) {  /* 宽限累计超过 1 秒 worker 仍未退完 */
                ngx_signal_worker_processes(cycle, SIGKILL);  /* 升级为 SIGKILL 强杀：不可捕获不可忽略，保证最终能退出 */
            } else {  /* 未超1秒宽限的 else：继续发普通终止信号 */
                ngx_signal_worker_processes(cycle,  /* 宽限期内继续发普通终止信号 */
                                       ngx_signal_value(NGX_TERMINATE_SIGNAL));  /* 参数续行：要投递的信号 */
            }  /* 宽限期内发普通终止信号 else 分支结束 */

            continue;  /* 回循环顶：睡到 SIGALRM/下一信号再来 */
        }  /* ngx_terminate 分支结束 */

        if (ngx_quit) {  /* 优雅退出：不设超时，耐心等 worker 处理完存量连接 */
            ngx_signal_worker_processes(cycle,  /* 通知所有 worker 优雅退出 */
                                        ngx_signal_value(NGX_SHUTDOWN_SIGNAL));  /* 参数续行 */
            continue;  /* 继续循环直至 live==0 才真正退出 */
        }  /* ngx_quit 分支结束 */

        if (ngx_timer) {  /* SIGALRM：worker 生命周期定时器到期（10 天上限，见 start_worker_processes） */
            ngx_timer = 0;  /* 消费标志 */
            ngx_start_worker_processes(cycle, ccf->worker_processes,  /* 先拉起一批全新 worker 接管流量 */
                                       NGX_PROCESS_JUST_RESPAWN);  /* JUST_RESPAWN：新 worker 标记 just_respawn，本轮不被通知退出 */
            live = 1;  /* 新 worker 已存在 */
            ngx_signal_worker_processes(cycle,  /* 再让老 worker 优雅退出，完成 worker 代际更替 */
                                        ngx_signal_value(NGX_SHUTDOWN_SIGNAL));  /* 参数续行 */
        }  /* ngx_timer 分支结束 */

        if (ngx_reconfigure) {  /* SIGHUP：配置热更新——不中断服务的平滑重启 */
            ngx_reconfigure = 0;  /* 消费标志 */

            if (ngx_new_binary) {  /* 平滑升级场景：新二进制的 master 重读配置失败/需要补 worker 时 */
                ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "start new workers");  /* 日志：直接补拉 worker */

                ngx_start_worker_processes(cycle, ccf->worker_processes,  /* 用现有配置直接拉起 worker */
                                           NGX_PROCESS_RESPAWN);  /* 支持崩溃自动重生 */
                ngx_noaccepting = 0;  /* 恢复"正常接受连接"状态 */

                continue;  /* 本轮结束，回循环顶睡眠 */
            }  /* 新二进制补拉 worker 分支结束 */

            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "reconfiguring");  /* 日志：开始重读配置 */

            cycle = ngx_init_cycle(cycle);  /* 以老 cycle 为模板重新解析配置、重开监听/日志，生成全新 cycle */
            if (cycle == NULL) {  /* 新配置解析失败 */
                cycle = (ngx_cycle_t *) ngx_cycle;  /* 回滚到老 cycle：坏配置不影响线上服务（热更新安全性关键） */
                continue;  /* 放弃本次重载 */
            }  /* 新 cycle 解析失败回滚判断结束 */

            ngx_cycle = cycle;  /* 全局 cycle 指针切换到新配置 */
            ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx,  /* 重新获取新 cycle 的核心配置 */
                                                   ngx_core_module);  /* 参数续行：核心模块 */
            ngx_start_worker_processes(cycle, ccf->worker_processes,  /* 用新配置 fork 新一代 worker */
                                       NGX_PROCESS_JUST_RESPAWN);  /* JUST_RESPAWN：老 worker 本轮也要被通知退出 */
            live = 1;  /* 新 worker 存活 */
            ngx_signal_worker_processes(cycle,  /* 通知旧 worker 携带旧配置优雅退出 */
                                        ngx_signal_value(NGX_SHUTDOWN_SIGNAL));  /* 参数续行 */
        }  /* ngx_reconfigure 分支结束 */

        if (ngx_restart) {  /* 升级回退场景：老 master 曾停掉全部 worker，现在重新拉起恢复服务 */
            ngx_restart = 0;  /* 消费标志 */
            ngx_start_worker_processes(cycle, ccf->worker_processes,  /* 按当前配置补齐 worker */
                                       NGX_PROCESS_RESPAWN);  /* 支持崩溃重生 */
            live = 1;  /* 有存活 worker */
        }  /* ngx_restart 分支结束 */

        if (ngx_reopen) {  /* SIGUSR1：重开日志文件（日志切割的标准流程） */
            ngx_reopen = 0;  /* 消费标志 */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "reopening logs");  /* 日志 */
            ngx_reopen_files(cycle, ccf->user);  /* master 关旧 fd、按配置属主重新 open：旧文件即可被 logrotate 安全移走 */
            ngx_signal_worker_processes(cycle,  /* 广播让所有 worker 同步重开自己的日志 fd */
                                        ngx_signal_value(NGX_REOPEN_SIGNAL));  /* 参数续行 */
        }  /* ngx_reopen 分支结束 */

        if (ngx_change_binary) {  /* SIGUSR2：平滑升级二进制 */
            ngx_change_binary = 0;  /* 消费标志 */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "changing binary");  /* 日志 */
            ngx_new_binary = ngx_exec_new_binary(cycle, ctx->argv);  /* fork+exec 新二进制：监听 fd 经环境变量移交，新老 master 共存 */
        }  /* ngx_change_binary 分支结束 */

        if (ngx_noaccept) {  /* "不再接受新连接"：只退 worker 保留 master（供平滑升级中途回退） */
            ngx_noaccept = 0;  /* 消费标志 */
            ngx_noaccepting = 1;  /* 标记处于无 worker 服务状态 */
            ngx_signal_worker_processes(cycle,  /* 通知全部 worker 优雅退出 */
                                        ngx_signal_value(NGX_SHUTDOWN_SIGNAL));  /* 参数续行 */
        }  /* ngx_noaccept 分支结束 */
    }  /* for 主循环体结束：回到 sigsuspend 睡眠等下一个信号 */
}  /* ngx_master_process_cycle 结束 */


/* 单进程模式主循环（master_process off / 调试场景）：本进程既当 master 又当 worker，
 * 直接跑事件循环处理连接；信号置位标志在每轮事件处理后消费。
 * 无 fork 隔离：任何模块崩溃即整机退出，故生产环境推荐多进程模式。 */
void ngx_single_process_cycle(ngx_cycle_t *cycle, ngx_master_ctx_t *ctx)  /* ctx 供退出例程使用 */
{
    ngx_uint_t  i;  /* 模块数组的循环下标 */

#if 0  /* 条件编译禁用的调试代码 */
    ngx_setproctitle("single worker process");  /* 原拟改写进程标题，已禁用 */
#endif  /* 结束 #if 0 */

    ngx_init_temp_number();  /* 初始化临时文件名编号计数器 */

    for (i = 0; ngx_modules[i]; i++) {  /* 遍历全部静态链接模块，执行进程级初始化 */
        if (ngx_modules[i]->init_process) {  /* 仅调用注册了 init_process 钩子的模块 */
            if (ngx_modules[i]->init_process(cycle) == NGX_ERROR) {  /* 初始化失败即致命：单进程没有 master 兜底重生 */
                /* fatal */  /* 原有英文注释：致命错误 */
                exit(2);  /* 以非零状态终止进程 */
            }  /* init_process 失败判断结束 */
        }  /* init_process 钩子判断结束 */
    }  /* 模块进程初始化遍历结束 */

    for ( ;; ) {  /* 事件主循环：epoll_wait 等待 I/O 就绪与定时器超时 */
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "worker cycle");  /* 调试日志：一轮循环开始 */

        ngx_process_events(cycle);  /* 一轮事件处理：等 epoll 返回并分发读/写/定时事件（事件驱动的核心入口） */

        if (ngx_terminate || ngx_quit) {  /* 收到快速终止或优雅退出信号 */
            ngx_master_exit(cycle, ctx);  /* 清理 pid 文件与内存池后退出（单进程退出即服务下线） */
        }  /* 终止/退出判断结束 */

        if (ngx_reconfigure) {  /* SIGHUP：重读配置 */
            ngx_reconfigure = 0;  /* 消费标志 */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "reconfiguring");  /* 日志 */

            cycle = ngx_init_cycle(cycle);  /* 基于老 cycle 重新解析配置生成新 cycle */
            if (cycle == NULL) {  /* 新配置解析失败 */
                cycle = (ngx_cycle_t *) ngx_cycle;  /* 回滚老 cycle，服务继续用旧配置 */
                continue;  /* 跳过本轮后续处理 */
            }  /* 新 cycle 解析失败回滚判断结束 */

            ngx_cycle = cycle;  /* 全局指针切换到新 cycle */
        }  /* ngx_reconfigure 分支结束 */

        if (ngx_reopen) {  /* SIGUSR1：重开日志 */
            ngx_reopen = 0;  /* 消费标志 */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "reopening logs");  /* 日志 */
            ngx_reopen_files(cycle, (ngx_uid_t) -1);  /* 重开日志文件；-1 表示不改变文件属主 */
        }  /* ngx_reopen 分支结束 */
    }  /* 主循环体结束 */
}  /* ngx_single_process_cycle 结束 */


/* 按配置数量 fork worker 进程，并把新进程信息经通道广播给所有既有进程。
 * 关键机理：worker 由 master fork 而来，继承 master 已打开的全部监听 socket fd
 * （fork 的 fd 复制=父子共享同一打开文件表项），故 worker 无需重新 bind/listen 即可 accept；
 * 多进程还带来故障隔离：单个 worker crash 不影响其他 worker，由 master respawn 兜底。 */
static void ngx_start_worker_processes(ngx_cycle_t *cycle, ngx_int_t n,  /* n：要启动的 worker 数（worker_processes 指令值） */
                                       ngx_int_t type)  /* 续行：NGX_PROCESS_RESPAWN / JUST_RESPAWN 等创建类型 */
{
    ngx_int_t         i;    /* 通道广播的循环下标 */
    ngx_channel_t     ch;   /* 通道消息：新进程的 pid / 槽位 / 通道 fd 三元组 */
    struct itimerval  itv;  /* worker 生命周期上限定时器结构 */

    ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "start worker processes");  /* 日志：开始拉起 worker */

    ch.command = NGX_CMD_OPEN_CHANNEL;  /* 消息命令字置为"打开通道"：宣告新 worker 的存在 */

    while (n--) {  /* 循环 n 次，每次 fork 一个 worker */
        ngx_spawn_process(cycle, ngx_worker_process_cycle, NULL,  /* fork 子进程并在 ngx_processes[] 表中登记槽位 */
                          "worker process", type);  /* 续行：进程名与创建类型（respawn 策略） */

        ch.pid = ngx_processes[ngx_process_slot].pid;  /* 取刚 fork 出的子进程 PID */
        ch.slot = ngx_process_slot;  /* 该子进程在全局进程表中的槽位号 */
        ch.fd = ngx_processes[ngx_process_slot].channel[0];  /* 其通道(socketpair)的读端 fd */

        for (i = 0; i < ngx_last_process; i++) {  /* 遍历进程表，向所有已登记进程广播新 worker 信息 */

            if (i == ngx_process_slot  /* 跳过新 worker 自己 */
                || ngx_processes[i].pid == -1  /* 跳过空槽位 */
                || ngx_processes[i].channel[0] == -1)  /* 跳过没有通道的进程 */
            {  /* if 条件跨行结束 */
                continue;  /* 不向其发送消息 */
            }  /* 跳过自身/空槽/无通道进程的 if 结束 */

            ngx_log_debug6(NGX_LOG_DEBUG_CORE, cycle->log, 0,  /* 调试日志：输出通道传递细节 */
                           "pass channel s:%d pid:" PID_T_FMT  /* 格式串跨行书写 */
                           " fd:%d to s:%d pid:" PID_T_FMT " fd:%d",  /* 格式串续行 */
                           ch.slot, ch.pid, ch.fd,  /* 新进程三元组 */
                           i, ngx_processes[i].pid,  /* 目标槽位与其 PID */
                           ngx_processes[i].channel[0]);  /* 目标进程通道 fd */

            /* TODO: NGX_AGAIN */  /* 原有 TODO：通道写返回 NGX_AGAIN（缓冲满）时尚未重试 */

            ngx_write_channel(ngx_processes[i].channel[0],  /* 经 unix 域 socketpair 把 OPEN_CHANNEL 消息发给每个进程 */
                              &ch, sizeof(ngx_channel_t), cycle->log);  /* 续行：消息体与日志参数 */
        }  /* 通道广播遍历结束 */
    }  /* 逐个 fork worker 的 while 循环结束 */

    /*
     * we have to limit the maximum life time of the worker processes
     * by 10 days because our millisecond event timer is limited
     * by 24 days on 32-bit platforms
     */  /* 原有英文注释：32 位平台毫秒定时器上限约 24 天，故强制 worker 每 10 天换代一次 */

    itv.it_interval.tv_sec = 0;  /* 周期间隔秒清零：一次性定时器 */
    itv.it_interval.tv_usec = 0;  /* 周期间隔微秒清零 */
    itv.it_value.tv_sec = 10 * 24 * 60 * 60;  /* 到期值 = 10 天（秒） */
    itv.it_value.tv_usec = 0;  /* 微秒部分为 0 */

    if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {  /* 启动实时定时器：到期 SIGALRM 置 ngx_timer，触发 worker 换代 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,  /* 定时器设置失败仅告警 */
                      "setitimer() failed");  /* 日志文本 */
    }  /* setitimer 失败告警块结束 */
}  /* ngx_start_worker_processes 结束 */


/* 向所有受管 worker 投递信号。设计要点：凡能映射为通道命令的（quit/terminate/reopen）
 * 优先走 unix 域 socketpair 通道——命令在 worker 的事件循环里被同步处理，无异步信号安全问题；
 * 通道写失败才退回 kill() 内核信号机制，SIGKILL 等无通道语义的信号只能走 kill。 */
static void ngx_signal_worker_processes(ngx_cycle_t *cycle, int signo)  /* signo：要投递的信号编号 */
{
    ngx_int_t      i;    /* 进程表循环下标 */
    ngx_err_t      err;  /* 保存 kill 失败的 errno */
    ngx_channel_t  ch;   /* 与信号语义等价的通道命令消息 */


    switch (signo) {  /* 把信号编号翻译成等价的通道命令字 */

    case ngx_signal_value(NGX_SHUTDOWN_SIGNAL):  /* 优雅退出信号 */
        ch.command = NGX_CMD_QUIT;  /* 映射为通道 QUIT 命令 */
        break;  /* case 分支结束 */

    case ngx_signal_value(NGX_TERMINATE_SIGNAL):  /* 快速终止信号 */
        ch.command = NGX_CMD_TERMINATE;  /* 映射为通道 TERMINATE 命令 */
        break;  /* case 分支结束 */

    case ngx_signal_value(NGX_REOPEN_SIGNAL):  /* 重开日志信号 */
        ch.command = NGX_CMD_REOPEN;  /* 映射为通道 REOPEN 命令 */
        break;  /* case 分支结束 */

    default:  /* 其他信号（如 SIGKILL）无通道命令语义 */
        ch.command = 0;  /* 置 0：只能用 kill() 投递 */
    }  /* 信号到通道命令翻译 switch 结束 */

    ch.fd = -1;  /* 本消息不携带描述符：不用 SCM_RIGHTS 传 fd */


    for (i = 0; i < ngx_last_process; i++) {  /* 遍历进程表逐个投递 */

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, cycle->log, 0,  /* 调试日志：打印子进程状态位图 */
                       "child: %d " PID_T_FMT " e:%d t:%d d:%d r:%d j:%d",  /* 格式串 */
                       i,  /* 槽位号 */
                       ngx_processes[i].pid,  /* 子进程 PID */
                       ngx_processes[i].exiting,  /* 退出中标志 */
                       ngx_processes[i].exited,   /* 已退出标志 */
                       ngx_processes[i].detached, /* 已脱离管理（如升级出的新 master） */
                       ngx_processes[i].respawn,  /* 崩溃重生开关 */
                       ngx_processes[i].just_respawn);  /* 刚换代重生标记 */

        if (ngx_processes[i].detached || ngx_processes[i].pid == -1) {  /* 跳过已脱离管理的进程与空槽位 */
            continue;  /* 不投递 */
        }  /* 跳过脱离/空槽进程 if 结束 */

        if (ngx_processes[i].just_respawn) {  /* 刚被"换代"拉起的新 worker */
            ngx_processes[i].just_respawn = 0;  /* 清除一次性标记 */
            continue;  /* 新 worker 不需要退出类通知，跳过 */
        }  /* 跳过刚重生 worker 的 if 结束 */

        if (ngx_processes[i].exiting  /* 该 worker 已在退出流程中 */
            && signo == ngx_signal_value(NGX_SHUTDOWN_SIGNAL))  /* 又收到优雅退出信号 */
        {  /* if 条件跨行结束 */
            continue;  /* 不重复打扰，避免打断其既有退出节奏 */
        }  /* 跳过退出中 worker 的 if 结束 */

        if (ch.command) {  /* 存在等价的通道命令 */
            if (ngx_write_channel(ngx_processes[i].channel[0],  /* 优先经通道投递：可靠且在事件循环内被处理 */
                           &ch, sizeof(ngx_channel_t), cycle->log) == NGX_OK)  /* 续行：写入成功判定 */
            {  /* if 块开始 */
                if (signo != ngx_signal_value(NGX_REOPEN_SIGNAL)) {  /* 退出类命令（quit/terminate） */
                    ngx_processes[i].exiting = 1;  /* 标记该 worker 进入退出流程 */
                }  /* 退出类命令标记 if 结束 */

                continue;  /* 通道已送达，无需再 kill */
            }  /* 通道投递成功 if 块结束 */
        }  /* 通道命令投递 if 结束 */

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, cycle->log, 0,  /* 调试日志：记录退回 kill 投递 */
                       "kill (" PID_T_FMT ", %d)" ,  /* 格式串（原文逗号前有空格，保持原样） */
                       ngx_processes[i].pid, signo);  /* 目标 PID 与信号号 */

        if (kill(ngx_processes[i].pid, signo) == -1) {  /* 直接系统调用发信号；-1 表示投递失败 */
            err = ngx_errno;  /* 保存 errno 供判断与日志 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, err,  /* 告警日志 */
                          "kill(%d, %d) failed",  /* 日志文本 */
                          ngx_processes[i].pid, signo);  /* 参数：目标 PID 与信号 */

            if (err == NGX_ESRCH) {  /* ESRCH：目标进程已不存在（刚死掉） */
                ngx_processes[i].exited = 1;  /* 直接标记已退出 */
                ngx_processes[i].exiting = 0;  /* 退出流程视为结束 */
                ngx_reap = 1;  /* 置收割标志：唤醒主循环去回收该槽位 */
            }  /* ESRCH 处理 if 结束 */

            continue;  /* 处理下一个进程 */
        }  /* kill 失败处理 if 结束 */

        if (signo != ngx_signal_value(NGX_REOPEN_SIGNAL)) {  /* 非 reopen 的退出类信号 */
            ngx_processes[i].exiting = 1;  /* 标记进入退出流程 */
        }  /* 退出类信号标记 if 结束 */
    }  /* 进程表遍历结束 */
}  /* ngx_signal_worker_processes 结束 */


/* 收割子进程：SIGCHLD 唤醒 master 后进入此处（SIGCHLD 处理函数只置 ngx_reap 标志，
 * 实际 waitpid 收尸放在主循环，避免在信号处理上下文做复杂操作）。
 * 职责：关闭死亡进程通道并广播 CLOSE_CHANNEL；按 respawn 策略原地重生；
 * 回收进程表槽位；返回是否仍有存活 worker 供退出判断。 */
static ngx_uint_t ngx_reap_childs(ngx_cycle_t *cycle)  /* 返回 live：1 表示仍有 worker 在运行 */
{
    ngx_int_t      i, n;   /* i：外层扫描槽位；n：广播 CLOSE_CHANNEL 的下标 */
    ngx_uint_t     live;   /* 存活标记 */
    ngx_channel_t  ch;     /* CLOSE_CHANNEL 消息体 */

    ch.command = NGX_CMD_CLOSE_CHANNEL;  /* 命令字：关闭某个已亡进程的通道 */
    ch.fd = -1;  /* 本消息不携带 fd */

    live = 0;  /* 先假设没有存活 worker，扫描中再置位 */
    for (i = 0; i < ngx_last_process; i++) {  /* 线性扫描整个进程表 */

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, cycle->log, 0,  /* 调试日志：子进程状态 */
                       "child: %d " PID_T_FMT " e:%d t:%d d:%d r:%d j:%d",  /* 格式串 */
                       i,  /* 槽位号 */
                       ngx_processes[i].pid,  /* PID */
                       ngx_processes[i].exiting,  /* 退出中 */
                       ngx_processes[i].exited,   /* 已退出 */
                       ngx_processes[i].detached, /* 已脱离 */
                       ngx_processes[i].respawn,  /* 重生开关 */
                       ngx_processes[i].just_respawn);  /* 刚重生标记 */

        if (ngx_processes[i].pid == -1) {  /* 空槽位 */
            continue;  /* 跳过 */
        }  /* 空槽位跳过 if 结束 */

        if (ngx_processes[i].exited) {  /* 该子进程已退出（信号处理函数中已 waitpid 收尸，避免僵尸进程） */

            if (!ngx_processes[i].detached) {  /* 非脱离进程才需要清理其通道 */

                ngx_close_channel(ngx_processes[i].channel, cycle->log);  /* 关闭本进程持有的该 socketpair 两端 */

                ngx_processes[i].channel[0] = -1;  /* 槽位读端 fd 复位 */
                ngx_processes[i].channel[1] = -1;  /* 槽位写端 fd 复位 */

                ch.pid = ngx_processes[i].pid;  /* 消息携带死亡进程的 PID */
                ch.slot = i;  /* 及其槽位号 */

                for (n = 0; n < ngx_last_process; n++) {  /* 通知所有存活进程删除指向死者的通道 */
                    if (ngx_processes[n].exited  /* 已退出的进程不用通知 */
                        || ngx_processes[n].pid == -1  /* 空槽位跳过 */
                        || ngx_processes[n].channel[0] == -1)  /* 无通道的进程跳过 */
                    {  /* if 条件跨行结束 */
                        continue;  /* 不通知 */
                    }  /* 跳过已亡/空槽/无通道进程的 if 结束 */

                    ngx_log_debug3(NGX_LOG_DEBUG_CORE, cycle->log, 0,  /* 调试日志 */
                       "pass close channel s:%d pid:" PID_T_FMT  /* 格式串跨行 */
                       " to:" PID_T_FMT, ch.slot, ch.pid, ngx_processes[n].pid);  /* 格式串续行与参数 */

                    /* TODO: NGX_AGAIN */  /* 原有 TODO：通道写 NGX_AGAIN 未重试 */

                    ngx_write_channel(ngx_processes[n].channel[0],  /* 广播 CLOSE_CHANNEL 消息 */
                                      &ch, sizeof(ngx_channel_t), cycle->log);  /* 续行：消息体与日志 */
                }  /* CLOSE_CHANNEL 广播遍历结束 */
            }  /* 非脱离进程通道清理 if 结束 */

            if (ngx_processes[i].respawn  /* 该槽位配置了崩溃自动重生 */
                && !ngx_processes[i].exiting  /* 且并非 master 主动要求其退出 */
                && !ngx_terminate  /* master 不在快速终止流程 */
                && !ngx_quit)  /* 也不在优雅退出流程 */
            {  /* if 条件跨行结束 */
                if (ngx_spawn_process(cycle, ngx_processes[i].proc,  /* 原地重生：复用原入口函数 */
                                      ngx_processes[i].data,  /* 续行：原参数 */
                                      ngx_processes[i].name, i)  /* 续行：原进程名与指定槽位号 */
                                                                  == NGX_ERROR)  /* 续行：重生失败判定 */
                {  /* if 块开始 */
                    ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,  /* 告警：重生失败 */
                                  "can not respawn %s", ngx_processes[i].name);  /* 日志文本 */
                    continue;  /* 放弃该槽位，继续扫描 */
                }  /* 重生失败处理 if 结束 */

                live = 1;  /* 有重生的新 worker 存活 */

                continue;  /* 该槽位处理完毕 */
            }  /* 崩溃自动重生判断块结束 */

            if (ngx_processes[i].pid == ngx_new_binary) {  /* 退出的是平滑升级启动的新二进制 master */
                ngx_new_binary = 0;  /* 清除新二进制 PID 记录 */
                if (ngx_noaccepting) {  /* 老 master 此前已停止接受连接（等待升级） */
                    ngx_restart = 1;  /* 置重启标志：重新拉起 worker 恢复服务（升级失败回退路径） */
                    ngx_noaccepting = 0;  /* 恢复接受连接状态 */
                }  /* noaccepting 回退处理 if 结束 */
            }  /* 新二进制 master 退出判断结束 */

            if (i == ngx_last_process - 1) {  /* 死亡的是表尾进程 */
                ngx_last_process--;  /* 直接收缩表长，O(1) 回收槽位 */

            } else {  /* 中间槽位不能搬移（slot 号被各进程通道消息引用，挪动会错乱） */
                ngx_processes[i].pid = -1;  /* 标记为空槽位，供下次 spawn 复用 */
            }  /* 中间槽位标记空槽 else 分支结束 */

        } else if (ngx_processes[i].exiting || !ngx_processes[i].detached) {  /* 仍在运行（或退出中）的受管进程 */
            live = 1;  /* 记录存在存活者 */
        }  /* 存活 worker 记录 else-if 分支结束 */
    }  /* 进程表扫描结束 */

    return live;  /* 返回是否仍有存活 worker：决定 master 能否退出 */
}  /* ngx_reap_childs 结束 */


/* master/单进程模式的退出例程：确认所有 worker 已终止后清理现场再退出。 */
static void ngx_master_exit(ngx_cycle_t *cycle, ngx_master_ctx_t *ctx)  /* ctx 参数当前未使用，保留接口一致性 */
{
    ngx_delete_pidfile(cycle);  /* 删除 pid 文件：防止残留过期 PID 误导管理脚本 stop/reload */

    ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "exit");  /* 日志：进程即将退出 */

    ngx_destroy_pool(cycle->pool);  /* 销毁 cycle 内存池：池上所有分配一次性释放（nginx 池式内存管理） */

    exit(0);  /* 以状态 0 正常退出：内核回收全部资源，孤儿 worker 由 init 收养 */
}  /* ngx_master_exit 函数结束 */


/* worker 进程主循环入口（由 ngx_spawn_process 在子进程中调用）：
 * 先降权/初始化事件模块（把监听 fd 与通道 fd 注册进 epoll），再进入事件主循环。
 * 退出两条路径：ngx_terminate 立即退出；ngx_quit 关闭监听后等存量连接排空再退出。
 * 单进程即可驱动数千连接：事件驱动+非阻塞 I/O，无锁无线程切换开销，多 worker 分摊多核。 */
static void ngx_worker_process_cycle(ngx_cycle_t *cycle, void *data)  /* data 未使用：worker 无需额外参数 */
{
    sigset_t           set;  /* 信号掩码集合 */
    ngx_err_t          err;  /* 错误码暂存 */
    ngx_int_t          n;    /* 进程表循环下标 */
    ngx_uint_t         i;    /* 通用循环下标 */
    struct timeval     tv;   /* 取当前时刻用于初始化时间基准 */
    ngx_listening_t   *ls;   /* 监听 socket 数组指针 */
    ngx_core_conf_t   *ccf;  /* 核心配置：user/group 等 */
    ngx_connection_t  *c;    /* 连接对象指针（本版本在此预留未用） */


    ngx_gettimeofday(&tv);  /* 取当前时刻 */

    ngx_start_msec = (ngx_epoch_msec_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;  /* 换算毫秒时间戳作为 worker 计时起点 */
    ngx_old_elapsed_msec = 0;  /* 上一轮事件循环时刻清零 */
    ngx_elapsed_msec = 0;      /* 当前时刻清零 */


    ngx_process = NGX_PROCESS_WORKER;  /* 设置全局进程角色为 worker：模块与信号处理据此切换行为 */

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);  /* 读取核心模块配置 */

    if (ccf->group != (gid_t) NGX_CONF_UNSET) {  /* 配置了 group 指令才需要降组 */
        if (setgid(ccf->group) == -1) {  /* 设置组 ID：先降组再降用户，保持降权顺序正确 */
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,  /* 失败即紧急日志 */
                          "setgid(%d) failed", ccf->group);  /* 日志文本 */
            /* fatal */  /* 原有英文注释：致命错误 */
            exit(2);  /* worker 无法安全运行即退出，由 master respawn 兜底 */
        }  /* setgid 失败处理结束 */
    }  /* 降组处理 if 块结束 */

    if (ccf->user != (uid_t) NGX_CONF_UNSET) {  /* 配置了 user 指令才需要降权 */
        if (setuid(ccf->user) == -1) {  /* 放弃 root 特权：低端口监听已在 master(root) 阶段完成，worker 只管连接 */
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,  /* 失败紧急日志 */
                          "setuid(%d) failed", ccf->user);  /* 日志文本 */
            /* fatal */  /* 原有英文注释：致命错误 */
            exit(2);  /* 退出 */
        }  /* setuid 失败处理结束 */
    }  /* 降权处理 if 块结束 */

#if (HAVE_PR_SET_DUMPABLE)  /* 条件编译：平台支持 prctl 的 PR_SET_DUMPABLE */

    /* allow coredump after setuid() in Linux 2.4.x */  /* 原有英文注释：setuid 后恢复可转储 */

    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1) {  /* 恢复 dumpable 标志：内核对 setuid 后进程默认禁止 core dump，重开以便事后排障 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,  /* 失败仅告警 */
                      "prctl(PR_SET_DUMPABLE) failed");  /* 日志文本 */
    }  /* prctl 恢复 dumpable 告警块结束 */

#endif  /* 结束 HAVE_PR_SET_DUMPABLE 条件编译 */

    sigemptyset(&set);  /* 构造空信号掩码 */

    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1) {  /* 解除从 master 继承的全部阻塞信号：worker 有自己的信号处理与标志位体系 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,  /* 失败仅告警 */
                      "sigprocmask() failed");  /* 日志文本 */
    }  /* 解除阻塞信号失败告警块结束 */

    ngx_init_temp_number();  /* 初始化临时文件名编号计数器 */

    /*
     * disable deleting previous events for the listening sockets because
     * in the worker processes there are no events at all at this point
     */  /* 原有英文注释：worker 此时尚无任何事件，无需对监听 socket 做旧事件清理 */
    ls = cycle->listening.elts;  /* 取监听 socket 数组首地址 */
    for (i = 0; i < cycle->listening.nelts; i++) {  /* 遍历所有监听 socket */
        ls[i].remain = 0;  /* 清除"保留旧事件"标志：本进程 epoll 中 fd 是全新注册的 */
    }  /* 监听 socket remain 清零遍历结束 */

    for (i = 0; ngx_modules[i]; i++) {  /* 遍历模块执行进程级初始化：事件模块在此把监听 fd 注册进本进程 epoll */
        if (ngx_modules[i]->init_process) {  /* 仅调用注册了钩子的模块 */
            if (ngx_modules[i]->init_process(cycle) == NGX_ERROR) {  /* 初始化失败 */
                /* fatal */  /* 原有英文注释：致命错误 */
                exit(2);  /* 退出，交由 master respawn */
            }  /* init_process 失败判断结束 */
        }  /* init_process 钩子判断结束 */
    }  /* 模块进程初始化遍历结束 */

    for (n = 0; n < ngx_last_process; n++) {  /* 清理 fork 继承来的无关通道写端 */

        if (ngx_processes[n].pid == -1) {  /* 空槽位 */
            continue;  /* 跳过 */
        }  /* 空槽位跳过结束 */

        if (n == ngx_process_slot) {  /* 自己的槽位 */
            continue;  /* 跳过 */
        }  /* 自身槽位跳过结束 */

        if (ngx_processes[n].channel[1] == -1) {  /* 该槽位无写端可关 */
            continue;  /* 跳过 */
        }  /* 无写端跳过结束 */

        if (close(ngx_processes[n].channel[1]) == -1) {  /* 关闭他人通道写端：fd 引用计数减一，防止误写他人通道 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,  /* 关闭失败仅告警 */
                          "close() failed");  /* 日志文本 */
        }  /* close 失败告警块结束 */
    }  /* 关闭他人通道写端遍历结束 */

    if (close(ngx_processes[ngx_process_slot].channel[0]) == -1) {  /* 关闭自己通道的读端：worker 统一经全局 ngx_channel（channel[1]）收消息 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,  /* 关闭失败仅告警 */
                      "close() failed");  /* 日志文本 */
    }  /* 关闭自身通道读端告警块结束 */

#if 0  /* 条件编译禁用的调试代码 */
    ngx_last_process = 0;  /* 原拟清空进程表，已禁用 */
#endif  /* 结束 #if 0 */

    if (ngx_add_channel_event(cycle, ngx_channel, NGX_READ_EVENT,  /* 把通道 fd 注册进 epoll：master 命令以"通道可读"事件进入事件循环 */
                                             ngx_channel_handler) == NGX_ERROR)  /* 续行：读事件回调为 ngx_channel_handler */
    {  /* if 块开始 */
        /* fatal */  /* 原有英文注释：致命错误 */
        exit(2);  /* 通道是 master 管理 worker 的生命线，注册失败即退出 */
    }  /* 通道事件注册失败处理结束 */

    ngx_setproctitle("worker process");  /* 改进程标题为 "worker process"，便于 ps 区分进程角色 */

#if (NGX_THREADS)  /* 条件编译：线程支持（可选特性，默认单线程事件循环） */

    if (ngx_time_mutex_init(cycle->log) == NGX_ERROR) {  /* 初始化时间缓存互斥锁：多线程并发读 gettimeofday 缓存需保护 */
        /* fatal */  /* 原有英文注释：致命错误 */
        exit(2);  /* 退出 */
    }  /* 时间锁初始化失败处理结束 */

    if (ngx_threads_n) {  /* 配置了线程数才创建线程池 */
        if (ngx_init_threads(ngx_threads_n,  /* 初始化线程基础设施（线程表/信号处理等） */
                                   ccf->thread_stack_size, cycle) == NGX_ERROR)  /* 续行：栈大小与 cycle */
        {  /* if 块开始 */
            /* fatal */  /* 原有英文注释：致命错误 */
            exit(2);  /* 退出 */
        }  /* 线程基础设施初始化失败处理结束 */

        err = ngx_thread_key_create(&ngx_core_tls_key);  /* 创建线程局部存储(TLS) key：各线程持独立的上下文 */
        if (err != 0) {  /* key 创建失败 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, err,  /* 告警日志 */
                          ngx_thread_key_create_n " failed");  /* 日志文本（宏拼接出实际函数名） */
            /* fatal */  /* 原有英文注释：致命错误 */
            exit(2);  /* 退出 */
        }  /* TLS key 创建失败处理结束 */

        for (n = 0; n < ngx_threads_n; n++) {  /* 逐个创建工作线程 */

            if (!(ngx_threads[n].cv = ngx_cond_init(cycle->log))) {  /* 为每线程初始化条件变量（任务队列的等待/唤醒原语） */
                /* fatal */  /* 原有英文注释：致命错误 */
                exit(2);  /* 退出 */
            }  /* 条件变量初始化失败处理结束 */

            if (ngx_create_thread((ngx_tid_t *) &ngx_threads[n].tid,  /* 创建线程并回填 tid */
                                  ngx_worker_thread_cycle,  /* 续行：线程入口函数 */
                                  (void *) &ngx_threads[n], cycle->log) != 0)  /* 续行：传线程描述块 */
            {  /* if 块开始 */
                /* fatal */  /* 原有英文注释：致命错误 */
                exit(2);  /* 退出 */
            }  /* 线程创建失败处理结束 */
        }  /* 创建工作线程遍历结束 */
    }  /* 线程池创建 if 块结束 */

#endif  /* 结束 NGX_THREADS 条件编译 */

    for ( ;; ) {  /* worker 事件主循环：进程 95% 以上时间阻塞在 ngx_process_events 内的 epoll_wait 上 */
        if (ngx_exiting  /* 处于优雅退出（排空）状态 */
            && ngx_event_timer_rbtree == &ngx_event_timer_sentinel)  /* 且定时器红黑树为空=所有连接均已处理完毕 */
        {  /* if 条件跨行结束 */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "exiting");  /* 日志：开始退出 */


#if (NGX_THREADS)  /* 线程支持 */
            ngx_terminate = 1;  /* 通知线程池全部退出 */

            ngx_wakeup_worker_threads(cycle);  /* 唤醒并 join 回收所有工作线程 */
#endif  /* 结束条件编译 */

            /*
             * we do not destroy cycle->pool here because a signal handler
             * that uses cycle->log can be called at this point
             */  /* 原有英文注释：不销毁内存池——信号处理函数此时仍可能引用 cycle->log */
            exit(0);  /* 正常退出：master 的 SIGCHLD 随即触发收割逻辑 */
        }  /* 排空完毕退出判断结束 */

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "worker cycle");  /* 调试日志：一轮循环开始 */

        ngx_process_events(cycle);  /* 一轮事件处理：epoll_wait 等待 I/O 就绪+最近定时器到期并分发事件（事件驱动核心） */

        if (ngx_terminate) {  /* 收到快速终止（SIGTERM 或通道 TERMINATE 命令） */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "exiting");  /* 日志 */

#if (NGX_THREADS)  /* 线程支持 */
            ngx_wakeup_worker_threads(cycle);  /* 回收全部工作线程 */
#endif  /* 结束条件编译 */

            /*
             * we do not destroy cycle->pool here because a signal handler
             * that uses cycle->log can be called at this point
             */  /* 原有英文注释：不销毁内存池，避免与信号处理函数竞态 */
            exit(0);  /* 立即退出：不等存量连接处理完（与 quit 的本质区别） */
        }  /* 快速终止处理结束 */

        if (ngx_quit) {  /* 收到优雅退出（SIGQUIT 或通道 QUIT 命令） */
            ngx_quit = 0;  /* 消费标志 */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0,  /* 日志 */
                          "gracefully shutting down");  /* 日志文本续行 */
            ngx_setproctitle("worker process is shutting down");  /* 改标题向运维直观展示"正在优雅退出" */

            if (!ngx_exiting) {  /* 保证初始化动作只执行一次 */
                ngx_close_listening_sockets(cycle);  /* 关闭监听 socket：不再 accept 新连接，存量连接继续服务 */
                ngx_exiting = 1;  /* 进入"排空中"状态：定时器树空即退出 */
            }  /* 优雅退出初始化一次性 if 结束 */
        }  /* 优雅退出处理结束 */

        if (ngx_reopen) {  /* 收到重开日志（SIGUSR1 或通道 REOPEN 命令） */
            ngx_reopen = 0;  /* 消费标志 */
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "reopen logs");  /* 日志 */
            ngx_reopen_files(cycle, -1);  /* 关旧日志 fd、重新 open 新文件：旧文件句柄释放后日志切割才真正生效 */
        }  /* 重开日志处理结束 */
    }  /* worker 主循环体结束 */
}  /* ngx_worker_process_cycle 结束 */


/* 通道可读事件回调：master 与每个 worker 间由 socketpair 构成全双工 unix 域通道。
 * 控制命令走事件循环而非信号处理函数，是 nginx 规避异步信号不可重入问题的关键设计：
 * 命令处理与连接状态机天然串行化，无需额外加锁。 */
static void ngx_channel_handler(ngx_event_t *ev)  /* ev：通道连接上触发的读事件 */
{
    ngx_int_t          n;    /* 实际读取字节数 */
    ngx_channel_t      ch;   /* 收到的命令消息 */
    ngx_connection_t  *c;    /* 通道对应的连接对象 */

    c = ev->data;  /* 从事件取出注册时绑定的连接上下文 */

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, ev->log, 0, "channel handler");  /* 调试日志 */

    n = ngx_read_channel(c->fd, &ch, sizeof(ngx_channel_t), ev->log);  /* 读一条完整命令（底层 recvmsg；若带 SCM_RIGHTS 辅助数据则同时接收 fd） */

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ev->log, 0, "channel: %d", n);  /* 调试日志：读取结果 */

    if (n <= 0) {  /* 数据不足一条完整消息或读错 */
        return;  /* 直接返回：等内核再次就绪触发下一次读事件 */
    }  /* 读取不完整/出错提前返回 if 结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ev->log, 0,  /* 调试日志：命令字 */
                   "channel command: %d", ch.command);  /* 日志文本续行 */

    switch (ch.command) {  /* 按命令字分发处理 */

    case NGX_CMD_QUIT:  /* master 要求优雅退出 */
        ngx_quit = 1;  /* 置位标志：主循环随后关闭监听并等连接排空 */
        break;  /* case 分支结束 */

    case NGX_CMD_TERMINATE:  /* master 要求快速终止 */
        ngx_terminate = 1;  /* 置位标志：主循环下一轮立即 exit */
        break;  /* case 分支结束 */

    case NGX_CMD_REOPEN:  /* master 要求重开日志 */
        ngx_reopen = 1;  /* 置位标志 */
        break;  /* case 分支结束 */

    case NGX_CMD_OPEN_CHANNEL:  /* 有新进程诞生：登记其通道 */

        ngx_log_debug3(NGX_LOG_DEBUG_CORE, ev->log, 0,  /* 调试日志 */
                       "get channel s:%d pid:" PID_T_FMT " fd:%d",  /* 格式串 */
                       ch.slot, ch.pid, ch.fd);  /* 参数：槽位/PID/fd */

        ngx_processes[ch.slot].pid = ch.pid;  /* 在本进程的进程表副本中登记新进程 PID */
        ngx_processes[ch.slot].channel[0] = ch.fd;  /* 登记其通道读端 fd：今后可向该进程发消息 */
        break;  /* case 分支结束 */

    case NGX_CMD_CLOSE_CHANNEL:  /* 某进程死亡：拆除指向它的通道 */

        ngx_log_debug4(NGX_LOG_DEBUG_CORE, ev->log, 0,  /* 调试日志 */
                       "close channel s:%d pid:" PID_T_FMT " our:" PID_T_FMT  /* 格式串跨行 */
                       " fd:%d",  /* 格式串续行 */
                       ch.slot, ch.pid, ngx_processes[ch.slot].pid,  /* 参数：槽位/对方 PID/本地记录的 PID */
                       ngx_processes[ch.slot].channel[0]);  /* 参数续行：本地记录的 fd */

        if (close(ngx_processes[ch.slot].channel[0]) == -1) {  /* 关闭本地持有的对端 fd：socketpair 引用计数减一 */
            ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno, "close() failed");  /* 关闭失败告警 */
        }  /* 关闭死进程通道告警块结束 */

        ngx_processes[ch.slot].channel[0] = -1;  /* 槽位通道复位为无效值 */
        break;  /* case 分支结束 */
    }  /* switch 命令分发结束 */
}  /* ngx_channel_handler 结束 */


#if (NGX_THREADS)  /* 条件编译：线程支持 */

/* 唤醒全部工作线程并逐个 join 回收；worker 退出前调用，确保线程不携带资源消亡。
 * 采用"轮询+让出 CPU"的简单方案：退出是低频路径，正确性优先于效率。 */
static void ngx_wakeup_worker_threads(ngx_cycle_t *cycle)  /* 参数仅用于日志 */
{
    ngx_int_t   i;     /* 线程表循环下标 */
    ngx_uint_t  live;  /* 是否仍有未结束的线程 */

    for ( ;; ) {  /* 反复扫描直至所有线程结束 */

        live = 0;  /* 每轮重置存活标记 */

        for (i = 0; i < ngx_threads_n; i++) {  /* 遍历线程描述表 */
            if (ngx_threads[i].state < NGX_THREAD_EXIT) {  /* 该线程尚未进入退出态 */
                ngx_cond_signal(ngx_threads[i].cv);  /* 条件变量唤醒：催促睡眠中的线程去检查退出标志 */

                if (ngx_threads[i].cv->tid == (ngx_tid_t) -1) {  /* 条件变量上无线程等待（线程已自行脱离） */
                    ngx_threads[i].state = NGX_THREAD_DONE;  /* 直接标记完成 */
                } else {  /* 条件变量上有等待线程的 else */
                    live = 1;  /* 仍有线程活着，需继续轮询 */
                }  /* 有等待线程 else 分支结束 */
            }  /* 未退出线程处理 if 结束 */

            if (ngx_threads[i].state == NGX_THREAD_EXIT) {  /* 该线程已置退出态 */
                ngx_thread_join(ngx_threads[i].tid, NULL);  /* join 回收线程资源（pthread_join 语义），防止僵尸线程 */
                ngx_threads[i].state = NGX_THREAD_DONE;  /* 标记完成，避免重复 join */
            }  /* 已退出线程 join 处理结束 */
        }  /* 线程表遍历结束 */

        if (live == 0) {  /* 全部线程均已结束 */
            ngx_log_debug0(NGX_LOG_DEBUG_CORE, cycle->log, 0,  /* 调试日志 */
                           "all worker threads are joined");  /* 日志文本 */

            /* STUB */  /* 原有英文注释：临时实现 */
            ngx_done_events(cycle);  /* 释放事件模块资源 */
            ngx_mutex_destroy(ngx_event_timer_mutex);  /* 销毁定时器互斥锁 */
            ngx_mutex_destroy(ngx_posted_events_mutex);  /* 销毁 posted 事件队列互斥锁 */

            return;  /* 回收完成 */
        }  /* 全部线程回收完毕 if 结束 */

        ngx_sched_yield();  /* sched_yield 让出 CPU：避免忙等空转，给正在退出的线程执行机会 */
    }  /* 轮询等待线程退出循环结束 */
}  /* ngx_wakeup_worker_threads 结束 */


/* worker 内工作线程主循环：平时睡眠在条件变量上，被唤醒后处理 posted 事件队列。
 * 设计意图：可能阻塞的耗时操作放入线程执行，主事件循环线程不被拖住；
 * 线程间以"互斥锁+条件变量"同步（pthread 语义）。 */
static void* ngx_worker_thread_cycle(void *data)  /* data：创建线程时传入的线程描述块 */
{
    ngx_thread_t  *thr = data;  /* 取回本线程的描述块 */

    sigset_t          set;   /* 线程信号掩码 */
    ngx_err_t         err;   /* 错误码暂存 */
    ngx_core_tls_t   *tls;   /* 线程局部存储上下文 */
    ngx_cycle_t      *cycle; /* 全局 cycle 的本地缓存 */
    struct timeval    tv;    /* 时间结构（本函数中预留） */

    thr->cv->tid = ngx_thread_self();  /* 在条件变量上登记本线程 tid：唤醒方据此判断是否有人在等待 */

    cycle = (ngx_cycle_t *) ngx_cycle;  /* 缓存全局 cycle 指针供日志使用 */

    sigemptyset(&set);  /* 构造信号集 */
    sigaddset(&set, ngx_signal_value(NGX_RECONFIGURE_SIGNAL));  /* 加入重配置信号 */
    sigaddset(&set, ngx_signal_value(NGX_REOPEN_SIGNAL));       /* 加入重开日志信号 */
    sigaddset(&set, ngx_signal_value(NGX_CHANGEBIN_SIGNAL));    /* 加入平滑升级信号 */

    err = ngx_thread_sigmask(SIG_BLOCK, &set, NULL);  /* 线程级阻塞管理信号：此类信号只由进程主线程消费，避免多线程竞争 */
    if (err) {  /* 掩码设置失败 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, err,  /* 告警日志 */
                      ngx_thread_sigmask_n " failed");  /* 日志文本（宏拼接函数名） */
        return (void *) 1;  /* 以非零状态结束线程 */
    }  /* 线程信号掩码失败处理结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,  /* 调试日志 */
                   "thread " TID_T_FMT " started", ngx_thread_self());  /* 打印线程启动与其 tid */

    ngx_setthrtitle("worker thread");  /* 设置线程名（支持的平台生效），便于 top/ps 观察 */

    if (!(tls = ngx_calloc(sizeof(ngx_core_tls_t), cycle->log))) {  /* 分配线程专属上下文（零初始化） */
        return (void *) 1;  /* 分配失败，结束线程 */
    }  /* TLS 上下文分配失败处理结束 */

    err = ngx_thread_set_tls(ngx_core_tls_key, tls);  /* 把上下文挂到 TLS 槽位：模块可按线程取回自己的数据 */
    if (err != 0) {  /* TLS 设置失败 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, err,  /* 告警日志 */
                      ngx_thread_set_tls_n " failed");  /* 日志文本（宏拼接函数名） */
        return (void *) 1;  /* 结束线程 */
    }  /* TLS 设置失败处理结束 */

    if (ngx_mutex_lock(ngx_posted_events_mutex) == NGX_ERROR) {  /* 锁住 posted 队列互斥锁：与条件变量配对使用 */
        return (void *) 1;  /* 加锁失败，结束线程 */
    }  /* 加锁失败处理结束 */

    for ( ;; ) {  /* 线程主循环 */
        thr->state = NGX_THREAD_FREE;  /* 标记空闲：可接受任务分配 */

        if (ngx_cond_wait(thr->cv, ngx_posted_events_mutex) == NGX_ERROR) {  /* 原子"解锁+睡眠"等唤醒（pthread_cond_wait 语义），醒来时重新持锁 */
            return (void *) 1;  /* 等待出错，结束线程 */
        }  /* 条件变量等待失败处理结束 */

        if (ngx_terminate) {  /* 被唤醒且全局要求终止 */
            thr->state = NGX_THREAD_EXIT;  /* 置退出态：供主线程扫描后 join */

            ngx_mutex_unlock(ngx_posted_events_mutex);  /* 释放互斥锁后再退出，避免死锁 */

            ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,  /* 调试日志 */
                           "thread %d is done", ngx_thread_self());  /* 打印线程结束 */

            return (void *) 0;  /* 正常结束线程 */
        }  /* 线程终止退出分支结束 */

        thr->state = NGX_THREAD_BUSY;  /* 标记忙碌：开始处理任务 */

        if (ngx_event_thread_process_posted(cycle) == NGX_ERROR) {  /* 取并处理 posted 事件队列中的事件 */
            return (void *) 1;  /* 出错则结束线程 */
        }  /* posted 事件处理失败判断结束 */

        if (ngx_event_thread_process_posted(cycle) == NGX_ERROR) {  /* 再处理一轮（原代码重复调用，保持原样） */
            return (void *) 1;  /* 出错则结束线程 */
        }  /* posted 事件二次处理失败判断结束 */

        if (ngx_process_changes) {  /* 事件模块注册了"提交变更"钩子（如 kqueue 的 change list 机制） */
            if (ngx_process_changes(cycle, 1) == NGX_ERROR) {  /* 把本次处理中的事件注册变更提交给内核 */
                return (void *) 1;  /* 出错则结束线程 */
            }  /* 提交变更失败判断结束 */
        }  /* 提交事件变更 if 结束 */
    }  /* 线程循环体结束 */
}  /* ngx_worker_thread_cycle 结束 */

#endif  /* 结束 NGX_THREADS 条件编译 */
