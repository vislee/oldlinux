
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>       /* 编译期配置头：autoconfigure 探测出的平台宏(HAVE_* 等)与系统头在此统一引入 */
#include <ngx_core.h>         /* 核心头：ngx_int_t/ngx_pid_t 等基础类型、内存池与日志 API 的声明 */
#include <ngx_event.h>        /* 事件框架头：accept 互斥锁指针 ngx_accept_mutex_ptr 与事件注册接口在此声明 */
#include <ngx_channel.h>      /* 进程通道头：ngx_channel_s 命令消息格式与通道读写函数的声明 */


/* 前置声明：execve 包装函数，它将作为"子进程体"回调传给 ngx_spawn_process，由 fork 出的子进程执行 */
static void ngx_execute_proc(ngx_cycle_t *cycle, void *data);

/* 本进程在进程表中的槽位下标；fork 时随写时复制(COW)页继承给子进程，worker 借此知道"我是谁" */
ngx_int_t      ngx_process_slot;
/* 本进程侧的通道描述符：master 经它向 worker 写命令，worker 的事件循环监听它读命令 */
ngx_socket_t   ngx_channel;
/* 进程表有效上界：下标 0..ngx_last_process-1 的槽位为在用或待收割的子进程记录 */
ngx_int_t      ngx_last_process;
/* master 维护的进程表：每槽位登记一个子进程的 pid、通道两端、入口回调、respawn 属性与退出状态 */
ngx_process_t  ngx_processes[NGX_MAX_PROCESSES];


/*
 * ngx_spawn_process：master 派生子进程的核心函数。
 * 主体是 fork()：Linux 采用写时复制(COW)，子进程复制的是父进程的页表而非物理页，
 * 父子短暂共享同一批页面，任一方首次写入时才触发缺页异常真正复制，故 fork 成本极低；
 * fork 前先为该子进程建好 socketpair 全双工通道，子进程靠 fork 继承 fd 而天然获得通信端点。
 * respawn >= 0 表示在原槽位重拉(崩溃恢复)，NGX_PROCESS_DETACHED 表示脱离 master 管理。
 */
ngx_pid_t ngx_spawn_process(ngx_cycle_t *cycle,   /* 运行周期：连接表、事件表、日志等全局资源的根对象 */
                            ngx_spawn_proc_pt proc, void *data,   /* 子进程入口回调及其私有参数，fork 后仅子进程执行它 */
                            char *name, ngx_int_t respawn)   /* 进程显示名(日志用)；respawn 指定槽位复用/新建/脱离模式 */
{
    u_long     on;   /* ioctl(FIOASYNC) 的入参缓冲：非 0 表示对该 fd 开启异步信号(SIGIO)通知 */
    ngx_pid_t  pid;  /* 保存 fork() 返回值：父进程得子 pid、子进程得 0、失败得 -1 */
    ngx_int_t  s;    /* 本子进程在 ngx_processes[] 进程表中的槽位下标 */

    if (respawn >= 0) {   /* 调用者直接指定槽位：用于按原参数重拉某个已退出的进程 */
        s = respawn;      /* 槽位下标即 respawn 值，原槽位的元数据可全部复用 */

    } else {   /* 新建模式：需要扫描进程表找空槽 */
        for (s = 0; s < ngx_last_process; s++) {   /* 只扫已用区间，避免遍历整张表 */
            if (ngx_processes[s].pid == -1) {   /* pid==-1 表示该槽位的进程已被收割回收，可复用 */
                break;   /* 找到空槽，提前跳出 */
            }   /* 命中空槽的 if 结束 */
        }   /* 空槽扫描 for 结束 */

        if (s == NGX_MAX_PROCESSES) {   /* 循环自然跑完仍无空槽：进程表已满 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,   /* 记 ALERT 级告警日志 */
                          "no more than %d processes can be spawned",   /* 提示系统上限 */
                          NGX_MAX_PROCESSES);   /* 上限常量值 */
            return NGX_ERROR;   /* 无法派生，返回失败 */
        }   /* 进程表满告警 if 结束 */
    }   /* 槽位选择 if/else 结束 */


    if (respawn != NGX_PROCESS_DETACHED) {   /* detached 进程(如 exec 的外部程序)不建通道；普通子进程先搭好 IPC 通道再 fork */

        /* Solaris 9 still has no AF_LOCAL */
        /* 保留原注释：Solaris 9 缺 AF_LOCAL 宏，故下面使用 POSIX 标准名 AF_UNIX */

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, ngx_processes[s].channel) == -1)   /* 创建全双工 Unix 域字节流 socket 对：channel[0]/[1] 互为对端，fork 后子进程继承另一端，即得 master<->worker 双向命令通道 */
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 告警并记录内核错误码 ngx_errno */
                          "socketpair() failed while spawning \"%s\"", name);   /* 带进程名便于定位 */
            return NGX_ERROR;   /* 通道建不起来就无法控制子进程，整体失败 */
        }   /* socketpair 失败 if 结束 */

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, cycle->log, 0,   /* CORE 级 debug 日志：输出通道两端 fd */
                       "channel %d:%d",   /* 两个 %d 对应 channel[0]/channel[1] */
                       ngx_processes[s].channel[0],   /* master 保留的一端 */
                       ngx_processes[s].channel[1]);   /* 子进程(worker) fork 时继承的一端 */

        if (ngx_nonblocking(ngx_processes[s].channel[0]) == -1) {   /* 置非阻塞：事件驱动架构严禁 read/write 阻塞挂起整个 master 进程 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 失败告警并带错误码 */
                          ngx_nonblocking_n " failed while spawning \"%s\"",   /* ngx_nonblocking_n 是"置非阻塞"调用的可移植符号名 */
                          name);   /* 进程名 */
            ngx_close_channel(ngx_processes[s].channel, cycle->log);   /* 回滚：关闭两端防止 fd 泄漏 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* 置非阻塞 ch[0] 失败 if 结束 */

        if (ngx_nonblocking(ngx_processes[s].channel[1]) == -1) {   /* 另一端同样置非阻塞，worker 事件循环读通道时也永不阻塞 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 失败告警并带错误码 */
                          ngx_nonblocking_n " failed while spawning \"%s\"",   /* 同上 */
                          name);   /* 进程名 */
            ngx_close_channel(ngx_processes[s].channel, cycle->log);   /* 关闭两端回收 fd */
            return NGX_ERROR;   /* 返回失败 */
        }   /* 置非阻塞 ch[1] 失败 if 结束 */

        on = 1;   /* 准备打开异步 I/O 通知开关 */
        if (ioctl(ngx_processes[s].channel[0], FIOASYNC, &on) == -1) {   /* FIOASYNC：socket 就绪(如有新命令)时内核向该 fd 属主投递 SIGIO 信号，master 无需轮询即可感知 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 失败告警并带错误码 */
                          "ioctl(FIOASYNC) failed while spawning \"%s\"", name);   /* 指明失败的 ioctl 命令 */
            ngx_close_channel(ngx_processes[s].channel, cycle->log);   /* 清理通道两端 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* ioctl(FIOASYNC) 失败 if 结束 */

        if (fcntl(ngx_processes[s].channel[0], F_SETOWN, ngx_pid) == -1) {   /* F_SETOWN：把 SIGIO 的投递属主设为 master 自己的 pid；不指定则信号无处投递 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 失败告警并带错误码 */
                          "fcntl(F_SETOWN) failed while spawning \"%s\"", name);   /* 指明 fcntl 命令 */
            ngx_close_channel(ngx_processes[s].channel, cycle->log);   /* 清理通道两端 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* fcntl(F_SETOWN) 失败 if 结束 */

        if (fcntl(ngx_processes[s].channel[0], F_SETFD, FD_CLOEXEC) == -1) {   /* 置 close-on-exec：此后 execve 外部程序时内核自动关闭该 fd，防止句柄泄漏进无关进程 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 失败告警并带错误码 */
                          "fcntl(FD_CLOEXEC) failed while spawning \"%s\"",   /* 指明 fcntl 命令 */
                           name);   /* 进程名 */
            ngx_close_channel(ngx_processes[s].channel, cycle->log);   /* 清理通道两端 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* FD_CLOEXEC ch[0] 失败 if 结束 */

        if (fcntl(ngx_processes[s].channel[1], F_SETFD, FD_CLOEXEC) == -1) {   /* 通道另一端同样加 FD_CLOEXEC，两端都不泄漏给 exec 后的程序 */
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 失败告警并带错误码 */
                          "fcntl(FD_CLOEXEC) failed while spawning \"%s\"",   /* 指明 fcntl 命令 */
                           name);   /* 进程名 */
            ngx_close_channel(ngx_processes[s].channel, cycle->log);   /* 清理通道两端 */
            return NGX_ERROR;   /* 返回失败 */
        }   /* FD_CLOEXEC ch[1] 失败 if 结束 */

        ngx_channel = ngx_processes[s].channel[1];   /* 把本槽位通道记入全局 ngx_channel，供本进程后续收发通道命令使用 */

    } else {   /* DETACHED 路径：外部 exec 程序不参与通道通信 */
        ngx_processes[s].channel[0] = -1;   /* 以 -1 作"无通道"哨兵值，后续代码据此跳过通道操作 */
        ngx_processes[s].channel[1] = -1;   /* 另一端同样置 -1 */
    }   /* 通道建立 if/else 结束 */

    ngx_process_slot = s;   /* 槽位号写入全局变量：COW 特性让 fork 出的子进程也看到它，从而得知自己的登记位置 */


    pid = fork();   /* 分叉！子进程获得父进程地址空间的 COW 副本，此后父子从同一返回点各奔前程 */

    switch (pid) {   /* 依 fork 返回值区分三种角色：-1 失败 / 0 子进程 / >0 父进程 */

    case -1:   /* fork 失败分支：进程数超上限、内存不足等 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 告警并带内核错误码 */
                      "fork() failed while spawning \"%s\"", name);   /* 带进程名定位 */
        ngx_close_channel(ngx_processes[s].channel, cycle->log);   /* 子进程未诞生，两端 fd 全由 master 负责回收 */
        return NGX_ERROR;   /* 返回失败 */

    case 0:   /* 返回值为 0：当前位于新生的子进程中 */
        ngx_pid = ngx_getpid();   /* 重新读取自己的真实 pid 覆盖继承值（写 COW 页，只改子进程私有副本） */
        proc(cycle, data);   /* 进入子进程主体(worker 事件循环或 execve)，正常情况下不再返回 */
        break;   /* 若 proc 返回则走出 switch（此路径不应发生） */

    default:   /* 返回值 > 0：当前位于父进程(master)，pid 即子进程 pid */
        break;   /* 父进程不执行子进程体，继续往下登记 */
    }   /* fork 结果 switch 结束 */

    ngx_log_debug2(NGX_LOG_DEBUG_CORE, cycle->log, 0,   /* debug 打印派生结果 */
                   "spawn %s: " PID_T_FMT, name, pid);   /* 进程名与 pid（子进程视角下 pid 为 0） */

    ngx_processes[s].pid = pid;   /* 进程表登记该子进程 pid（父进程视角的记账） */
    ngx_processes[s].exited = 0;   /* 清除"已退出"标志，槽位进入活动状态 */

    if (respawn >= 0) {   /* 原槽位重拉场景：入口/参数/名字/属性早已在表中 */
        return pid;   /* 只需补上 pid 即可返回，避免覆盖旧元数据 */
    }   /* 重拉短路 if 结束 */

    ngx_processes[s].proc = proc;   /* 首次登记：保存入口回调，后续自动重拉时无需调用者再传 */
    ngx_processes[s].data = data;   /* 保存回调的私有参数 */
    ngx_processes[s].name = name;   /* 保存进程名，日志与告警显示用 */
    ngx_processes[s].exiting = 0;   /* 清"正在退出"标志（master 经通道通知其退出时置位） */

    switch (respawn) {   /* 按派生模式写入三个行为属性 */

    case NGX_PROCESS_RESPAWN:   /* 常驻可重生模式：该进程异常退出后 master 自动重拉 */
        ngx_processes[s].respawn = 1;   /* 打开自动重生 */
        ngx_processes[s].just_respawn = 0;   /* 清一次性重生标记 */
        ngx_processes[s].detached = 0;   /* 非 detached */
        break;   /* 分支结束 */

    case NGX_PROCESS_JUST_RESPAWN:   /* 一次性重生模式：仅本轮操作(如 reload 平滑替换)重生一次 */
        ngx_processes[s].respawn = 1;   /* 打开自动重生 */
        ngx_processes[s].just_respawn = 1;   /* 标记仅重生这一次 */
        ngx_processes[s].detached = 0;   /* 非 detached */
        break;   /* 分支结束 */

    case NGX_PROCESS_DETACHED:   /* 脱离管理模式：exec 出去的二进制，死了不补 */
        ngx_processes[s].respawn = 0;   /* 不自动重生 */
        ngx_processes[s].just_respawn = 0;   /* 无一次性标记 */
        ngx_processes[s].detached = 1;   /* detached：无通道，退出仅由 waitpid 收割 */
        break;   /* 分支结束 */
    }   /* respawn 属性 switch 结束 */

    if (s == ngx_last_process) {   /* 若本次占用的是全新的最高槽位 */
        ngx_last_process++;   /* 进程表有效长度增一 */
    }   /* 表长维护 if 结束 */

    return pid;   /* 向调用者返回子进程 pid（子进程通常已进入 proc 内部运行） */
}   /* ngx_spawn_process 结束 */


/*
 * ngx_execute：fork + execve 一个外部程序（0.1.0 的二进制平滑升级就靠它）。
 * 复用 ngx_spawn_process 的 fork 框架，仅把子进程体换成 ngx_execute_proc，
 * 并强制 NGX_PROCESS_DETACHED：不建通道、不自动重生。
 */
ngx_pid_t ngx_execute(ngx_cycle_t *cycle, ngx_exec_ctx_t *ctx)   /* cycle：周期对象；ctx：描述目标程序的上下文 */
{
    return ngx_spawn_process(cycle, ngx_execute_proc, ctx, ctx->name,   /* 子进程体为 execve 包装；进程名取 ctx->name */
                             NGX_PROCESS_DETACHED);   /* detached：exec 后脱离 master 的管理与重生机制 */
}   /* ngx_execute 结束 */


/*
 * ngx_execute_proc：子进程体——把 fork 出的子进程映像整体替换成目标程序。
 * Unix 经典 fork+exec 组合：fork 负责复制进程，execve 负责装载新程序映像；
 * execve 成功时原代码(含本函数)被丢弃、不再返回，仅在失败时才有机会记录日志。
 */
static void ngx_execute_proc(ngx_cycle_t *cycle, void *data)   /* data 即 ngx_exec_ctx_t 上下文 */
{
    ngx_exec_ctx_t  *ctx = data;   /* 还原 exec 上下文：path/argv/envp/name */

    if (execve(ctx->path, ctx->argv, ctx->envp) == -1) {   /* execve：丢弃当前映像、装载 path 程序并以 argv/envp 运行；-1 表示失败 */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,   /* 失败告警并带错误码 */
                      "execve() failed while executing %s \"%s\"",   /* 程序名与路径 */
                      ctx->name, ctx->path);   /* 两个参数 */
    }   /* execve 失败 if 结束 */

    exit(1);   /* exec 失败必须立即终止子进程，防止它带着 master 的完整内存状态误入 worker 逻辑 */
}   /* ngx_execute_proc 结束 */


/*
 * ngx_process_get_status：master 收割已退出的子进程（SIGCHLD 信号处理流程中调用）。
 * 理论要点：子进程终止后内核保留其退出状态形成僵尸进程(zombie)，只有父进程
 * wait/waitpid 才能回收其进程表项；信号不排队，一次 SIGCHLD 可能对应多个子进程
 * 同时退出，故用 WNOHANG 非阻塞循环把"死者"一次收干且不阻塞 master。
 */
void ngx_process_get_status()   /* 无参数：通过全局 ngx_processes 进程表工作 */
{
    int              status;   /* waitpid 回填的退出状态字，需用 WEXITSTATUS/WTERMSIG 等宏解码 */
    char            *process;   /* 退出者名字，用于日志显示 */
    ngx_pid_t        pid;   /* 被收割子进程的 pid */
    ngx_err_t        err;   /* 暂存 errno，避免被后续日志调用覆盖 */
    ngx_int_t        i;   /* 进程表扫描下标 */
    ngx_uint_t       one;   /* "本轮已成功收割过"标志，用于区分真假 ECHILD */
    struct timeval   tv;   /* 时间值声明（保留原样；0.1.0 中未被后续代码使用） */
    one = 0;   /* 初始：本轮尚未收割任何进程 */

    for ( ;; ) {   /* 无限循环：直到没有可收割的子进程 */
        pid = waitpid(-1, &status, WNOHANG);   /* -1 等待任意子进程；WNOHANG 非阻塞：无退出者立即返回 0 而非挂起 master */

        if (pid == 0) {   /* 返回 0：子进程都还活着，本轮收割完毕 */
            return;   /* 收割完毕，正常返回 */
        }   /* pid==0 if 结束 */

        if (pid == -1) {   /* 返回 -1：出错（无子进程可收或被信号打断） */
            err = ngx_errno;   /* 先取 errno 再分支，防止中间调用改变它 */

            if (err == NGX_EINTR) {   /* EINTR：waitpid 这个慢系统调用被信号打断（经典 EINTR 语义），重试即可 */
                continue;   /* 重试 waitpid */
            }   /* EINTR if 结束 */

            if (err == NGX_ECHILD && one) {   /* ECHILD=已无可收子进程；若本轮确实收过则视为正常收尾 */
                return;   /* 正常收尾 */
            }   /* ECHILD if 结束 */

            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, errno,   /* 非预期错误：ALERT 告警（此处直接传全局 errno） */
                          "waitpid() failed");   /* 指明失败的系统调用 */
            return;   /* 放弃本轮收割 */
        }   /* pid==-1 if 结束 */


        if (ngx_accept_mutex_ptr) {   /* 共享内存中的 accept 互斥锁已初始化（多 worker 争抢 accept 的架构） */

            /*
             * unlock the accept mutex if the abnormally exited process
             * held it
             */

            /* 保留原注释语义：若异常退出者生前持有 accept 锁，须替它解锁 */
            ngx_atomic_cmp_set(ngx_accept_mutex_ptr, pid, 0);   /* 原子 CAS：锁变量的值==死亡者 pid 才清 0——worker 持锁崩溃会把锁"带进坟墓"造成死锁；由于锁在共享内存中，任何 worker 都能做此修复 */
        }   /* accept 锁修复 if 结束 */


        one = 1;   /* 标记本轮已成功收割至少一个进程 */
        process = "unknown process";   /* 默认名字：pid 不在进程表时（如 detached 程序）的占位串 */

        for (i = 0; i < ngx_last_process; i++) {   /* 线性扫描进程表定位该 pid 的槽位 */
            if (ngx_processes[i].pid == pid) {   /* pid 匹配即命中 */
                ngx_processes[i].status = status;   /* 保存原始退出状态字供后续判定 */
                ngx_processes[i].exited = 1;   /* 置已退出标志：master 主循环据此重拉或清理槽位 */
                process = ngx_processes[i].name;   /* 取真实进程名用于日志 */
                break;   /* 命中即止 */
            }   /* pid 命中 if 结束 */
        }   /* 进程表扫描 for 结束 */

        if (WTERMSIG(status)) {   /* 宏解码：非 0 表示子进程是被信号杀死的（异常死亡） */
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,   /* ALERT 级：worker 被信号杀死是严重事件 */
                          "%s " PID_T_FMT " exited on signal %d%s",   /* 输出致命信号编号 */
                          process, pid, WTERMSIG(status),   /* 名字/pid/信号值 */
                          WCOREDUMP(status) ? " (core dumped)" : "");   /* 附加是否生成了 core 转储 */

        } else {   /* 走到 else：子进程是通过 exit() 正常退出的 */
            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,   /* INFO 级：正常退出仅记录流水 */
                          "%s " PID_T_FMT " exited with code %d",   /* 输出退出码 */
                          process, pid, WEXITSTATUS(status));   /* 宏从状态字解出 exit(code) 的 code */
        }   /* 死因判定 if/else 结束 */

        if (WEXITSTATUS(status) == 2 && ngx_processes[i].respawn) {   /* 约定退出码 2 为致命错误：即使可重生也不许再拉，防止无限崩溃-重生循环 */
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,   /* 告警说明放弃重生 */
                          "%s " PID_T_FMT   /* 消息前半段（字符串常量拼接） */
                          " exited with fatal code %d and could not respawn",   /* 消息正文（跨两行字符串拼接） */
                          process, pid, WEXITSTATUS(status));   /* 名字/pid/退出码 */
            ngx_processes[i].respawn = 0;   /* 关闭重生标志：该槽位将被主循环回收不再重拉 */
        }   /* 致命退出码 if 结束 */
    }   /* 收割循环 for(;;) 结束 */
}   /* ngx_process_get_status 结束 */
