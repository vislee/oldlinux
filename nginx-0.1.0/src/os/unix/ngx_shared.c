
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>   /* 编译期配置头：HAVE_MAP_ANON/HAVE_MAP_DEVZERO/HAVE_SYSVSHM 平台探测宏在此定义 */
#include <ngx_core.h>   /* 核心头：size_t 封装、日志 API 等基础声明 */


/*
 * 本文件提供 ngx_create_shared_memory 的三套平台实现（编译期三选一）。
 * 理论要点：fork 出的子进程默认继承父进程地址空间的"私有"副本（写时复制），
 * 各 worker 的普通变量互不可见；而 accept 互斥锁(ngx_accept_mutex_ptr)要求
 * 所有 worker 争抢"同一个"原子变量，因此必须把它放进多进程共享的同一批
 * 物理页——即共享内存。MAP_SHARED 语义保证 fork 前后父子映射的是同一物理页。
 */
#if (HAVE_MAP_ANON)   /* 首选实现：匿名共享映射（BSD/Linux 风格），无需任何文件支撑 */

void *ngx_create_shared_memory(size_t size, ngx_log_t *log)   /* size：共享区字节数；log：日志对象 */
{
    void  *p;   /* 映射成功的起始虚拟地址 */

    p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);   /* MAP_ANON：匿名映射不关联文件；MAP_SHARED：对它的写入对 fork 出的所有 worker 可见（共享同一物理页）；fd 传 -1 占位 */

    if (p == MAP_FAILED) {   /* mmap 失败：返回 MAP_FAILED 而非 NULL */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,   /* ALERT 告警并带错误码 */
                      "mmap(MAP_ANON|MAP_SHARED, " SIZE_T_FMT ") failed",   /* 指明失败的映射方式与大小 */
                      size);   /* 申请的字节数 */
        return NULL;   /* 失败返回空指针 */
    }   /* mmap 失败 if 结束 */

    return p;   /* 返回可供全进程组共享的地址 */
}   /* MAP_ANON 版本结束 */

#elif (HAVE_MAP_DEVZERO)   /* 次选实现：以 /dev/zero 设备文件为后备的共享映射（老 BSD/Solaris 无 MAP_ANON） */

void *ngx_create_shared_memory(size_t size, ngx_log_t *log)   /* 参数同上 */
{
    void      *p;   /* 映射起始地址 */
    ngx_fd_t   fd;   /* /dev/zero 的描述符，作 mmap 的文件后备 */

    fd = open("/dev/zero", O_RDWR);   /* 打开零设备：读之得无限 0，常用来提供匿名字节后备 */

    if (fd == -1) {   /* 打开失败 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,   /* ALERT 告警并带错误码 */
                      "open(/dev/zero) failed");   /* 指明失败的调用 */
        return NULL;   /* 返回空指针 */
    }   /* open 失败 if 结束 */

    p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);   /* MAP_SHARED 映射 /dev/zero 的前 size 字节：各进程写入落在同一物理页上，实现共享 */

    if (p == MAP_FAILED) {   /* 映射失败 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,   /* ALERT 告警并带错误码 */
                      "mmap(/dev/zero, MAP_SHARED, " SIZE_T_FMT ") failed",   /* 指明映射参数 */
                      size);   /* 申请大小 */
        p = NULL;   /* 统一以 NULL 表示失败 */
    }   /* mmap 失败 if 结束 */

    if (close(fd) == -1) {   /* 映射建立后 fd 即无用（映射不随 fd 关闭而解除），关闭防泄漏 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno, "close() failed");   /* 关闭失败告警 */
    }   /* close if 结束 */

    return p;   /* 返回共享区地址（失败时为 NULL） */
}   /* MAP_DEVZERO 版本结束 */

#elif (HAVE_SYSVSHM)   /* 兜底实现：System V 共享内存接口 shmget/shmat */

#include <sys/ipc.h>   /* System V IPC 公共头：IPC_PRIVATE/IPC_RMID 等键值与命令定义 */
#include <sys/shm.h>   /* shmget/shmat/shmctl 原型与 SHM_R/SHM_W 权限宏 */


void *ngx_create_shared_memory(size_t size, ngx_log_t *log)   /* 参数同上 */
{
    int    id;   /* System V 共享内存段标识符 shmid */
    void  *p;   /* 段附加(attach)后的本进程虚拟地址 */

    id = shmget(IPC_PRIVATE, size, (SHM_R|SHM_W|IPC_CREAT));   /* IPC_PRIVATE：不进入公共键空间、仅凭 shmid 在父子进程间共享；新建读写段 */

    if (id == -1) {   /* 建段失败 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,   /* ALERT 告警并带错误码 */
                      "shmget(" SIZE_T_FMT ") failed", size);   /* 带申请大小 */
        return NULL;   /* 返回空指针 */
    }   /* shmget 失败 if 结束 */

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, log, 0, "shmget id: %d", id);   /* debug 打印 shmid：该段不靠文件系统，排查需凭此 id 用 ipcs 查看 */

    p = shmat(id, NULL, 0);   /* 把段附加到本进程地址空间（NULL 交内核选地址，0 为默认读写） */

    if (p == (void *) -1) {   /* 附加失败时返回 (void*)-1 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno, "shmat() failed");   /* ALERT 告警 */
        p = NULL;   /* 统一失败表示 */
    }   /* shmat 失败 if 结束 */

    if (shmctl(id, IPC_RMID, NULL) == -1) {   /* 立刻给段打"删除"标记：此后任何新进程无法再附加，现有附加者全部脱接后内核即回收——经典的"孤儿段"防泄漏手法 */
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno, "shmctl(IPC_RMID) failed");   /* 打标记失败则段可能永久残留，ALERT 告警 */
        p = NULL;   /* 视为失败 */
    }   /* shmctl if 结束 */

    return p;   /* 返回共享区地址；master fork 后各 worker 天然共享该段 */
}   /* SYSVSHM 版本结束 */

#endif   /* 三选一实现分支结束 */
