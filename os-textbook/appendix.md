# 附录 术语表 · C 语言速查 · 源码地图

> 本附录是工具章，不需要从头读到尾。读正文卡在某个词时翻附录 A，
> 看不懂某段内核代码时翻附录 B，想找某个文件在哪时翻附录 C。

---

## 附录 A 术语表

74 个术语按字母/拼音排序，第二版新增的 CPL/DPL、CFS、vruntime、futex 等均已入册。"详见"指向正文章节，想看代码实现请配合附录 C 的目录地图。

### 以字母和数字开头的术语

| 术语 | 英文全称 | 一句话解释 | 详见 |
|---|---|---|---|
| 8259A | Programmable Interrupt Controller | 可编程中断控制器：把时钟、键盘等外设的 IRQ 汇接成一条中断线送给 CPU，0.11 把它重映射到中断号 0x20~0x2F | [ch02](ch02-计算机硬件基础.md) |
| CAS | Compare-And-Swap | CPU 提供的原子指令：变量等于预期旧值才写入新值，是无锁编程的基石 | [ch12](ch12-并发与同步.md) |
| CFS | Completely Fair Scheduler | 完全公平调度器（2.6.23 起）：就绪进程按 vruntime 排进红黑树，谁最"亏"谁先跑；nice 不改时间片、只改记账速度 | [ch06](ch06-CPU调度.md) |
| CPL/DPL | Current/Descriptor Privilege Level | 当前特权级与目标门的门槛：CPL 够不着（数值大于）DPL 就被硬件拦下，`int 0x80` 的门特意把 DPL 开到 3 | [ch05](ch05-中断与系统调用.md) |
| EOI | End Of Interrupt | 中断结束回执：处理完先给 8259A 发 EOI（`outb %al,$0x20`），它才继续转发后续同级中断 | [ch05](ch05-中断与系统调用.md) |
| epoll | epoll | Linux 的高效多路复用接口：一个内核事件对象同时盯成千上万个套接字，谁就绪叫醒谁 | [ch11](ch11-网络编程.md) |
| futex | fast userspace mutex | 快速用户空间互斥体：无竞争时一条 CAS 在用户态解决，有竞争才陷入内核睡眠——现代 pthread 锁与 mutex 的地基 | [ch12](ch12-并发与同步.md) |
| min_vruntime | minimum vruntime | CFS 就绪队列的 vruntime 下界：新进程与睡醒进程都从它附近开始补账，防止一觉醒来反客为主 | [ch06](ch06-CPU调度.md) |
| SIGPIPE | Signal: write on a pipe with no readers | 往无人读取的管道/套接字写入时收到的 13 号信号，默认动作是终止；服务器惯用 SIG_IGN 忽略、改看 write 返回值 | [ch08](ch08-进程间通信.md) |
| TLB | Translation Lookaside Buffer | MMU 里缓存"页号→物理页框号"的高速部件，查页表前先问它；改页表必须冲刷它（0.11 重写 CR3 即全清） | [ch07](ch07-内存管理.md) |
| TIME_WAIT | TIME_WAIT | TCP 主动关闭方进入的等待状态（约 2 个报文最大生存期），保证最后的 ACK 送达、旧报文自然消亡 | [ch11](ch11-网络编程.md) |
| VFS | Virtual File System | 虚拟文件系统：给磁盘文件、管道、设备文件定一套统一的 open/read/write 接口 | [ch09](ch09-文件系统.md) |
| vruntime | virtual runtime | CFS 的记账时钟：`vruntime += 实际运行时间 × NICE_0_LOAD ÷ 权重`，人人涨得一样快即"完全公平" | [ch06](ch06-CPU调度.md) |

### 按拼音排序的术语

| 术语 | 英文 | 一句话解释 | 详见 |
|---|---|---|---|
| 按需调页 | demand paging | 页面缺席了才真正调入：0.11 的 `exec.c` 借缺页处理把程序"用一页装一页" | [ch07](ch07-内存管理.md) |
| 半连接队列 | SYN queue | 服务端收到 SYN、握手未完成时暂存连接的队列，长度过小会被 SYN 洪水打爆 | [ch11](ch11-网络编程.md) |
| 边缘触发 | edge-triggered, ET | epoll 模式之一：状态"从无到有"只通知一次，必须一口气读到 EAGAIN，否则数据滞留 | [ch11](ch11-网络编程.md) |
| 操作系统 | Operating System | 管理硬件资源、为程序提供服务的系统软件，开机后第一个接管机器的大型程序 | [ch01](ch01-操作系统导论.md) |
| 超级块 | superblock | 文件系统的"户口本"：记录块大小、inode 总数等全局信息，0.11 中见 `fs/super.c` | [ch09](ch09-文件系统.md) |
| 调度 | scheduling | 几十个进程抢一个 CPU，由 `schedule()` 决定下一个运行谁、跑多久 | [ch06](ch06-CPU调度.md) |
| 调度权重表 | prio_to_weight | nice 值→权重的对照表（约 1024×1.25^-nice），决定 CPU 时间按什么比例分：nice 0 对 nice 10 约为 1024 : 110 | [ch06](ch06-CPU调度.md) |
| 丢失唤醒 | lost wake-up | "检查条件"与"睡下"之间被插进一次唤醒，唤醒落空、进程睡在已就绪的资源上——0.11 `sleep_on` 的著名竞态 | [ch12](ch12-并发与同步.md) |
| 独占唤醒 | exclusive wakeup | 唤醒只放行一个等待者、其余继续睡（现代内核的 WQ_FLAG_EXCLUSIVE/EPOLLEXCLUSIVE），专治惊群 | [ch12](ch12-并发与同步.md) |
| 段选择子 | segment selector | 保护模式下段寄存器里存的"登记表行号"：高 13 位是描述符表索引，1 位选 GDT/LDT，低 2 位是请求特权级 | [ch02](ch02-计算机硬件基础.md) |
| 分段 | segmentation | x86 用"段基址+偏移"先把程序里的地址翻译成线性地址，0.11 用它实现任务隔离 | [ch02](ch02-计算机硬件基础.md) |
| 分页 | paging | 把内存切成 4KB 的页，用页表完成线性地址到物理地址的映射，虚拟内存的基础 | [ch07](ch07-内存管理.md) |
| 管道 | pipe | 一端写一端读的单向字节流，`ls \| more` 里那个竖线，实现在 `fs/pipe.c` | [ch08](ch08-进程间通信.md) |
| 挂载 | mount | 把一个文件系统接到目录树的某个目录上，之后访问该目录即访问该文件系统 | [ch09](ch09-文件系统.md) |
| 缓冲区 | buffer cache | 内核在内存里缓存磁盘块的仓库，同一块反复读写不必反复摸硬盘，见 `fs/buffer.c` | [ch09](ch09-文件系统.md) |
| 惊群 | thundering herd | 一个事件唤醒了所有等待者、却只有一个能拿到资源，其余白忙一场；nginx 用锁避免它 | [ch12](ch12-并发与同步.md) |
| 进程 | process | "运行中的程序"：代码+数据+一份记账本（task_struct），是资源分配的基本单位 | [ch04](ch04-进程管理.md) |
| 进程控制块 | PCB | 进程的记账本，Linux 里就是 `struct task_struct`：状态、寄存器现场、打开文件表全在里面 | [ch04](ch04-进程管理.md) |
| 竞态条件 | race condition | 结果取决于执行时序的缺陷：两个执行流同时"读—改—写"同一变量就可能丢更新 | [ch12](ch12-并发与同步.md) |
| 交换分区 | swap | 磁盘上专门暂存"换出内存页"的区域，内存不够时拿空间换时间 | [ch07](ch07-内存管理.md) |
| 块设备 | block device | 按固定大小的块随机读写的设备（硬盘），可挂文件系统，驱动在 `kernel/blk_drv/` | [ch10](ch10-设备与IO.md) |
| 临界区 | critical section | 访问共享资源的代码段，进入前要加锁、出来要解锁，否则可能发生竞态条件 | [ch12](ch12-并发与同步.md) |
| 零拷贝 | zero copy | 让数据不在"内核—用户"之间来回搬运的技术（mmap、sendfile），省下 CPU 和内存带宽 | [ch11](ch11-网络编程.md) |
| 逻辑地址 | logical address | 程序里写出的"段：偏移"地址，每条指令看到的都是它 | [ch07](ch07-内存管理.md) |
| 门描述符 | gate descriptor | IDT 里描述"一扇门"的 8 字节表项：存处理程序的段选择子与偏移、门类型（中断门/陷阱门）和 DPL | [ch05](ch05-中断与系统调用.md) |
| 目录项 | directory entry | 目录文件里的一条记录："文件名 → inode 号"的对应关系，路径解析的台阶 | [ch09](ch09-文件系统.md) |
| 内存屏障 | memory barrier | 禁止编译器/CPU 把前后的指令重排越界，保证"先清锁、后放行"这类顺序对所有核可见——解锁为什么敢不用 CAS 的答案 | [ch12](ch12-并发与同步.md) |
| 内核 | kernel | 操作系统里常驻内存、拥有最高权限的核心程序，`linux-0.11/` 整个目录就是它 | [ch01](ch01-操作系统导论.md) |
| 内核态 | kernel mode | CPU 的最高特权级（Ring 0）：可以执行任何指令、摸任何硬件，只有内核代码在跑 | [ch05](ch05-中断与系统调用.md) |
| 缺页异常 | page fault | 访问的页不在内存（或权限不符）时 CPU 抛出的异常（14 号），虚拟内存的"进货铃" | [ch07](ch07-内存管理.md) |
| 全连接队列 | accept queue | 完成三次握手、等待 `accept()` 取走的连接队列，长度由 `listen(backlog)` 限定 | [ch11](ch11-网络编程.md) |
| 三次握手 | 3-way handshake | TCP 建连过程：SYN → SYN+ACK → ACK，双方各确认一次"你能听到我吗" | [ch11](ch11-网络编程.md) |
| 上下文切换 | context switch | 换进程运行前把寄存器现场存进旧 task_struct、再装入新进程的现场 | [ch06](ch06-CPU调度.md) |
| 时间片 | time slice | 进程一次连续使用 CPU 的额度，0.11 里就是 task_struct 的 `counter` 字段 | [ch06](ch06-CPU调度.md) |
| 实时信号 | real-time signal | POSIX.1b 的 SIGRTMIN~SIGRTMAX（Linux 上是 34~64 号）：内核为它们维护真正的排队队列，发几次投几次 | [ch08](ch08-进程间通信.md) |
| 时钟中断 | timer interrupt | 每 10ms（0.11 的 HZ=100）由定时器芯片发一次中断，内核靠它给时间片倒计时 | [ch06](ch06-CPU调度.md) |
| 守护进程 | daemon | 脱离终端、在后台长期服役的进程，nginx 的 master/worker 进程就是典型 | [ch11](ch11-网络编程.md) |
| 水平触发 | level-triggered, LT | epoll 默认模式：只要缓冲区里还有数据，每次 `epoll_wait` 都会再通知你一遍 | [ch11](ch11-网络编程.md) |
| 索引节点 | inode | 文件的"身份证"：大小、权限、数据块位置等元数据，文件名反而在目录项里 | [ch09](ch09-文件系统.md) |
| 套接字 | socket | 网络通信端点的抽象，用法和文件描述符一样：open（connect/bind）、read、write | [ch11](ch11-网络编程.md) |
| 特权级 | privilege level（Ring 0~3） | x86 的 4 级权力等级：内核住 Ring 0、应用住 Ring 3，Ring 1、2 基本闲置；跨界只能走系统调用门 | [ch02](ch02-计算机硬件基础.md) |
| 文件描述符 | file descriptor, fd | 进程打开文件的编号（0/1/2 是标准输入/输出/错误），是操作打开文件的句柄 | [ch09](ch09-文件系统.md) |
| 物理地址 | physical address | 内存条上的真实位置，分段、分页翻译链的终点 | [ch07](ch07-内存管理.md) |
| 系统调用 | system call | 应用请求内核代办事务的唯一窗口，0.11 里就是 `int 0x80` 这条指令 | [ch05](ch05-中断与系统调用.md) |
| 线程 | thread | 同一进程内共享地址空间、各自独立调度的执行流，nginx 用 worker 内线程分担工作 | [ch12](ch12-并发与同步.md) |
| 线性地址 | linear address | 分段之后、分页之前的地址；0.11 里逻辑地址经过段机制就得到它 | [ch07](ch07-内存管理.md) |
| 陷阱 | trap | 软件主动触发的同步异常，如 `int 0x80` 和缺页异常，"陷阱"与中断相对 | [ch05](ch05-中断与系统调用.md) |
| 行规程 | line discipline | 终端驱动里把原始字节"烹制成行"的加工层：回显、退格、Ctrl+C 转信号，全发生在 copy_to_cooked | [ch10](ch10-设备与IO.md) |
| 信号 | signal | 内核发给进程的异步通知（编号 1~31），0.11 用一个 long 位图记录待处理信号 | [ch08](ch08-进程间通信.md) |
| 信号不排队 | standard signals don't queue | 标准信号（1~31）的账本只有"有/没有"一位：pending 期间连发多次只投递一次，0.11 的位图把这一点暴露无遗 | [ch08](ch08-进程间通信.md) |
| 写时复制 | copy on write, COW | fork 后父子共享物理页并标记只读，谁先写谁领新页——少拷贝一大半 | [ch07](ch07-内存管理.md) |
| 页表 | page table | 记录"线性页号 → 物理页框号"的映射表，0.11 是两级页表，见 `mm/memory.c` | [ch07](ch07-内存管理.md) |
| 页表项标志位 | page table entry flags | 页表项低 12 位的硬件开关：P 存在、R/W 读写、U/S 特权、A 已访问、D 已弄脏——COW 靠 R/W，换页与回写靠 A/D | [ch07](ch07-内存管理.md) |
| 用户态 | user mode | CPU 的低特权级（Ring 3）：应用程序只能算数、算内存，摸硬件必须靠系统调用 | [ch05](ch05-中断与系统调用.md) |
| 硬链接 | hard link | 多个目录项直接指向同一个 inode，删一个名字只是引用计数减一 | [ch09](ch09-文件系统.md) |
| 原子操作 | atomic operation | 要么全做完、要么没开始的操作，不可被中断或其他执行流插队 | [ch12](ch12-并发与同步.md) |
| 自旋锁 | spinlock | 拿不到锁就一直原地循环查询的锁，适合锁持有时间极短的场景 | [ch12](ch12-并发与同步.md) |
| 中断 | interrupt | 硬件打断 CPU 当前工作喊人来处理（键盘、硬盘、时钟），是内核感知世界的耳朵 | [ch02](ch02-计算机硬件基础.md) |
| 中断向量表 | IDT | 中断号 → 处理函数入口的表，0.11 在 `traps.c` 的 `trap_init()` 里设置 | [ch05](ch05-中断与系统调用.md) |
| 主次设备号 | major/minor number | 设备文件的身份证：主设备号找驱动程序，次设备号区分该驱动下的哪个具体设备 | [ch10](ch10-设备与IO.md) |
| 字符设备 | char device | 按字节流顺序访问的设备（键盘、串口、终端），驱动在 `kernel/chr_drv/` | [ch10](ch10-设备与IO.md) |

---

## 附录 B C 语言速查——读懂内核源码的十个必备招式

大学一年级 C 语言课讲到指针、结构体就算完了，而内核源码把这十几样东西
用出了花。下面十招覆盖 `linux-0.11/` 和 `nginx-0.1.0/` 里最常见的写法，
每招都是：是什么 → 一个取自真实源码的迷你例子 → 在哪见过。
读罢十招还有余力？文末另备两道加餐：offsetof 的双向用法与 AT&T 汇编速成。

### 招式 1　指针与多级指针

指针是"存放地址的变量"；多级指针存放的是"指针的地址"。内核里最常见的
两级结构是：**指针数组**（一排格子，每格放一个指针）+ **二级指针参数**
（函数要修改格子本身，就必须拿到格子的地址）。

```c
/* 任务表：64 个格子，每格放一个进程的 task_struct 指针（kernel/sched.c:65） */
struct task_struct * task[NR_TASKS] = {&(init_task.task), };

/* wake_up 要把"格子里放谁"改成 NULL，所以参数是格子的地址（二级指针） */
void wake_up(struct task_struct **p)      /* kernel/sched.c:188 */
{
    if (p && *p) {
        (**p).state = 0;    /* 两次解引用：格子→指针→进程，改它的状态 */
        *p = NULL;          /* 把格子清空 */
    }
}
```

在哪见过：`linux-0.11/kernel/sched.c` 的 `schedule()` 里 `p = &task[NR_TASKS]`
配合 `*--p` 从表尾往前扫——指针当游标用；`sleep_on()`/`interruptible_sleep_on()`
的参数同样是 `struct task_struct **p`。

### 招式 2　结构体嵌套与成员访问

`. ` 用于"结构体变量取成员"，`->` 用于"结构体指针取成员"。内核结构体层层
嵌套，于是出现 `->` 连环：`current->filp[fd]->f_inode->i_size`——current 是
进程指针，filp 是打开文件表，f_inode 又指向 inode，一路点到底。

```c
struct task_struct {                      /* include/linux/sched.h:78 */
    long state;                           /* 进程状态 */
    ...
    struct sigaction sigaction[32];       /* 成员是结构体数组 */
    struct m_inode * pwd;                 /* 成员是指向其他结构体的指针 */
    struct file * filp[NR_OPEN];          /* 文件描述符表 */
    struct desc_struct ldt[3];            /* 局部描述符表 */
    struct tss_struct tss;                /* 整个 TSS 直接嵌在末尾 */
};
/* tss 里还有 tss.eflags——嵌套套嵌套，内核乐此不疲 */
```

在哪见过：`linux-0.11/include/linux/sched.h` 的 task_struct；nginx 的
`ngx_http_request_t` 同样嵌着连接、内存池一大串结构体。

### 招式 3　typedef 与 struct 前置声明

C 里 `struct A` 和 `A` 是两回事（后者必须 typedef 出来才有）。nginx 的统一
写法是：结构体带标签名，再 typedef 一个短别名；头文件之间只互相声明
"有这么个结构体"（前置声明），可以减少 include、打破循环依赖。

```c
struct ngx_event_s { ... };                    /* src/event/ngx_event.h:48 */
typedef struct ngx_event_s  ngx_event_t;       /* src/core/ngx_core.h:20 */
/* 之后全部代码只写 ngx_event_t；别的头文件只需一行
   struct ngx_event_s;  就能声明指向它的指针，不必 include 整个头文件 */
```

在哪见过：`nginx-0.1.0/src/core/ngx_core.h` 集中 typedef 了全套类型；
0.11 里也有 `typedef int (*fn_ptr)();`（`include/linux/sched.h:38`）。

### 招式 4　函数指针与函数指针表

函数名就是函数入口地址，可以存进变量、放进数组。内核用它做**分发表**：
一张表按编号索引函数，调度逻辑就退化成一行查表。0.11 的系统调用、
nginx 的过滤模块，都是这张思想的两个应用。

```c
/* 系统调用表：下标就是系统调用号（include/linux/sys.h） */
fn_ptr sys_call_table[] = { sys_setup, sys_exit, sys_fork, sys_read, ... };
/* 汇编入口按下标跳转（kernel/system_call.s:94，%eax 里存着调用号）：
   call _sys_call_table(,%eax,4)   —— 每项 4 字节 */

/* nginx 反过来"头插"：每个过滤模块启动时把自己设为链顶，旧链顶存进自己的 next */
ngx_int_t  (*ngx_http_top_header_filter)(ngx_http_request_t *r);  /* src/http/ngx_http.c:26 */
```

在哪见过：`linux-0.11/include/linux/sys.h`（72 个 sys_xxx 函数列队）；
`nginx-0.1.0/src/http/ngx_http.c` 第 26~27 行的两个过滤链顶端指针。

### 招式 5　宏函数与带参宏

`#define` 不只定义常量，还能定义"长得像函数"的宏。没有函数调用开销、
却能生成重复代码。0.11 用 4 个宏生成了全部 70 多个系统调用函数的
外壳——不写宏就得手抄 70 遍同样的汇编。

```c
#define _syscall0(type,name)                 /* include/unistd.h:133 */
type name(void) { long __res;
    __asm__ volatile ("int $0x80"            /* 触发系统调用 */
        : "=a" (__res)
        : "0" (__NR_##name));                /* ## 把名字拼成编号，如 __NR_getpid */
    if (__res >= 0) return (type) __res;
    errno = -__res; return -1;
}
/* lib/write.c 里一句 _syscall3(int,write,int,fd,const char *,buf,int,n)
   就"变"出完整的 write() 函数，参数经 %ebx/%ecx/%edx 传入内核 */
```

在哪见过：`linux-0.11/include/unistd.h` 第 133~186 行的
`_syscall0`/`_syscall1`/`_syscall2`/`_syscall3`；`linux-0.11/lib/` 全目录使用。

### 招式 6　volatile 与内嵌汇编

C 编译器会"自作聪明"地优化代码，但有些指令它看不懂也不能动：
`volatile` 告诉编译器"这个变量随时可能被你不知道的力量改变，别优化"；
内嵌汇编（`__asm__`）则在 C 里直接插机器指令，冒号后是"输出：输入"清单。

```c
extern inline unsigned char get_fs_byte(const char * addr)
{                                              /* include/asm/segment.h:1 */
    unsigned register char _v;
    __asm__ ("movb %%fs:%1,%0"                 /* 用 %fs 段寄存器取用户内存 */
        : "=r" (_v)                            /* 输出：_v 放寄存器 */
        : "m" (*addr));                        /* 输入：addr 指向的内存 */
    return _v;
}
```

在哪见过：`linux-0.11/include/asm/` 下四个头文件全是这种内嵌汇编
（segment.h、io.h、system.h、memory.h）；`unistd.h` 的系统调用宏加了
`volatile`；nginx 的多线程代码里也有 `ngx_thread_volatile`。

### 招式 7　位运算

内核舍不得每一个字节：一个 long 就是 32 个开关，一次页表项读写就带一组
标志位。必须熟练四件事：置位 `x |= (1<<n)`、清位 `x &= ~(1<<n)`、
测位 `x & (1<<n)`、取一段 `x & mask`。

```c
current->blocked = newmask & ~(1<<(SIGKILL-1));   /* kernel/signal.c:24 */
/* 信号屏蔽位图：每位一个信号，SIGKILL 谁都不许屏蔽 */

page_table[(address>>12) & 0x3ff] = page | 7;     /* mm/memory.c:216 */
/* 页表项低 3 位是标志：P(存在)/R/W(读写)/U/S(用户)，|7 即全部打开 */
if ((*page_table)&1) { ... }                      /* 只测"存在"位 */
*table_entry |= 2;                                /* un_wp_page 写时复制：补上可写位 */

/* 位图分配器：找第一个 0 比特，即第一个空闲 inode/磁盘块（fs/bitmap.c） */
if ((j = find_first_zero(bh->b_data)) < 8192)     /* set_bit/clear_bit 定义在 string.h */
```

在哪见过：`linux-0.11/kernel/signal.c`（信号位图）、`linux-0.11/mm/memory.c`
（页表项标志，`put_page()`/`un_wp_page()`）、`linux-0.11/fs/bitmap.c`
（inode 与块位图）。

### 招式 8　offsetof 与"从成员反推宿主"

结构体成员比宿主小，但知道"成员的地址"和"成员在宿主里的偏移量"，
一减就得到宿主首地址。nginx 把一个红黑树节点内嵌在每个定时事件里，
从树节点反查事件就靠这一减。

```c
/* src/event/ngx_event_timer.c:135 —— node 是事件内嵌的树节点 */
ev = (ngx_event_t *) ((char *) node - offsetof(ngx_event_t, rbtree_key));
/* offsetof 给出 rbtree_key 在 ngx_event_t 里的字节偏移，相减即得首地址 */
```

在哪见过：`nginx-0.1.0/src/event/ngx_event_timer.c` 第 135~136 行（源码里有
中文注释详解）；`rbtree_key` 字段定义在 `src/event/ngx_event.h:151`。
这是"侵入式数据结构"的标准玩法——Linux 内核的 `list_entry` 同理。

### 招式 9　多文件编译与 Makefile 入门

几十个 .c 文件谁先编译、怎么链接？Makefile 用"三要素"回答：
**目标**（要生成什么）：**依赖**（比目标新的文件会触发重建）：**命令**
（怎么生成）。内核几千个文件只靠一条规则描述，`make` 自动算出最少重编哪些。

```make
all: Image                       # 目标 all 依赖 Image

Image: boot/bootsect boot/setup tools/system tools/build
	tools/build boot/bootsect boot/setup tools/system $(ROOT_DEV) > Image
	                                     # 有任一依赖比 Image 新，就执行此命令

tools/system: boot/head.o init/main.o $(ARCHIVES) $(DRIVERS) $(LIBS)
	$(LD) $(LDFLAGS) boot/head.o ...    # 把所有 .o 链接成内核

.c.o:                                # 隐式规则：任何 .c 都能这么变 .o
	$(CC) $(CFLAGS) -c -o $*.o $<    # $< 是依赖文件，$*.o 是目标文件
```

在哪见过：`linux-0.11/Makefile`（顶层组装，`ARCHIVES`/`DRIVERS`/`LIBS`
变量收集各子目录产物）；每个子目录还有自己的 Makefile，两层级联。

### 招式 10　头文件与 extern、static 的作用

`extern` 是"承诺"：这个函数/变量在别的文件里定义，链接时自然找得到——
于是头文件只放声明，实现分散在各 .c。`static` 则相反，把函数/变量"锁"在
本文件内：不污染全局命名空间，也方便编译器优化。内核大量使用
static 小函数，就是为了让每个文件各自独立、互不干扰。

```c
/* include/linux/sys.h：72 行 extern，全是"声明在别处"的承诺 */
extern int sys_fork();
extern int sys_read();
...

/* kernel/chr_drv/console.c：45 个 static，屏幕驱动内部工具全不出文件 */
static unsigned char  video_type;     /* 本文件私有的全局变量 */
static void set_origin(void) { ... }  /* 本文件私有的函数 */
```

在哪见过：`linux-0.11/include/linux/sys.h` 与 `linux-0.11/kernel/chr_drv/console.c`
（全文件约 45 处 static）；各 .c 文件开头成串的 `#include` 则是 extern 的来源。

### 加餐一　offsetof 的双向用法：成员反推宿主，宿主取成员

招式 8 只用 offsetof 做了"减法"：成员地址 − 偏移 = 宿主地址。它还有一面
"加法"：宿主首地址 + 偏移 = 成员地址——`&ev->rbtree_key` 就是一次隐形的
加法。挂树时把内嵌成员的地址交给通用容器，容器就能服务任意宿主结构；
取回时再用减法还原宿主。一进一出，"侵入式数据结构"才两头都通。

```c
/* 正向（加法）：&ev->rbtree_key 即"首地址 + offsetof"——把内嵌节点交给通用容器 */
ngx_rbtree_delete((ngx_rbtree_t **) &ngx_event_timer_rbtree,
                  &ngx_event_timer_sentinel,
                  (ngx_rbtree_t *) &ev->rbtree_key);   /* src/event/ngx_event_timer.c:168 */

/* 反向（减法，招式 8）：成员地址 - offsetof = 宿主首地址，同一对结构的回程票 */
ev = (ngx_event_t *) ((char *) node - offsetof(ngx_event_t, rbtree_key));  /* 同文件 135~136 行 */
```

在哪见过：一对加减就在同一文件——`nginx-0.1.0/src/event/ngx_event_timer.c`
第 135~136 行减回去、第 168 行加过去。`linux-0.11` 里同款思想更显朴素：
把成员地址当独立结构用——`sleep_on(&bh->b_wait)`（`fs/buffer.c:40`）、
`sleep_on(&inode->i_wait)`（`fs/pipe.c`）都是"首地址+偏移"取到成员，
再把它当作一条等待队列交出去。Linux 内核后来把这套双向玩法封装成标准宏
`container_of`。

### 加餐二　读 0.11 的 AT&T 汇编（GNU 语法）

0.11 的 boot/、kernel/ 下散着十来个 .s 文件，用的是 GNU as 的 AT&T 风格，
和课堂常见的 Intel 汇编正好"拧着"：① **源、目的相反**——`mov %ax,%ds`
读作"把 ax 装进 ds"，Intel 写成 mov ds, ax；② 寄存器加 `%`、立即数加 `$`；
③ 助记符带 `l` 后缀表 32 位——`movl` 的 l 即 long；④ 注释用 `#`（而
bootsect.s、setup.s 这些 16 位实模式文件 GNU as 默认接受 `!`）。

```asm
movl $0x10,%eax    # l=32 位；$0x10 是立即数；注释以 # 起（boot/head.s:26）
mov %ax,%ds        # 方向与 Intel 相反：源在前、目的在后——把 ax 装进 ds（head.s:27）
jmpi 0,8           ! "段:偏移"远跳转：跳到选择子 8 那个段的偏移 0——
                   ! 保护模式的第一次换挡（boot/setup.s:193）
```

在哪见过：`boot/head.s` 与 `boot/setup.s` 全文；第 5 章出现过的
`call _sys_call_table(,%eax,4)`（kernel/system_call.s:94）也是 AT&T 独有的
变址寻址写法——逗号隔开的"基址、变址、比例因子"。

---

## 附录 C 本仓库源码树地图与参考资料

### C.1 版本演进表

行数为仓库内 `.c/.h/.s` 文件的实际统计（约数）。

| 目录 | 年代 | 规模（约） | 一句话看点 | 教材用在哪 |
|---|---|---|---|---|
| `linux-0.00` | 1991 年中 | 约 500 行 | 内核雏形：两个任务来回切换，目录里的 `Image` 是当年可启动镜像 | [第 1 章](ch01-操作系统导论.md)、[第 3 章](ch03-系统启动.md) |
| `linux-0.01` | 1991.9 | 约 1 万行 | 第一个正式版，无网络、注释稀少，"内核可以很小" | [第 1 章](ch01-操作系统导论.md) |
| `linux-0.11` | 1991.12 | 约 1.3 万行 | 五脏俱全的稳定版：进程、分页、文件系统、驱动全齐——**本教材主力** | [第 2~10 章](ch02-计算机硬件基础.md) |
| `linux-0.12` | 1992.1 | 约 1.9 万行 | 引入数学协处理器模拟与大量修复 | 各章延伸阅读 |
| `linux-0.98` | 1992 | 约 5.6 万行 | 网络子系统并入前的成长期版本 | 对照阅读 |
| `linux-1.0` | 1994 | 约 17 万行 | 第一个正式发布的 1.0，模块化雏形已现 | 对照阅读 |
| `linux-1.2.0` | 1995 | 约 28 万行 | 支持更多硬件平台 | 对照阅读 |
| `linux-2.4.0` | 2001 | 约 300 万行 | SMP 多核、企业级时代的内核 | [第 6 章](ch06-CPU调度.md)对照 |
| `linux-2.6.0` | 2003 | 约 520 万行 | O(1) 调度器、内核可抢占 | [第 6 章](ch06-CPU调度.md)对照 |
| `linux-2.6.12` | 2005 | 约 600 万行 | 2.6 稳定系列中期形态 | 对照阅读 |
| `linux-2.6.35` | 2010 | 约 1200 万行 | CFS 完全公平调度（2.6.23 起）、现代内存与 TCP | [第 6、7、11 章](ch06-CPU调度.md)对照 |
| `nginx-0.1.0` | 2004.10 | 约 4.5 万行（含本仓库中文注释） | nginx 首个公开版，已逐行注释：用户态如何"用好"内核 | [第 11、12 章](ch11-网络编程.md) |

### C.2 linux-0.11 目录地图（教材主力源码）

```
linux-0.11/
├── boot/          启动三件套（第 2、3 章）
│   ├── bootsect.s   从软盘把自己搬到内存，再加载 setup
│   ├── setup.s      收集硬件参数，打开 A20、进入保护模式
│   └── head.s       重新设置页表，跳入 main.c——汇编的最后一棒
├── init/
│   └── main.c       1 号进程：内核初始化总装车间，fork 出 shell（第 3 章）
├── kernel/        进程与内核核心（第 4~6、8 章）
│   ├── fork.c         copy_process()：fork 的全部魔法
│   ├── sched.c        schedule() 调度、sleep_on/wake_up、时钟中断处理
│   ├── system_call.s  int 0x80 汇编入口 + 时钟中断入口
│   ├── sys.c          各系统调用的 C 语言实现（fork 的"账房"）
│   ├── exit.c         进程退出与僵尸清理
│   ├── signal.c       信号的发送与处理（第 8 章）
│   ├── traps.c        异常（除零、缺页等）处理入口的设置
│   ├── asm.s          低级异常的公共出口
│   ├── printk.c / vsprintf.c / panic.c / mktime.c   内核自用的"libc"
│   ├── math/          协处理器模拟（没装 FPU 的机器用）
│   ├── blk_drv/       块设备驱动（第 10 章）：ll_rw_blk.c 请求队列、
│   │                  hd.c 硬盘、floppy.c 软盘、ramdisk.c 内存盘
│   └── chr_drv/       字符设备驱动（第 10 章）：tty_io.c 终端总入口、
│                      console.c 屏幕、keyboard.S 键盘、serial.c 串口
├── mm/            内存管理（第 7 章）
│   ├── memory.c       页表维护、缺页处理、写时复制
│   └── page.s         缺页异常的汇编入口
├── fs/            MINIX 文件系统（第 9 章）
│   ├── super.c        超级块与挂载        bitmap.c   inode/块位图
│   ├── inode.c        inode 读写与引用      namei.c    路径解析
│   ├── buffer.c       缓冲区缓存            open.c     open/close
│   ├── read_write.c   read/write 分发       exec.c     execve 装载程序
│   ├── pipe.c         管道（第 8 章）       file_dev.c 普通文件读写
│   ├── char_dev.c     字符设备文件          block_dev.c 块设备文件
│   └── file_table.c / stat.c / fcntl.c / ioctl.c / truncate.c
├── include/       全部头文件
│   ├── linux/         sched.h(进程) fs.h(文件) sys.h(系统调用表)…
│   ├── asm/           segment.h io.h system.h memory.h（内嵌汇编）
│   └── sys/           types.h stat.h wait.h（POSIX 风格接口）
│   └── unistd.h / signal.h / string.h / ctype.h …
├── lib/           用户态库：write.c open.c dup.c wait.c malloc.c 等，
│                  全部用 _syscallN 宏包出系统调用（附录 B 招式 5）
└── tools/
    └── build.c        把 bootsect+setup+system 拼成 Image 镜像的工具
```

### C.3 linux-2.6.35 关键路径（现代对照）

| 路径 | 是什么 | 对照章节 |
|---|---|---|
| `kernel/sched_fair.c` | CFS 完全公平调度器：红黑树按 vruntime 排队，谁最"亏"谁先跑 | [第 6 章](ch06-CPU调度.md) |
| `kernel/fork.c` | 现代 fork/clone：与 0.11 的 copy_process() 对照着看最有趣 | [第 4 章](ch04-进程管理.md) |
| `mm/` | `memory.c`（缺页、COW）、`filemap.c`（页缓存）等现代内存管理 | [第 7 章](ch07-内存管理.md) |
| `fs/` | VFS 核心：`namei.c` 路径解析、`ext4/` 日志文件系统 | [第 9 章](ch09-文件系统.md) |
| `net/ipv4/tcp_input.c` | TCP 状态机与拥塞控制（接收侧），0.11 里网络还没出生 | [第 11 章](ch11-网络编程.md) |

### C.4 参考资料清单

1. **《Linux 内核完全注释 v5.0》**（赵炯著）——对 linux-0.11 逐行注释的经典，
   本仓库 `doc/Linux内核完全注释v5.0.pdf` 内置，每章"延伸阅读"指定了对应章节；
2. **《操作系统导论（OSTEP）》**（Remzi 夫妇著）——概念讲得最亲切的现代教材，
   适合与本书互为对照（英文版官方免费）；
3. **《深入理解计算机系统（CSAPP）》**——第 2、7、9 章的"为什么"都在这里
   （汇编、链接、虚拟内存、I/O）；
4. **《TCP/IP 详解 卷 1》**（W. Richard Stevens）——第 11 章网络内容的权威底本；
5. **`nginx-0.1.0/注释与理论说明.md`**——本仓库自带的 TCP/epoll/并发理论地图，
   第 11、12 章的配套阅读；
6. **oldlinux.org**——赵炯老师的旧 Linux 资料站，本仓库多数版本的出处；
7. **内核官方中文文档**（docs.kernel.org/translations/zh_CN/）——第 6 章 CFS
   一节的公式、权重表与 min_vruntime 即取自其中《调度器设计——CFS》
   （scheduler/sched-design-CFS）；
8. **小林coding**（xiaolincoding.com）——图解网络/图解系统系列：三次握手、
   TIME_WAIT、等待队列等主题的通俗中文讲解（第 5、11、12 章"现代资料"来源）；
9. **JavaGuide、腾讯云社区等公开技术站点**——"Linux 等待队列分析"、8259A
   中断控制器与 TIME_WAIT 排查等经典中文文章的出处（第 4、11 章延伸阅读）。

---

[← 第 13 章 综合实验与课程设计](ch13-综合实验.md)　|　[返回教材首页 README.md](README.md)
