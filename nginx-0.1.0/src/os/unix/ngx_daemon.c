
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>   /* 编译期配置头：平台探测宏与系统头统一在此引入 */
#include <ngx_core.h>   /* 核心头：ngx_int_t/ngx_pid_t 等类型与日志 API 的声明 */


/*
 * ngx_daemon：把 master 变成守护进程(daemon)。
 * 经典守护进程化三步的 0.1.0 实现：
 * 1) fork 后父进程立即退出——shell 认为命令已结束返回提示符，子进程变成孤儿被
 *    init(1 号进程)收养，且不再是进程组组长（这是能成功 setsid 的前提）；
 * 2) setsid() 建立新会话并脱离原控制终端——终端挂断不再向进程投递 SIGHUP；
 * 3) umask(0) 清除文件权限掩码，并把 stdin/stdout 重定向到 /dev/null，
 *    防止任何代码读写终端造成阻塞（注意：本版本刻意保留了 stderr，
 *    因为此时错误日志仍走继承来的 stderr 描述符；也没做 chdir("/")，
 *    以便配置中的相对路径仍相对启动目录解析）。
 */
int ngx_daemon(ngx_log_t *log)   /* log：此时仍指向继承来的(通常是 stderr)错误日志 */
{
    int  fd;   /* /dev/null 的文件描述符，稍后 dup2 到标准输入输出上 */

    switch (fork()) {   /* fork：子进程得到父进程的 COW 副本，按返回值三分支处理 */

    case -1:   /* fork 失败：系统进程数超限或资源不足 */
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "fork() failed");   /* EMERG 级记录失败原因，daemon 化无法继续 */
        return NGX_ERROR;   /* 向调用者(启动流程)报错，nginx 直接退出 */

    case 0:   /* 返回 0：位于子进程，继续执行守护进程化后续步骤 */
        break;   /* 直接走出 switch */

    default:   /* 返回值 >0：位于父进程，pid 为子进程号 */
        exit(0);   /* 父进程立即退出：用户 shell 恢复提示符；子进程变孤儿被 init 收养，并脱离"组长"身份为 setsid 铺路 */
    }   /* fork 结果 switch 结束 */

    ngx_pid = ngx_getpid();   /* 子进程重新取真实 pid 覆盖继承值（写入触发 COW，仅改子进程私有页） */

    if (setsid() == -1) {   /* setsid：创建新会话使进程脱离原控制终端；若调用者本已是组长则会失败(EPERM)——这正是先让父进程退出的原因 */
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "setsid() failed");   /* 失败则 EMERG 退出，绝不带着控制终端运行 */
        return NGX_ERROR;   /* 返回错误 */
    }   /* setsid 失败 if 结束 */

    umask(0);   /* 清除进程的文件模式创建掩码：之后创建日志/pid 文件时权限完全由 open 的 mode 参数决定，不受继承掩码干扰 */

    fd = open("/dev/null", O_RDWR);   /* 打开黑洞设备 /dev/null：可读(立即 EOF)可写(数据被丢弃)，用作新的标准输入输出 */
    if (fd == -1) {   /* 打开失败（极端情况下 /dev/null 缺失） */
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,   /* EMERG 记录错误码 */
                      "open(\"/dev/null\") failed");   /* 指明失败的调用 */
        return NGX_ERROR;   /* 返回错误 */
    }   /* open 失败 if 结束 */

    if (dup2(fd, STDIN_FILENO) == -1) {   /* 把 /dev/null 复制到 fd 0：此后任何读 stdin 的代码立即得到 EOF，而不会挂起等待不存在的终端输入 */
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "dup2(STDIN) failed");   /* 失败记录并退出 */
        return NGX_ERROR;   /* 返回错误 */
    }   /* stdin 重定向 if 结束 */

    if (dup2(fd, STDOUT_FILENO) == -1) {   /* 把 /dev/null 复制到 fd 1：stdout 的输出全部丢弃，避免向已脱离的终端写数据时阻塞或收到 SIGHUP */
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "dup2(STDOUT) failed");   /* 失败记录并退出 */
        return NGX_ERROR;   /* 返回错误 */
    }   /* stdout 重定向 if 结束 */

#if 0   /* 编译开关置 0：本版本刻意不启用 stderr 的重定向 */
    /* 被禁用的分支：0.1.0 刻意不重定向 stderr——错误日志此刻仍写在继承的 stderr 上，关掉它将丢失启动期日志 */
    if (dup2(fd, STDERR_FILENO) == -1) {   /* （未编译）原计划把 stderr 也指向 /dev/null */
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "dup2(STDERR) failed");   /* （未编译）失败记录 */
        return NGX_ERROR;   /* （未编译）返回错误 */
    }   /* （未编译）stderr 重定向 if 结束 */
#endif   /* 被禁用的 stderr 重定向块结束 */

    if (fd > STDERR_FILENO) {   /* 若 /dev/null 的 fd 大于 2，说明 0/1 之外还占着一个描述符 */
        if (close(fd) == -1) {   /* 原始 fd 已被 dup2 复制到 0/1，可以关闭回收，避免描述符泄漏 */
            ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "close() failed");   /* 失败记录（几乎不可能发生） */
            return NGX_ERROR;   /* 返回错误 */
        }   /* close 失败 if 结束 */
    }   /* fd 回收 if 结束 */

    return NGX_OK;   /* 守护进程化完成，调用者将以全新会话首进程的身份继续 master 初始化 */
}   /* ngx_daemon 结束 */
