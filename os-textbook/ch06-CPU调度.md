# 第 6 章 CPU 调度

> **学习目标**
> 1. 能用自己的话回答："单核 CPU 只有一条流水线，凭什么能同时跑几十个进程？"
> 2. 说清调度器的三难困境：公平、效率、响应快为什么不可兼得；
> 3. 用生活比喻讲出 FCFS、SJF、优先级、时间片轮转四种经典算法；
> 4. 打开 `linux-0.11/kernel/sched.c`，读懂 `schedule()`、`do_timer()`、`sleep_on()`、
>    `wake_up()`，解释 `counter` 字段如何决定"下一个跑谁"；
> 5. 说出 Linux 调度器三代进化（0.11 扫描 → 2.6 O(1) → CFS 红黑树）各自的挑人方式；
> 6. 做完三个用户态实验，亲眼看到时间片均分、nice 生效和"睡得香起得猛"。

---

## 6.1 从一个问题开始：一条流水线，为什么不卡？

你此刻的电脑：音乐在放、浏览器开着二十多个标签页、终端里还挂着编译。可第 2 章讲过：
**CPU 一次只能执行一条指令**——就算 8 核，进程一多照样超员。一条流水线，几十个
进程排队，为什么音乐不结巴、鼠标不卡顿？

先排除一个误解：**不是**因为 CPU 太快所以"顺便"都干完了——音乐每一拍都有严格
的时间要求，谁也等不了谁。真正的答案是操作系统在玩**魔术**：

> **CPU 在进程之间高速轮换：每个进程每次只跑一小段时间（如 10 毫秒）就被换下，
> 隔几十毫秒又轮回来。人耳听不出 10 毫秒级的声音间隙，人眼看不出 100 毫秒内的
> 画面停顿——轮换发生在感知盲区里，于是每个进程都"感觉"自己在独占电脑。**

这就像辅导作业的老师面对 40 个学生：每人只讲 30 秒就被"请"换下一个，每个人都
觉得"老师一直在管我"。轮换的规则与执行轮换的那只手，就是本章的主角：

- **时间片（time slice）**：分给每个进程的一次性 CPU 使用额度，比如 10 毫秒；
- **调度器（scheduler）**：内核里决定"下一个跑谁"的那段代码。

这两个词不是比喻——0.11 内核里真有它们：每个进程的 PCB（第 4 章的 `task_struct`）
里有一个 `counter` 字段记着时间片余额；`linux-0.11/kernel/sched.c` 里的
`schedule()` 函数就是那只换人的手。本章后半程，我们把这份真实代码逐段读通。

## 6.2 调度要解决什么：一个"三难"问题

调度器的目标可以概括成三个词，但它们**天生互相打架**：

1. **公平**——每个进程分到的 CPU 时间大致均等；
2. **效率**——单位时间完成的总工作量（吞吐量）最大；
3. **响应快**——你敲一下键盘，程序立刻有反应。

为什么不可兼得？看三个进程：A 在编译内核（要跑 10 分钟），B 是终端（在等你
敲字），C 在播音乐（每 10 毫秒都有一拍要处理）：

- **绝对公平**（每人 1/3）：把宝贵的 CPU 切给"正等人敲键盘的终端"纯属浪费——
  公平 ≠ 合理；
- **纯效率**（谁有活干谁干到完）：A 一口气编译 10 分钟，终端和音乐全部失联——
  吞吐量最高，但你会以为死机了；
- **纯响应**（切得越频繁越好）：响应极快，但每次切换要花几微秒、还把缓存搅冷
  （6.7 节细算这笔账），真正干活的时间反而变少。

不同的场景，对三者的取舍不同。一句话记住三种典型场景：

| 场景 | 典型例子 | 首要目标 | 比喻 |
|---|---|---|---|
| 批处理 | 编译农场、科学计算 | 吞吐量 | 流水线工人，只求总产量 |
| 交互式 | 桌面、shell、编辑器 | 响应快 | 前台接待，客人一说话就得理 |
| 实时 | 音频引擎、刹车控制 | 截止时间（deadline） | 消防车，晚到＝没到 |

那轮换的时机从哪来？只有两个，而且**最终都汇入同一个函数 `schedule()`**：

1. **进程主动让出**：睡在等键盘/等磁盘（`sleep_on`），或者干脆退出了（`do_exit`）；
2. **被动被打断**：时钟中断发现"你的时间片用完了"（6.4.2 节）。

## 6.3 经典算法速览：为读懂 0.11 打底

0.11 的调度器是"优先级 + 时间片轮转"的混血。先把四种经典算法各用几行说完。

**FCFS（先来先服务）——食堂排队打饭。** 谁先到谁先打，打完才轮下一个。优点是
简单且绝不饿死人；缺点是你前面站一个买 20 份盒饭的（长任务），后面 10 个买
饮料的（短任务）全体跟着等。

**SJF（短作业优先）——先做小作业。** 哪份作业页数少先做哪份，总等待时间数学上
最优。缺点是要**预知**作业多长（现实里很难），而且大作业可能永远轮不到——
又一个饥饿案例。

**优先级调度——VIP 插队。** 每人身上贴个号，号小的先来。直觉正确——交互式
进程天然该比批处理优先；缺点是低优先级进程可能永远排不上，即**饥饿
（starvation）**。经典解法叫**老化（aging）**：等得越久，优先级自动抬高，总有
一天轮到你。记住这个词，6.4.3 节你会看到 0.11 用 `counter` 玩了一个漂亮的老化
变体。

**RR（时间片轮转）——幼儿园轮流玩滑梯。** 孩子们排一队，每人玩 1 分钟就被请
下来排到队尾，谁也别想霸占滑梯。公平的极致，响应时间只跟队里人数和时间片长短
有关；但对总产量不友好（切换有开销）。时间片长短是门手艺：太短→切换开销占比
飙升；太长→退化成 FCFS。0.11 选了 15 个 tick ≈ 150 毫秒，现代 Linux 约几毫秒。

| 算法 | 一句话规则 | 致命弱点 | 0.11 里有没有 |
|---|---|---|---|
| FCFS | 先来先服务 | 长任务堵门 | 无（不可抢先） |
| SJF | 短的先做 | 要预知未来 | 无 |
| 优先级 | 号小先行 | 饥饿 | **有**（counter 大者优先） |
| RR | 轮流玩滑梯 | 切换开销 | **有**（时间片用完换人） |

## 6.4 源码导读：0.11 的调度器，全部逻辑不到 100 行

放好地图再进山。本章打开的主文件是 `linux-0.11/kernel/sched.c`（全文件 412 行），
配角是 `include/linux/sched.h`（`counter`、`switch_to` 宏）和
`kernel/system_call.s`（时钟中断入口）。第 4 章已经认识过 `task_struct` 里的
`counter` 字段（"这节课还剩几分钟"），本章它从配角转正。

### 6.4.1 schedule()：全场只有一个裁判

`sched.c` 第 104 行起就是调度器本尊。Linus 在它头顶写了句自夸的注释：
"This is GOOD CODE!"。全文不过 40 行。下面把第 104~142 行**原文**一字不改地贴出
（连同 Linus 自己的两句英文注释），再分三段逐行读：闹钟唤醒段 → 挑人循环 → 换人。

```c
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		if (c) break;
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	switch_to(next);
}
```

**第一段（第 111~120 行）：上台先理两笔旧账。** 外层 `for` 把进程表里每个非空
槽位过一遍：

- **第 113~116 行，闹钟段**：进程若用 `alarm()` 定过闹钟、且 `jiffies`（系统开机
  以来的 tick 计数，第 5 章）已走过头，就把 `SIGALRM` 记入它的 `signal` 位图、
  清掉闹钟。为什么发信号的活儿塞在调度器里？因为对方可能正睡着，而调度器保证
  会被定期执行——只有这里能"顺路"把信号送到；
- **第 117~119 行，信号唤醒段**：如果该进程有**未被阻塞的信号**待处理，而且睡
  的是 `TASK_INTERRUPTIBLE` 浅觉，就把它改成 `TASK_RUNNING`——**睡浅觉的可被
  信号叫醒**，醒来即加入第二段的竞选。这正是第 4 章"可中断睡眠"的落地处，
  信号机制的完整故事在第 8 章。

**第二段（第 124~140 行）：挑人循环**（Linus 注释里的 "the scheduler proper"）。
规则朴素得惊人：**在所有 `TASK_RUNNING` 的进程里，选 `counter` 最大的那一个**。
三个细节值得停一停：

1. **扫描从 63 号格倒着扫到 1 号格**——0 号进程（`task[0]`，第 4 章手工造出的
   0 号任务）永远不参与竞选。它是 **idle 任务**：全场没别人可跑时 `next` 保持
   初值 0，`switch_to(0)` 把 CPU 交回给它"空转等中断"；
2. **`counter` 相等怎么办？** 扫描条件是严格大于 `>`，并列时先被扫到的那个
   （槽位号大的）保留席位——无伤大雅的 tie-break；
3. **`if (c) break` 与补发时间片**（第 135 行）：注意 `c` 存的是 counter 的
   **值**而非进程号，`if (c)` 就是"最大余额是否 > 0"。若全场 RUNNING 进程的
   `counter` 都是 0（时间片用光了），就先执行 136~139 行给**所有**进程补发
   时间片，再回头重扫一遍。这段"补发"是 0.11 调度器的灵魂，6.4.3 节细讲。

**第三段（第 141 行）：`switch_to(next)`。** 裁判吹哨换人——这一行的魔法在
6.4.4 节展开。

### 6.4.2 counter：时间片的余额，10 毫秒扣一次

`counter` 从哪来、怎么扣？第 4 章见过源头：`fork()` 时 `p->counter = p->priority`，
即"时间片余额 = 优先级"——0.11 的默认优先级是 15（`sched.h` 第 113 行
`INIT_TASK` 里开头的三个数 `0,15,15`：state、counter、priority）。所以**每个新生
进程领到 15 个 tick 的时间片**。

tick 是多久？`sched.h` 第 5 行：`#define HZ 100`——时钟中断每秒响 100 次，
**1 个 tick = 10 毫秒**，默认时间片 = 15 × 10ms = 150 毫秒。

扣款动作发生在第 5 章讲过的时钟中断里，调用链：时钟芯片每 10ms 发中断 →
`_timer_interrupt`（system_call.s 第 176 行）→ `incl _jiffies`（第 189 行，系统
"心跳" +1）→ `call _do_timer`（第 195 行，进入 C 语言的定时器总管）。
`do_timer()`（sched.c 第 305 行）里与本章直接相关的四行在末尾：

```c
	if ((--current->counter)>0) return;   /* 第 332 行：余额还够，继续跑 */
	current->counter=0;                   /* 第 333 行：扣到 0 为止，不许欠账 */
	if (!cpl) return;                     /* 第 334 行：刚才在内核态？那不换人 */
	schedule();                           /* 第 335 行：时间片用完，换人！ */
```

逐行读：

- **第 332 行**：只有 `current`（此刻在 CPU 上的进程）被扣，每 10 毫秒扣 1；
- **第 333 行**：扣到 0 封顶，防负数；
- **第 334 行**：`cpl` 是被打断时 CPU 的特权级（`system_call.s` 第 191~192 行取
  CS 寄存器低 2 位：0＝内核态，3＝用户态）。如果中断响时进程正在内核态干活，
  0.11 **不**立刻换人——0.11 是"不可抢占"内核，内核代码总要跑完当前这一段
  才敢换人；
- **第 335 行**：用户态时间片用尽，调 `schedule()`——被动换人，正是 6.2 节说的
  第二个调度时机。

### 6.4.3 动态补偿："睡得越香，起得越猛"

现在看 6.4.1 节埋的伏笔——sched.c 第 138~139 行：

```c
	(*p)->counter = ((*p)->counter >> 1) + (*p)->priority;
```

触发条件是"全场 RUNNING 进程的 counter 都为 0"。这行代码同时干了**两件事**，
合起来就是 6.3 节说的"老化"：

1. **补发**：每人重新领到 `priority` 个 tick（默认又是 15）；
2. **奖赏睡眠**：睡眠中的进程 counter 一直没被扣（只有 `current` 被扣），别人每
   耗光一轮时间片，它就白得一次"counter/2 + 15"。于是：

```
某个 priority=15 的进程睡着，别的进程独占 CPU：
  每过 150ms（别人耗光一轮），它的 counter 走：
  15 → 22 → 26 → 28 → 29 → 29 → ……（停在 29）
```

x/2+15=x 的数学不动点是 30，但整数减半向下取整——29>>1=14，14+15=29——序列
实际停在 **29**，每轮只涨不跌。它醒来那一刻 `counter ≈ 29`——全场最大，立刻被
`schedule()` 选中，还能连跑将近两个标准时间片。反过来，**一直在跑的进程每轮
用光后只剩 `priority`**（因为 `0>>1 + 15 = 15`），永远够不着这个天花板。一句
口诀：**睡得越香，起得越猛**。终端、shell 这些"睡等键盘"的进程每次醒来都带着
满额奖赏插队；编译这种"死磕 CPU"的进程自动退居幕后——没有一行代码写着"交互
进程优先"，优先级自己长了出来。

公式里的 `>>1`（减半）是**遗忘因子**：奖赏不能无限囤积（上限 2×priority），否则
一个睡了一小时的进程醒来能霸占 CPU 一分钟。减半让旧账尽快翻篇。

顺带认识一个系统调用：`sys_nice()`（sched.c 第 378 行）——你在命令行用 `nice`
命令运行程序时，最终走到这几行：

```c
int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}
```

看清方向：**nice 值越大，`priority` 被削得越小，补发的时间片越少，进程越
"不优先"**。`nice -n 19` 是最客气的好公民，`nice -n -20`（要 root）是插队狂魔。
再接上 6.4.2 节那根线——fork 时 `p->counter = p->priority`：`sys_nice()` 改的
`priority`，既是**新进程 counter 的初值**（出生领多少时间片），又是**每轮补发的
额度**（用光后回血多少）。nice 一改，从出生到补发两头同时生效，这就是"nice
如何变成 counter 初值"的完整链条。
这个方向感 40 年没变——6.7 节的误区二还要回来鞭它一次。

### 6.4.4 switch_to：一条汇编指令完成"换人"

选好了 `next`，最后一步是 sched.c 第 141 行的 `switch_to(next)`。它不是函数，
而是 `sched.h` 第 171 行定义的**宏**，核心是一条 `ljmp`（long jump）——它的
目标不是内存地址，而是新任务的 **TSS**（x86 CPU 的硬件特性）。CPU 执行它时
自动把当前全部寄存器（eip、eax、esp……）存进**旧进程的 TSS**，再从**新进程的
TSS** 一口气恢复全部寄存器。第 4 章说过 TSS 是"停课瞬间的课堂笔记快照"——
所谓进程切换，物理上就是**换一本笔记接着写**。全部魔法压缩在这一条指令里，
0.11 之后 Linux 改为手工汇编保存现场，但"寄存器快照换人"的语义沿用至今。

### 6.4.5 睡眠与唤醒：sleep_on()、interruptible_sleep_on() 和 wake_up()

6.2 节说过，调度的另一半时机是进程**主动让出**。sched.c 第 151~194 行的三个
函数就是全部剧本，原文一并贴出（一字未改），对照着读：

```c
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}
```

四个观察：

1. **睡眠 = 改状态 + 让出 CPU**（第 159~162 行）。`tmp = *p; *p = current;`——
   `*p` 是这条等待队伍的**队头记号**，这两行让我顶到队头、并记下睡在我前面的
   那位；随后把 `state` 改成睡眠态、调 `schedule()` 把 CPU 送回调度器——进程
   就此从就绪队伍里消失，直到被人改回 `TASK_RUNNING`（值恰为 0）才回归；
2. **深觉与浅觉，一字之差**。`sleep_on` 睡 `TASK_UNINTERRUPTIBLE`（第 161 行，
   **不可中断**）——"睡死觉"，信号也叫不醒，专为"等的事马上就来、别的都别烦
   我"的短等待（如等一块磁盘块到位）；`interruptible_sleep_on` 睡
   `TASK_INTERRUPTIBLE`（第 177 行，**可中断**）——浅觉，**信号能叫醒**，用于
   可能遥遥无期的用户事件（如等键盘敲下）。谁负责叫醒浅觉者？正是 6.4.1 节
   `schedule()` 开头第 117~119 行——有未阻塞信号的浅觉者被改成 `TASK_RUNNING`。
   信号如何递送是第 8 章的主题，那一章的机制有一半就踩在这三行上；
3. **隐式等待链**（第 159~164 行）。A 先睡、B 再睡、C 最后睡，就串成一条链：
   `*w→C，C.tmp→B，B.tmp→A`。`wake_up()`（第 188~194 行）只显式叫醒队头 C、
   清空队头记号；C 上台后从 `schedule()` 返回，顺手 `tmp->state=0` 叫醒 B；
   B 上台再叫醒 A——**一次唤醒沿链传播**，每人只轮到一次 CPU，天然公平。
   `interruptible_sleep_on` 的 `repeat` 循环（第 179~182 行）是"只叫队头"规矩
   的补丁：浅觉者被信号叫醒后检查 `*p != current`——若队头已被别人顶替，就把
   那位叫醒、自己 `goto repeat` 接着睡，直到自己当上队头、被 `wake_up` 正式放行；
4. **著名竞态点**。这根"指针栈"没有任何加锁：`sleep_on` 里 `tmp=*p` 与
   `*p=current` 之间，`wake_up` 若恰好插进来把 `*p` 清成 NULL，唤醒就凭空丢失，
   有人一睡不醒。这是 0.11 最常被后人点名批评的地方——成因、后果与现代修复
   （等待队列 + 自旋锁）在第 12 章展开。此刻只需记住：**读出味道了，也读出
   毛破了，这段代码就没白读**。

## 6.5 三代调度器进化史：从 40 行到 3690 行

0.11 的调度器美在简单，但简单有价：全场扫描是 O(n)；"counter 最大者优先"这一招
到了几十个进程、多用户、多核的机器上，既不够精细也跟不上规模。Linux 的调度器
此后推倒重来过两次。仓库里恰好躺着三个时代的完整源码，逐个看它们的挑人方式。

### 6.5.1 第一代：0.11，"最大 counter 优先"（1991）

上面刚读完：**扫全场，谁时间片余额最大谁上**，数据结构是朴素数组 `task[64]`，
复杂度 O(n)。40 行换来的洞察（动态补偿、交互优先）影响了此后所有调度器。

### 6.5.2 第二代：2.6.0 的 O(1) 调度器（2003）

打开 `linux-2.6.0/kernel/sched.c`（2916 行，是 0.11 的 7 倍），先找数据结构——
第 186 行的 `prio_array`：

```c
struct prio_array {
	int nr_active;                          /* 这个数组里有几个活进程 */
	unsigned long bitmap[BITMAP_SIZE];      /* 位图：第 i 位=1 表示优先级 i 有人 */
	struct list_head queue[MAX_PRIO];       /* 140 条队列，每条一个优先级 */
};
```

思路变了：**不比较，直接查**。每个进程按优先级放进 140 条队列中的一条（
`MAX_PRIO` 定义在 `include/linux/sched.h` 第 285 行）；挑下一个进程时不再遍历
全场，而是在 bitmap 上找**第一个为 1 的位**。`schedule()`（第 1471 行）里的
挑人只剩三行（第 1558~1560 行）：

```c
	idx = sched_find_first_bit(array->bitmap);   /* 最高优先级里有没有人？ */
	queue = array->queue + idx;                  /* 定位到那条队列 */
	next = list_entry(queue->next, task_t, run_list);  /* 取队头 */
```

这三行里把 O(1) 坐实的是 `sched_find_first_bit`（`include/asm-i386/bitops.h`
第 433 行）。bitmap 在概念上是 **140 个优先级各占一位**的位图（底层实现是
`BITMAP_SIZE` 个 `unsigned long`——i386 上 5 个字、共 160 位），第 i 位为 1 表示
优先级 i 的队列里有活进程。它按字逐个检查，遇到第一个非零字就用 `__ffs`
（find-first-set，i386 上是一条 `bsf` 指令）取出字内第一个置位的位置——最多
5 次判断加 1 条指令，与系统里有 10 个还是 10000 个进程毫无关系。

也就是说，不管负载多重，挑人耗时恒定——**O(1)** 因此得名。时间片
用完的人被丢进另一个 `expired` 数组，等 `active` 全部耗光，两个数组指针一换
（第 1550 行附近），一轮重新开始——0.11 那个"全场补发时间片"的思想还在，只是
从"挨个补"变成了"换一下指针"。

### 6.5.3 第三代：2.6.35 的 CFS 完全公平调度（2010）

O(1) 调度器参数繁多、交互性调优的补丁越打越复杂，2007 年 Ingo Molnar 推倒重来，
交出 CFS（Completely Fair Scheduler），2.6.23 起成为 Linux 的默认调度器。核心
文件 `linux-2.6.35/kernel/sched_fair.c`（3690 行）里，一个新字段取代了 `counter`：

> **vruntime（virtual runtime，虚拟运行时间）**：每个进程"应当分到的 CPU 时间"
> 的记账。谁跑得越多，vruntime 涨得越快；谁睡得越多（或权重越高），涨得越慢。

**记账公式（现代资料）**：CFS 每个时钟 tick 都给当前进程记一笔账（代码落在
`update_curr()`，sched_fair.c 第 515 行）——

```
vruntime += delta_exec × NICE_0_LOAD / weight
```

- `delta_exec`：两次记账之间，该进程实际跑掉的**物理**时间；
- `NICE_0_LOAD` = 1024：nice 0 进程的权重，全体进程的"基准体重"；
- `weight`：本进程的权重，由 nice 值查表得到，约等于 `1024 × 1.25^(-nice)`——
  **nice 每好（大）1 级，权重缩为上一级的约 1/1.25**。

权重表就在仓库里：`linux-2.6.35/kernel/sched.c` 第 1365 行的 `prio_to_weight`
数组（现代内核里改名 `sched_prio_to_weight`），摘几格：

| nice | −20 | −10 | −5 | 0 | +5 | +10 | +19 |
|---|---|---|---|---|---|---|---|
| weight | 88761 | 9548 | 3121 | 1024 | 335 | 110 | 15 |

方向与 0.11 的 `sys_nice()` 一脉相承：nice 越大权重越小，越"不优先"。权重怎么
变成 CPU 份额？**手算一遍就懂**：

> 两个进程抢同一个 CPU：甲 nice 0（weight 1024），乙 weight 4096（按 1.25 倍率
> 约合 nice −5，取 2 的幂便于口算）。假设 CFS 打算在一个调度周期里发下 12 毫秒
> CPU（真实默认值随 CPU 数与系统配置浮动，这里取整数）：
>
> - 甲分到 12 × 1024/(1024+4096) = 12 × 20% = **2.4 毫秒**；
> - 乙分到 12 × 4096/5120 = 12 × 80% = **9.6 毫秒**。
>
> 再把各自分到的时间代回记账公式验证：甲的 vruntime 涨 2.4 × 1024/1024 =
> **2.4**；乙的涨 9.6 × 1024/4096 = **2.4**——物理时间分了 1:4，vruntime 却涨得
> 一样快！CFS 的"公平"从来不是均分物理时间，而是**让所有就绪进程的 vruntime
> 齐头并进**；nice 的全部作用，就是改写"1 毫秒物理时间值多少毫秒 vruntime"
> 的汇率。

挑人规则只有一句话：**谁的 vruntime 最小（最"亏欠"），谁下一个上**。而"找最小"
不用遍历——所有就绪进程按 vruntime 排在一棵**红黑树**里，永远挑**最左节点**
（红黑树的性质保证最左就是最小）。为什么选红黑树？一句话：**把"挑最小"从
0.11 式的 O(n) 全场扫描（6.4.1 节的 `while (--i)`）降为 O(1)**——代价是把计算
挪到"进队/出队"那一刻（插入、删除各 O(log n)），此后每次挑人都只取最左，连树
都不用爬：当前最左节点缓存在 `rb_leftmost` 里。看 `sched_fair.c` 第 377 行：

```c
static struct sched_entity *__pick_next_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *left = cfs_rq->rb_leftmost;

	if (!left)
		return NULL;

	return rb_entry(left, struct sched_entity, run_node);
}
```

红黑树你不陌生——第 11 章读 nginx 时会打开
`nginx-0.1.0/src/core/ngx_rbtree.c`（文件里有逐行中文注释），到时候可以两相对照：
同一个数据结构，nginx 拿它管定时器，内核拿它管公平。

**min_vruntime：防"插队"与防"欠账"的基准线（现代资料）**。每个运行队列还维护
一个**单调递增**的 `min_vruntime`（`update_min_vruntime()`，sched_fair.c 第 304
行），大致取"全场最小 vruntime 与当前进程的较大者"，只增不减。**新进程**和
**睡醒的进程**入场时，vruntime 都以它为起点初始化（`place_entity()`，
sched_fair.c 第 725 行）——这一步同时堵住两个漏洞：

- **新来插队者**：vruntime 若从 0 起算，新进程立刻是全场"最亏欠"，能把 CPU 霸占
  到追平别人才罢休。所以新进程的起点是 `min_vruntime` 再加一笔"起步欠账"
  （`START_DEBIT`，第 736~737 行）——先欠着，和大家站在同一条账线上再跑；
- **久睡者欠账爆炸**：vruntime 若沿用睡前老账，睡一小时的进程醒来会"亏欠"出
  天文数字，反过来独占 CPU 一小时补账。所以睡醒者的起点定在 `min_vruntime`
  略减半个调度周期（`GENTLE_FAIR_SLEEPERS`，第 740~750 行）——只保留一点
  "睡眠积分"当奖赏，横财与欠账一并封顶。

对照着看：0.11 用 `counter/2 + priority` 的遗忘因子手工封顶睡眠奖赏（6.4.3 节），
CFS 用 `min_vruntime` 把同一件事做成了**结构性**的——账本随时间自动前进，谁也
别想靠"睡攒账"或"出生早"钻空子。

CFS 里"时间片"概念被打散了：不再是"固定 15 tick 用完换人"，而是每个时钟 tick
都给当前进程的 vruntime 记账，随时发现自己不再是最左节点就自觉让位——**从
"配额制"变成了"记账制"**。`counter` 的精神还在，只是不再需要"全场清零补发"
这样的手工戏码。

> 本节公式、权重表与 min_vruntime 的说法取自**现代资料**：内核官方中文文档
> docs.kernel.org/translations/zh_CN/scheduler/sched-design-CFS.html（CFS 设计
> 说明）。仓库里的 `linux-2.6.35/kernel/sched_fair.c` 就是文档背后的实现现场。

### 6.5.4 三个时代挑人方式对照表

| | 0.11（1991） | 2.6.0 O(1)（2003） | 2.6.35 CFS（2010） |
|---|---|---|---|
| 挑人标准 | `counter` 最大 | 优先级最高的队头 | `vruntime` 最小 |
| 数据结构 | 数组 `task[64]` | 位图 + 140 条队列 | 红黑树（+最左缓存） |
| 挑人耗时 | O(n) 全场扫描 | O(1) 找位图 | O(1) 取缓存 |
| 记账方式 | 每 tick 扣 counter，用完补发 | 双数组轮换 | 每 tick 记 vruntime |
| 交互友好 | 睡眠攒 counter | sleep_avg 手工打分 | 睡眠 = vruntime 不涨，天然公平 |
| 比喻 | 教练挨个问"你还剩几口气" | 140 个包间看第一个有人坐的 | 谁发言最少谁下一个说 |

三代的共同点比差异更值得记：**都在回答同一个问题——"下一个跑谁"，且都让
"交互式进程优先"从一个手工补丁演化成了结构的自然结果**。

## 6.6 动手实验

以下程序在 macOS 和 Linux 上都可直接编译运行。每做一步，先**预测**输出再运行。
机器核数用 `nproc`（Linux）或 `sysctl -n hw.ncpu`（macOS）查看。

### 实验 6-1：亲眼看时间片轮转

三个子进程同时死循环数数，5 秒后父进程叫它们各自报数。如果调度器在轮转均分，
三个计数应近似相等。

```c
/* exp6-1-rr.c：观察时间片轮转——三个死循环进程分到的 CPU 是否均等 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

volatile unsigned long my_count = 0;  /* 5 秒能数几十亿，用 long 防溢出 */

void report(int sig)                 /* 子进程收到 SIGUSR1：报数并退出 */
{
    printf("  子进程 pid=%d，5 秒数到 %lu\n", getpid(), my_count);
    exit(0);
}

int main(void)
{
    pid_t pid[3];
    int i;

    for (i = 0; i < 3; i++) {
        pid[i] = fork();             /* 第 4 章的 fork：一次调用、两次返回 */
        if (pid[i] == 0) {           /* ── 子进程：装好报数器，死循环数数 ── */
            signal(SIGUSR1, report);
            while (1)
                my_count++;          /* 纯计算：不睡、不打印，全力抢 CPU */
        }
    }
    sleep(5);                        /* 让孩子们抢 5 秒 CPU */
    for (i = 0; i < 3; i++) {
        kill(pid[i], SIGUSR1);       /* 通知报数 */
        wait(NULL);                  /* 收尸（第 4 章） */
    }
    printf("三次计数近似相等 ⇒ CPU 被均分了\n");
    return 0;
}
```

```bash
gcc exp6-1-rr.c -o rr && ./rr
```

一次真实输出（多核测试机）：三个计数约为 6.99 亿、4.32 亿、5.01 亿——同一量级，
却差了百分之几十。**思考两问**：(a) 多核机器上三个进程可能被分到不同核心
**真并行**，甚至落在性能不同的核上（快慢核），计数自然有偏差。想纯粹观察轮转，
把实验 6-2 的"陪跑进程"思路搬过来，先开满所有核再跑；(b) 在占满核的环境里重跑，
三个计数应当几乎相等——那时的相等才是调度器轮转的铁证。

### 实验 6-2：nice 生效——让贤 10 个刻度会怎样

两个计数子进程，一个 `nice(10)` 自愿降低优先级，一个保持默认。先开一批"陪跑"
死循环进程把核占满——**核被占满时，优先级的差异才压不住**。

```c
/* exp6-2-nice.c：nice 值如何改变分到的 CPU 份额 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define NFILL 8        /* 陪跑进程数：请改成 ≥ 你的 CPU 核数 */

volatile unsigned long count = 0;    /* 5 秒能数几十亿，用 long 防溢出 */
volatile long filler = 0;

void report(int sig)
{
    printf("  pid=%d 报数：%lu\n", getpid(), count);
    exit(0);
}

int main(void)
{
    pid_t a, b, fills[NFILL];
    int i;

    for (i = 0; i < NFILL; i++)      /* 陪跑进程：把 CPU 占满，不计成绩 */
        if ((fills[i] = fork()) == 0)
            while (1) filler++;
    if ((a = fork()) == 0) {         /* 计数进程 1：自愿让贤 */
        nice(10);                    /*   nice +10 ⇒ 优先级降低 */
        signal(SIGUSR1, report);
        while (1) count++;
    }
    if ((b = fork()) == 0) {         /* 计数进程 2：普通 nice（0） */
        signal(SIGUSR1, report);
        while (1) count++;
    }
    sleep(5);
    kill(b, SIGUSR1); wait(NULL);    /* 先问礼貌的那个 */
    kill(a, SIGUSR1); wait(NULL);    /* 再问让贤的那个 */
    for (i = 0; i < NFILL; i++) {
        kill(fills[i], SIGKILL);     /* 打扫陪跑进程 */
        wait(NULL);
    }
    return 0;
}
```

```bash
gcc exp6-2-nice.c -o nice-demo && ./nice-demo
```

预期：两个计数**不等**，且 NFILL 开得越足差距越大——占满核的 Linux 上约 9 倍
（CFS 给 nice 0 / nice 10 的权重约为 1024 : 110）；核没占满时差距会缩水（测试机
实测 1.2 倍~5 倍不等，随 NFILL 增大）。回头对照 6.4.3 节 `sys_nice()`——
差距的源头就是 `priority -= increment` 的方向。也可以不改代码直接体验：
`nice -n 10 yes > /dev/null &` 和 `yes > /dev/null &` 各起一个，几秒后用 `top`
比较两个 yes 的 CPU%。

### 实验 6-3：交互优先——"睡得香，起得猛"

睡眠型进程（每毫秒醒来干一下，模拟等用户输入）与计算型进程混跑。如果调度器真的
偏爱交互式进程，混跑时睡眠型的"醒来节奏"几乎不该被拖慢。

```c
/* exp6-3-interactive.c：睡眠型与计算型进程混跑，感受交互优先 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>

#define NWAKE 2000     /* 睡眠型进程醒来 2000 次 */
#define NLOAD 8        /* 计算型陪跑进程数：改成 ≥ 你的核数 */

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    double t0, t1;
    pid_t pid, loads[NLOAD];
    int i, status;

    t0 = now_sec();                    /* ── 第 1 轮：睡眠型独跑 ── */
    if ((pid = fork()) == 0) {
        for (i = 0; i < NWAKE; i++)
            usleep(1000);              /* 睡 1 ms：模拟"等事件" */
        exit(0);
    }
    waitpid(pid, &status, 0);
    t1 = now_sec();
    printf("独跑：醒 %d 次用时 %.2f 秒\n", NWAKE, t1 - t0);
    t0 = now_sec();                    /* ── 第 2 轮：与计算型混跑 ── */
    fflush(stdout);                    /* fork 连 stdio 缓冲区一起复印，先清空！ */
    if ((pid = fork()) == 0) {
        for (i = 0; i < NWAKE; i++)
            usleep(1000);
        exit(0);
    }
    for (i = 0; i < NLOAD; i++)        /* 计算型陪跑：死循环抢 CPU */
        if ((loads[i] = fork()) == 0) {
            volatile double x = 0;
            while (1) x += 1.0;
        }
    waitpid(pid, &status, 0);
    t1 = now_sec();
    printf("混跑：醒 %d 次用时 %.2f 秒（注意：几乎没变慢）\n", NWAKE, t1 - t0);

    for (i = 0; i < NLOAD; i++) {      /* 打扫陪跑进程 */
        kill(loads[i], SIGKILL);
        waitpid(loads[i], NULL, 0);
    }
    return 0;
}
```

```bash
gcc exp6-3-interactive.c -o interactive && ./interactive
```

预期：两行时间都在 2 秒出头（2000 次 × 1ms 睡眠的本底），混跑几乎没有变慢——
这是 6.4.3 节补偿机制的现代回声：睡眠时账面欠账越攒越多，每次醒来都被优先安排。
彩蛋：删掉 `fflush(stdout)` 那行并把输出重定向到文件，"独跑"会被打印**两次**
——fork 把父进程还没刷出去的缓冲区也复印了一份，第 4 章"复印件"的又一铁证。

### 实验 6-4（可选，Linux）：从 /proc 偷看调度记账

Linux 把每个进程的调度统计摊开在 `/proc/<pid>/` 下。开一个睡眠进程：

```bash
sleep 60 &
cat /proc/$!/sched | head -15   # 字段名随内核版本略有差异
grep ctxt /proc/$!/status       # voluntary / nonvoluntary_ctxt_switches
```

`voluntary_ctxt_switches`（自愿切换：主动睡眠、等 I/O）与
`nonvoluntary_ctxt_switches`（非自愿：时间片被掐）正是 6.2 节两种调度时机的
记账。对实验 6-1 的三个计数进程重复第二条命令：nonvoluntary 计数个个上千
——5 秒里被掐断上千次，轮转从未停过。

## 6.7 常见误区

**误区一："多核电脑才需要调度器，我核多，调度器没什么用。"**
反了：**调度器天生是为"进程数 > 核数"而生的**，核越少越离不开它。你的笔记本
同时跑几百个进程，核只有 8~16 个——绝大多数进程的"同时运行"始终是轮换出来的
幻觉。核多只是让幻觉更容易维持，没有消灭轮换。

**误区二："nice 值越大，权重越大，越优先。"**
方向完全相反：nice 是"客气值"，**越大越不优先**。0.11 的 `sys_nice()` 写得
明明白白：`priority -= increment`，nice +10 等于把补发时间片削小，实验 6-2 里的
计数差距就是反方向。现代内核同样如此：`nice 19` 是好公民，`nice -20`
（root 专用）才是 VIP。

**误区三："进程切换是免费的，切得越勤越好。"**
一次切换至少三笔开销：保存/恢复全套寄存器（微秒级）；新进程的代码和数据不在
CPU 缓存里，头几万条指令都在"冷启动"（常常最贵）；流水线里未完成的指令作废。
切得太勤，CPU 沦为"专职换人师"。这就是时间片不能无限调小的原因——0.11 取
150ms，现代内核毫秒级，都是"响应"与"开销"讨价还价的结果。

## 练习题

**6.1（概念）** 用一句话向室友解释：为什么单核电脑能"同时"放音乐和开网页？
再回答：如果时间片从 10ms 改成 1ms 或 500ms，各会出什么问题？
（提示：1ms——切换开销占比飙升，还频繁污染缓存；500ms——音乐开始"打嗝"，
敲键盘有迟钝感。）

**6.2（手算）** 三个就绪进程 A、B、C，`counter` 分别为 5、3、1，`priority` 恰好
也是 5、3、1（fork 时 `counter = priority`）。按 0.11 规则写出**前 4 次调度**
各选中谁，每人每次连跑几个 tick。
（提示：第 2 次调度发生在被选中者把 counter 减到 0 的那个 tick，由 `do_timer`
第 335 行触发；第 4 次调度前全场 counter 都是 0，先走一遍 sched.c 第 138~139 行
的补偿公式再重新扫描。答案序列是 A→B→C→A，然后永远循环——算完想想为什么
这就是"公平"。）

**6.3（读码）** `do_timer()` 里第 334 行 `if (!cpl) return;`——时间片明明用完了
却不换人，什么情况下走这条路？0.11 为什么不敢在内核态中途换人？
（提示：`cpl` 来自 system_call.s 第 191~192 行取的 CS 低 2 位，0 表示内核态。
想想 6.4.5 节的等待链如果切到一半会发生什么——内核代码处处假设"我跑着的时候
没人动这些数据结构"。）

**6.4（读码）** 三个进程 A、B、C 先后调用 `sleep_on(&w)`（A 最先）。画出此刻
`*w` 和三者各自的 `tmp` 指向。`wake_up(&w)` 只被调用一次，A 最终凭什么能醒？
这条链在什么时序下会出错？
（提示：`*w→C`，C.tmp→B，B.tmp→A，A.tmp→NULL。传播靠"上台后顺手叫醒前一个"
（sched.c 第 163~164 行）。出错场景：`wake_up` 恰好插在 `tmp=*p` 与 `*p=current`
之间——第 12 章细究。）

**6.5（实验）** 把实验 6-1 的 `sleep(5)` 分别改成 `sleep(1)` 和 `sleep(20)`，
观察三个计数的比例还稳定吗？再用实验 6-2 的 NFILL 思路把核占满重跑一遍，
写下观察结果，并回答：多核机器上"计数相等"能证明"轮转均分"吗？
（提示：1 秒时比例波动大——启动误差占比高；占满核之后，相等才真正出自调度器的
轮换。区分"真并行"与"模拟并行"是本题的全部意义。）

## 本章小结

1. 调度器回答的问题只有一句："下一个跑谁"。公平、效率、响应快三难不可兼得，
   不同场景各取一头：批处理要吞吐、交互要响应、实时要 deadline。
2. 调度时机只有两个，最终都汇入 `schedule()`：进程主动睡/退（`sleep_on`、
   `do_exit`），或时钟中断发现时间片用尽（`do_timer` → `schedule`）。
3. 0.11 挑人规则：`TASK_RUNNING` 且 `counter` 最大者上；全场余额为 0 就先按
   `counter = counter/2 + priority` 补发再重扫；无人可跑时交给 idle 任务
   `task[0]` 兜底。
4. 1 tick = 10ms（`HZ 100`）。`counter` 初值 = `priority`（默认 15，即 150ms），
   每 tick 只扣 `current` 一人，扣到 0 封顶。
5. 补发公式是"老化"的漂亮变体：睡眠者每轮白拿 `counter/2 + 15`，序列
   15→22→26→28→29 后停在 29（理论不动点是 2×priority=30，整数减半使它停在 29）
   ——醒来的瞬间全场最大，"睡得香，起得猛"，交互优先不用一行专门代码就自己
   长了出来。
6. `sys_nice()`：nice 越大 `priority` 越小。它同时决定新进程的 counter 初值
   （fork 时 `counter = priority`）与每轮补发额度——从出生到回血两头一起压。
7. 睡眠 = 改状态 + `schedule()`。`TASK_UNINTERRUPTIBLE` 深觉信号叫不醒，
   `TASK_INTERRUPTIBLE` 浅觉可被信号叫醒（`schedule()` 开头第 117~119 行负责叫）；
   `sleep_on` 用队头指针串成隐式等待链，`wake_up` 只叫队头，唤醒沿链传播；
   `tmp=*p` 与 `*p=current` 之间有一个著名竞态窗口。
8. O(1) 调度器：`prio_array` 用 140 位位图 + 140 条优先级队列，`sched_find_first_bit`
   （`__ffs`）恒定时间挑人；active/expired 双数组指针一换，就完成一轮"补发"。
9. CFS：按 `vruntime += delta_exec × 1024 / weight` 记账，权重由 nice 查表
   （约 `1024 × 1.25^(-nice)`，nice −20→88761、+19→15），挑 vruntime 最小者上；
   红黑树最左节点加缓存，让挑人 O(1)、插删 O(log n)。
10. `min_vruntime` 单调递增，新进程与睡醒者都以它为起点——同时堵住"新进程
    插队"与"久睡者欠账爆炸"两个漏洞，0.11 的遗忘因子升级成了结构性保障。
11. 0.11 内核不可抢占：`do_timer` 发现中断前在内核态（`!cpl`）就不换人，切换
    只发生在即将返回用户态的时刻。
12. 三代调度器同答一题：挑人标准从"counter 最大"到"优先级位图"再到"vruntime
    最小"，交互友好从手工补丁演化成数据结构的自然结果。

## 自测清单

全部能独立完成，再进第 7 章：

- [ ] 不看书写出 0.11 挑人的核心判断
      （`(*p)->state == TASK_RUNNING && (*p)->counter > c`），并说清 `next = 0`
      留给谁、为什么；
- [ ] 向室友解释"睡得香，起得猛"是哪两行代码合谋的结果，遗忘因子 `>>1` 防什么；
- [ ] 说清 nice 的方向（越大越客气），以及它影响 counter 的两条路径
      （fork 初值与每轮补发额度）；
- [ ] 给三个场景——等磁盘块到位、等键盘敲下、等 `alarm()` 闹钟——说出各该睡
      深觉（`TASK_UNINTERRUPTIBLE`）还是浅觉（`TASK_INTERRUPTIBLE`），为什么；
- [ ] 说出 O(1) 调度器把挑人降为恒定时间的两个数据结构（位图 + 140 条队列），
      以及 active/expired 换指针如何完成一轮"补发"；
- [ ] 用 CFS 公式手算：weight 1024 与 weight 4096 两进程分一个 12ms 周期，
      各跑几毫秒、各自的 vruntime 涨多少；
- [ ] 解释 `min_vruntime` 如何同时防住"新进程插队"和"久睡者欠账爆炸"；
- [ ] 说出三代调度器各自的挑人标准、数据结构与复杂度
      （O(n) 全场扫描 / O(1) 位图 / O(1) 红黑树取最左）。

## 延伸阅读

- 《Linux内核完全注释 v5.0》（本仓库 `doc/` 目录）进程调度相关章节——对
  `schedule()`、`sleep_on()`、时钟中断与 `counter` 的逐行注释，本章最佳配套；
- 《操作系统导论（OSTEP）》第 7~9 章"调度"——FIFO、SJF、MLFQ 到彩票调度的
  现代讲法，与本章 6.3 节互为印证；
- `linux-2.6.35/Documentation/scheduler/sched-design-CFS.txt`——CFS 作者留下的
  设计文档，读懂 6.5.3 后你能看懂它的前半篇；
- CFS 设计文档官方中文版（现代资料）：
  docs.kernel.org/translations/zh_CN/scheduler/sched-design-CFS.html——
  6.5.3 节记账公式、权重表与 min_vruntime 的出处；
- Ingo Molnar 的 O(1) 调度器设计邮件（网上搜 "O(1) scheduler design"）——
  看 2003 年的内核黑客如何论证"位图 + 队列"胜过"全场扫描"。

[← 第 5 章](ch05-中断与系统调用.md) | [第 7 章 →](ch07-内存管理.md)
