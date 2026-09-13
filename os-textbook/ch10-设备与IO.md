# 第 10 章 设备与 I/O：内核伸向硬件世界的手

> **学习目标**
> 1. 能说出三种 I/O 方式（轮询、中断驱动、DMA）的区别，并用餐厅的比喻讲清楚；
> 2. 理解"一切皆文件"：设备文件、主/次设备号、字符设备与块设备；
> 3. 能顺着 `bread()` 讲完一次磁盘读：请求排队 → 端口编程 → 中断收货 → 唤醒进程；
> 4. 能讲完一次按键：扫描码 → 字符表 → 行规程 → 显存 0xB8000；
> 5. 知道"中断上半部/下半部"为什么要分两半，并在自己的终端上亲眼验证行缓冲。

---

## 10.1 从一个问题开始：printf 输出 1 字节，要多久？

就一行 `printf("A");`，能花多少时间？我们分三段算：

| 段 | 发生了什么 | 量级（现代机器） |
|---|---|---|
| CPU 执行一条指令 | 加法、跳转 | 亚纳秒（< 1 ns） |
| `printf("A")` 全程 | C 库缓冲 → `write()` 系统调用进内核 → 终端驱动 → 写显存 | 约 1 微秒（µs） |
| 磁盘读一个数据块 | 磁头寻道 + 盘片旋转 + 传输 | 约几毫秒（ms） |

看出来了吗？**I/O 比计算慢三到六个数量级**。于是操作系统这门手艺的核心难题
来了：**CPU 这么快，设备这么慢，等的时候 CPU 干什么？** 最笨的答案是
"原地傻等"（轮询），我们来算算傻等的代价。

1991 年的主流外存是软盘。一张 1.44 MB 软盘约合 1.5×10⁶ 字节，持续传输速率
约 250 kbit/s（约 31 KB/s）。如果 CPU 用轮询方式读完整张盘：

> **1.5×10⁶ 字节 ÷ 31×10³ 字节/秒 ≈ 47 秒。** 这 47 秒里 CPU 一圈一圈地查
> "好了没？好了没？"，什么正事都干不了，你的 shell、别的进程全部冻结——
> 就读个软盘而已。

这还只是软盘。内核必须提供更聪明的等法。本章的主角——`kernel/blk_drv/`
（块设备驱动）与 `kernel/chr_drv/`（字符设备驱动）——就是 0.11 给出的答案。

## 10.2 核心概念 I：三种 I/O 方式

把 CPU 想成餐厅服务员，设备想成后厨：

| 方式 | CPU 在做什么 | 比喻 | 0.11 里谁在用 |
|---|---|---|---|
| **轮询**（programmed I/O） | 反复读设备状态寄存器，直到数据就绪 | 服务员站在厨房门口，死死盯着："好了吗？好了吗？" | 罕见（hd.c 写盘前等一个状态位，最多 3000 次） |
| **中断驱动** | 下达命令后去干别的；设备完成时发**中断**喊 CPU 回来 | 服务员点完单就去招呼别的桌，后厨好了按铃 | **主力**：硬盘（`read_intr`）、键盘、串口 |
| **DMA**（直接内存访问） | 下达命令后连数据搬运都不管，DMA 芯片直接把数据搬进内存，完成时才中断一次 | 外包配送：菜由配送员直接送到客人桌上，服务员只在签收时露面 | 软盘（`floppy.c` 的 `setup_DMA`） |

三种方式，成本一条比一条低：轮询 CPU 全程被占；中断驱动 CPU 只在每块数据
到达时被占用；DMA 只在整批完成时打扰一次。

> **一个如实的说明**：0.11 的硬盘驱动其实是"中断驱动 + 程序搬运"——数据到来
> 时由 CPU 亲手从控制器端口搬进内存（`read_intr` 里的 `port_read`），没有用
> DMA；反倒是不起眼的软盘驱动用了 DMA（`floppy.c` 的 `setup_DMA`）。技术史
> 从来不是越新的设备越先进。

## 10.3 核心概念 II：Linux 的设备观——一切皆文件

第 9 章说过"一切皆文件"。设备怎么"皆文件"？答案是**设备文件**：
`/dev` 目录下的那些文件不是数据的容器，而是**设备的门牌**。你 `open` 它，
内核就把它路由到对应的驱动程序。

### 10.3.1 主设备号 + 次设备号：两个数字找到驱动

打开 `linux-0.11/include/linux/fs.h` 第 11~34 行：

```c
/* devices are as follows: (same as minix, so we can use the minix
 * file system. These are major numbers.)
 *
 * 0 - unused (nodev)
 * 1 - /dev/mem
 ...
 * 6 - /dev/lp
 * 7 - unnamed pipes
 */
...
#define MAJOR(a) (((unsigned)(a))>>8)
#define MINOR(a) ((a)&0xff)
```

这就是 0.11 全部的设备户口本：**主设备号（major）指定哪一类驱动，次设备号
（minor）指定这一类里的哪一个**。编码规则藏在宏里——设备号是 16 位数，高
8 位是主、低 8 位是次：`0x0301` = 主 3 次 1 = 第一块硬盘上的第一个分区；
`ramdisk.c` 末尾把根设备改成 `0x0101` = 内存盘。（注释漏了一个：`ramdisk.c`
第 17 行 `#define MAJOR_NR 1`，内存盘也在主 1 下面。）

这些数字存在哪？就在 inode 里。普通文件的 inode 用 `i_zone[9]` 记数据块号；
设备文件的 inode 只在 `i_zone[0]` 里记一个设备号——`fs/read_write.c` 第 69 行
`rw_char(READ, inode->i_zone[0], ...)` 就是证据，所以设备文件**不占任何数据
块**。用户态建设备文件只需一条命令：`sudo mknod /dev/hd0 b 3 0`（`b` = 块
设备，主 3 次 0）。名字随便起——名字只是门牌，主/次设备号才是地址。

### 10.3.2 字符设备 vs 块设备

| | 字符设备（chr_drv） | 块设备（blk_drv） |
|---|---|---|
| 数据形态 | **字节流**，像水龙头，来了就喝 | **定长块**（0.11 里 1 KB），像搬砖，一摞一摞 |
| 典型成员 | 键盘、串口、打印机、（控制台） | 硬盘、软盘、内存盘 |
| 随机访问 | 通常不行（键盘不能"倒回去重敲"） | 行（跳到任意块去读） |
| 缓存 | 一般不缓存 | 走缓冲区缓存（第 9 章 buffer.c） |
| 0.11 主设备号 | 4（ttyx）、5（tty） | 1（ramdisk）、2（fd）、3（hd） |

为什么区分？因为用法天差地别：键盘读一个字节就够，磁盘一次读 1 KB 才划算；
磁盘读过的块很可能马上又被读（第 9 章的缓冲区缓存），键盘输入没人想缓存。
**驱动按数据形态分类，而非按设备大小分类。**

## 10.4 源码导读一：块设备——一次 bread 的完整旅程

第 9 章我们读到 `bread()` 就停下了："缓冲区不在缓存里，就去读盘"。把后半程走完，整条链路是：

```
进程A: bread(dev, block)                     fs/buffer.c:267
   │ 缓存未命中
   ▼
ll_rw_block ──► make_request()：填"请求单"插进队列   ll_rw_blk.c:145/88
   ▼
do_hd_request(): 把请求翻译成控制器端口操作    hd.c:294
   ├──► CPU 返回，去跑别的进程（第 6 章的调度器接管）
磁盘转好，一扇区就绪 ──► 中断 0x2E ──► read_intr()  hd.c:250
   │                     （搬运数据，512 字节）
   ▼
end_request(): 解锁缓冲区、唤醒所有人          blk.h:109
   ▼
进程A 从 bread() 里醒来，拿着数据继续跑
```

### 10.4.1 出发点：buffer.c 的 bread

```c
struct buffer_head * bread(int dev,int block)      /* fs/buffer.c:267 */
{
	if (!(bh=getblk(dev,block)))       /* 在缓存里找/腾一个缓冲区 */
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)                /* 数据已是最新：直接用，根本不碰硬件 */
		return bh;
	ll_rw_block(READ,bh);              /* 否则：委托块设备层去读 */
	wait_on_buffer(bh);                /* 睡，直到中断来唤醒 */
	...
}
```

注意 `wait_on_buffer(bh)`：进程在这里**睡去**（第 4 章的 `sleep_on` 家族）。
谁叫醒它？往下看。

### 10.4.2 排队：ll_rw_blk.c 的请求队列

`ll_rw_block()` 只有一件事：把缓冲区交给 `make_request()` 排队。队列在
`ll_rw_blk.c` 第 21 行：`struct request request[NR_REQUEST];`——
`NR_REQUEST = 32`（`blk.h` 第 15 行），**全内核就 32 张请求单**。一张请求单
（`struct request`）记录：设备号、读还是写（`cmd`）、起始扇区（`sector`）、
扇区数（`nr_sectors`）、搬到哪块内存（`buffer`）、对应哪个缓冲区（`bh`），
再用 `next` 串成一串。

`make_request()`（第 88 行）填单的这几行值得划重点：

```c
	req->sector = bh->b_blocknr<<1;    /* 1 块 = 1024 字节 = 2 扇区(512B) */
	req->nr_sectors = 2;
	req->buffer = bh->b_data;
```

文件系统按 1 KB 访问，硬盘按 512 字节的扇区搬货，块号左移一位变扇区号——两层"语言"之间的翻译。

排队时有个精妙的公平设计（第 115~118 行）：**写请求只许用队列的前 2/3，
后 1/3 永远留给读**——等读的进程通常正卡在那儿干不了活，读比写急。队列满
了怎么办？`sleep_on(&wait_for_request)`，睡到有人腾出请求单。

`add_request()`（第 64 行）负责插入。如果当前没有正在进行的请求，就把
`dev->current_request` 指向它（这就是"当前请求"），立刻调用派活函数
`request_fn` 开工；否则按 `IN_ORDER`（`blk.h` 第 40 行）插队——这就是
**电梯算法**的雏形：像电梯不来回乱跑，磁头扫过去把沿路扇区的请求都办了；
同类请求按扇区号升序，读永远排在写前面。

### 10.4.3 派活与收货：hd.c 的端口编程

每个块设备驱动要向块设备层登记一个"派活函数"。硬盘驱动 `hd.c` 第 25 行
`#define MAJOR_NR 3`，第 345 行完成登记：

```c
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;  /* = do_hd_request */
	set_intr_gate(0x2E,&hd_interrupt);              /* 中断向量 0x2E = IRQ14 */
```

`do_hd_request()`（第 294 行）把"读第 N 扇区"翻译成对控制器端口的**一串
`outb`**（端口定义在 `include/linux/hdreg.h`：数据 0x1f0、扇区号 0x1f3、
柱面 0x1f4/0x1f5、驱动器/磁头 0x1f6、命令/状态 0x1f7）。按下命令时，
把收货函数一并登记：

```c
	if (CURRENT->cmd == WRITE)
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
	else if (CURRENT->cmd == READ)
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
```

`hd_out()`（第 180 行）先 `do_hd = intr_addr`——把"中断来了以后该调谁"
记到一个全局函数指针上——再连写 7 个端口发出命令。然后呢？**然后函数返回，
CPU 走了**，磁盘在转，CPU 在跑你的 shell——这就是 10.2 节说的中断驱动。
数据就绪时，中断门（`kernel/system_call.s` 第 221 行）把控制权交回来，
它有个绝妙的间接跳转：

```asm
	xchgl _do_hd,%edx        # 原子地取出（并清零）"该调谁"
	...
	call *%edx		# "interesting" way of handling intr.（Linus 原话）
```

调到的就是当初登记的 `read_intr()`（`hd.c` 第 250 行）——中断收货员。原文
一共 18 行，正好演完一遍完整的收货流程：

```c
static void read_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	port_read(HD_DATA,CURRENT->buffer,256);
	CURRENT->errors = 0;
	CURRENT->buffer += 512;
	CURRENT->sector++;
	if (--CURRENT->nr_sectors) {
		do_hd = &read_intr;
		return;
	}
	end_request(1);
	do_hd_request();
}
```

对着原文走一遍循环的五步：

1. **验收**：`win_result()`（第 168 行）读状态寄存器 0x1f7 检查出错位；出错
   就交给 `bad_rw_intr()` 记账（错满 `MAX_ERRORS` 次放弃本请求，错到一半就
   先复位控制器再战），随后调 `do_hd_request()` 转向队列里的下一个请求。
2. **搬数据**：`port_read(HD_DATA,CURRENT->buffer,256)`——从数据端口 0x1f0
   连取 256 个 16 位字，正好一扇区 512 字节，CPU 亲手搬进请求单指定的内存。
3. **advance**：`CURRENT->buffer += 512; CURRENT->sector++;`——"下一扇区从
   哪搬、搬到哪"，两个指针各前进一步。
4. **预告下一次中断**：`if (--CURRENT->nr_sectors)` 不为零，说明这个 1 KB
   缓冲块还欠一扇区，于是 `do_hd = &read_intr;`，函数随即返回——**函数指针
   又指回了自己**。等下一扇区就绪，`_hd_interrupt` 照旧 `xchgl` 取出 `do_hd`
   间接调用，进来的还是 `read_intr`。这正是附录 B 招式 4 的函数指针分派，
   只是连表都省了：**每个进行中的请求，自带它的下一个中断处理程序**。
5. **完工**：两扇区齐了，`end_request(1)` 解锁缓冲区、唤醒睡着的进程（10.4.4），
   再顺手调 `do_hd_request()` 开工下一个请求。

写方向的 `write_intr()`（第 269~285 行）是它的镜像——读搬完才预约下一次；
写是供给方，先把 `do_hd` 预约好、指针推进去，再 `port_write` 出 512 字节：

```c
static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		do_hd = &write_intr;
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);
	do_hd_request();
}
```

读和写就这样一扇区一中断地循环：**收货 → 验收 → 搬运 → advance → 完工，
或者预告下一次**。读 1 KB 挨两次中断，但内核里没有一行在"等磁盘"。

### 10.4.4 闭环：end_request 把所有人叫醒

`end_request()`（`blk.h` 第 109 行）是这次旅程的收官，它一口气唤醒三拨人：

```c
	CURRENT->bh->b_uptodate = uptodate;   /* 数据有效 */
	unlock_buffer(CURRENT->bh);           /* → wake_up(&bh->b_wait)：叫醒 bread 里睡的进程A */
	wake_up(&CURRENT->waiting);           /* 叫醒直接等请求的进程（分页用） */
	wake_up(&wait_for_request);           /* 叫醒等空闲请求单的进程 */
	CURRENT = CURRENT->next;              /* 队列往后走 */
```

还记得 10.4.1 里在 `wait_on_buffer` 睡着的进程 A 吗？就是这里醒的。
**系统调用发起 → 请求排队 → CPU 干别的事 → 中断搬运数据 → 唤醒进程**——
第 4、5、9 章的三条线索，在这一刻接上了头。

### 10.4.5 特殊选手：ramdisk.c——内存当磁盘

`kernel/blk_drv/ramdisk.c` 开头写着 `Written by Theodore Ts'o, 12/2/91`——
这是内核历史上第一份由 Linus 之外的人提交的代码，作者就是日后内核圈元老
"tytso"。内存盘把一块内存假装成磁盘，它的派活函数 `do_rd_request` 全部
核心就是一行 memcpy：

```c
	addr = rd_start + (CURRENT->sector << 9);
	len = CURRENT->nr_sectors << 9;
	if (CURRENT-> cmd == WRITE) {
		(void ) memcpy(addr, CURRENT->buffer, len);   /* 就是 memcpy！ */
	} else if (CURRENT->cmd == READ) {
		(void) memcpy(CURRENT->buffer, addr, len);
	}
	end_request(1);
```

没有端口、没有中断、没有等待——`memcpy` 是纳秒级的。对照 `read_intr`
那一套，你会瞬间明白：**驱动的全部复杂性都来自设备的慢**。

它的大小从哪来？`init/main.c` 第 123~124 行：`#ifdef RAMDISK` 时执行
`main_memory_start += rd_init(main_memory_start, RAMDISK*1024);`——
编译时打开 `RAMDISK` 宏（单位 KB），启动时从内存划出一块。`rd_load()` 还会
检查软盘上有没有藏文件系统镜像，有就整卷搬进内存盘，把根设备改成 `0x0101`
——"装系统不碰盘"的老祖先。

## 10.5 源码导读二：字符设备——你敲下的那个"a"去哪了

从按下键盘到屏幕上出现"a"，要经过 keyboard.S → tty_io.c → console.c 三站
（外加可选的串口）。先看总管。

### 10.5.1 总管：tty_io.c 的 tty_table

`kernel/chr_drv/tty_io.c` 第 51 行：

```c
struct tty_struct tty_table[] = {
	{ {ICRNL, OPOST|ONLCR, 0,
	   ISIG | ICANON | ECHO | ECHOCTL | ECHOKE,   /* ← 行规程的开关全在这 */
	   0, INIT_C_CC},
	  0, 0,
	  con_write,                     /* 0 号：控制台，写用 con_write */
	  {0,0,0,0,""},                  /* read_q：原始字节队列 */
	  {0,0,0,0,""},                  /* write_q：待输出队列 */
	  {0,0,0,0,""}                   /* secondary：加工好的行 */
	},
	{ ... rs_write, {0x3f8,...} },   /* 1 号：串口1（COM1） */
	{ ... rs_write, {0x2f8,...} }    /* 2 号：串口2（COM2） */
};
```

0.11 一共三个 tty：0 号控制台，1、2 号串口。每个 tty 有**三个队列**（每个
1024 字节，见 `include/linux/tty.h` 的 `TTY_BUF_SIZE`）：原始输入 `read_q`、
输出 `write_q`、加工后的行 `secondary`。

### 10.5.2 行规程：copy_to_cooked

`ICANON`（行模式）、`ECHO`（回显）、`ISIG`（信号）这些开关背后，是 Unix 的
**行规程（line discipline）**：把原始字节"烹制"成行——0.11 里就是
`copy_to_cooked()`（`tty_io.c` 第 145 行），原文开头：

```c
void copy_to_cooked(struct tty_struct * tty)
{
	signed char c;

	while (!EMPTY(tty->read_q) && !FULL(tty->secondary)) {
		GETCH(tty->read_q,c);
		if (c==13)
			if (I_CRNL(tty))
				c=10;
			else if (I_NOCR(tty))
				continue;
			else ;
		else if (c==10 && I_NLCR(tty))
			c=13;
		if (I_UCLC(tty))
			c=tolower(c);
```

看这门"烹制"手艺的头两道工序：

- **回车转 LF**：`c==13` 且 `I_CRNL` 开着（`tty_table` 初始化恰好填了
  `ICRNL`），CR 就翻译成 LF=10——回车键发来的是 0x0D，你程序里读到的却是
  `'\n'`，就是在这里换的货；
- **退格与杀行**：接着的 `L_CANON` 块里，`ERASE_CHAR`（退格）与 `KILL_CHAR`
  （Ctrl-U 杀行）两分支几乎逐行对称——从 `secondary` 弹出要抹的字节，每弹
  一个就往 `write_q` 塞 127（DEL）抹屏，控制字符占两列所以多抹一个。**屏幕
  显示的字和程序收到的字，从此是两回事**——加工全发生在行规程里。

循环的后半段还有信号、行账本与回显（原文接 `L_ISIG` 分支起）：

```c
		if (c==10 || c==EOF_CHAR(tty))
			tty->secondary.data++;
		if (L_ECHO(tty)) {
			if (c==10) {
				PUTCH(10,tty->write_q);
				PUTCH(13,tty->write_q);
			} else if (c<32) {
				if (L_ECHOCTL(tty)) {
					PUTCH('^',tty->write_q);
					PUTCH(c+64,tty->write_q);
				}
			} else
				PUTCH(c,tty->write_q);
			tty->write(tty);
		}
		PUTCH(c,tty->secondary);
	}
	wake_up(&tty->secondary.proc_list);
```

- **信号**：`L_ISIG` 开着时若字节是 `INTR_CHAR`（Ctrl-C），`tty_intr` 给前台
  进程组发 SIGINT——0x03 这个字节根本进不了队列，直接变成了信号（第 8 章）；
- **行账本**：见到 LF 或 EOF 就 `secondary.data++`——"有一行齐了"的记账；
- **回显**：LF 回显成 CRLF（不然只挪行不回车）、控制字符回显成 `^X`，再由
  `tty->write(tty)` 当场送 `con_write` 上屏——**回显是行规程替你画的**；
- 最后 `PUTCH(c,tty->secondary)` 记入正账；队列加工完，`wake_up` 叫醒读者。

所以**你还没按回车，程序就已经收到了你的每个键**——只是行规程把它们扣在
`secondary` 队列里，顺手回显、顺手处理退格和 Ctrl-C。按了回车，`data` 标记
置位，读的人才被放行。实验 10-3 会让你亲眼看到这一切。

### 10.5.3 睡与醒：tty_read / tty_write

用户进程 `read(0, buf, n)` 一路走到 `tty_read()`（第 230 行），关键就一处：

```c
		if (EMPTY(tty->secondary) || (L_CANON(tty) &&
		!tty->secondary.data && LEFT(tty->secondary)>20)) {
			sleep_if_empty(&tty->secondary);   /* 没有完整的行？睡！ */
			continue;
		}
```

`sleep_if_empty` 用的是 `interruptible_sleep_on(&queue->proc_list)`——
**可中断睡眠**：既能被键盘唤醒，也能被 Ctrl-C 的 SIGINT 打断。于是闭环形成：
`tty_read` 睡 → 键盘中断把字节送进队列 → `copy_to_cooked` 收尾时 `wake_up` →
`tty_read` 醒来取走数据。和 10.4.4 的磁盘路径如出一辙，**只是唤醒人从硬盘
中断换成了键盘中断**。

写方向 `tty_write()`（第 290 行）反过来：进程把字节填进 `write_q`，
调 `tty->write(tty)` 交给设备；队列满就 `schedule()` 让出 CPU。

### 10.5.4 画字：console.c 与 0xB8000

`tty_table[0].write = con_write`。`kernel/chr_drv/console.c` 第 445 行，
核心就是往显存写一个 16 位数：

```c
	while (nr--) {
		GETCH(tty->write_q,c);
		switch(state) {
			case 0:
				if (c>31 && c<127) {
					__asm__("movb _attr,%%ah\n\t"   /* 属性字节(颜色)进高8位 */
						"movw %%ax,%1\n\t"       /* 16 位整体写进显存 */
						::"a" (c),"m" (*(short *)pos)
						:"ax");
					pos += 2;
					x++;
				} else if (c==27)
					state=1;              /* 转义序列：ESC */
				...                          /* 换行/退格/制表另分支 */
```

显存从哪开始？`con_init()` 第 649 行：彩显卡文本模式 `video_mem_start = 0xb8000`
（单色卡 0xb0000）。PC 的约定：**从 0xB8000 起，每 2 字节画一个字符**——低
字节是字符码，高字节是属性（默认 `attr=0x07`，黑底白字）。"往屏幕写一个字符"
物理上就是一次 16 位内存写，比任何外设都快。光标由 `set_cursor()`（第 313 行）
把偏移量折算后写到显卡的索引端口 0x3d4/0x3d5。换行、退格、ANSI 转义序列
（`ESC [ ...` 状态机）也都在这个文件里。

### 10.5.5 上半场：keyboard.S 从扫描码到字符表

字符从哪进来？`kernel/chr_drv/keyboard.S` 第 37 行的 `_keyboard_interrupt`，
真正的中断第一线。入口先压栈保现场、把数据段换成内核段 0x10（被中断的可能
是任何进程，呼应第 5 章），然后直奔主题——原文第 47~53 行：

```asm
	xorl %al,%al		/* %eax is scan code */
	inb $0x60,%al
	cmpb $0xe0,%al
	je set_e0
	cmpb $0xe1,%al
	je set_e1
	call key_table(,%eax,4)
```

`inb $0x60,%al` 从键盘控制器的 0x60 端口**读走扫描码**（0xE0、0xE1 是扩展
键前缀，先摘出去单独记账）；`call key_table(,%eax,4)` 按扫描码**查函数指针
表分派**（`key_table` 就定义在本文件第 502 行，每项 4 字节，正合 `%eax*4`
的寻址），普通按键都进 `do_self`——第 5 章和附录 B 招式 4 的查表分派，硬件
给编号，表负责翻译成语义。函数末尾的"收尾"代码，10.7 节单独讲。

普通按键走 `do_self`（第 453 行）：`lea key_map,%ebx`——**扫描码 → 字符
靠查表**。第 295 行起是美式键盘表：

```asm
key_map:
	.byte 0,27
	.ascii "1234567890-="
	.byte 127,9
	.ascii "qwertyuiop[]"
	...
```

硬件给你的从来不是"键盘左上角第二个键"，而是叫扫描码的编号（1=ESC，2=1，
30=a……）；按下 shift 走 `shift_map`，Alt 走 `alt_map`。查出的字符经
`put_queue`（第 88 行）塞进控制台的 `read_q`，有人排队等输入就顺手置其可
运行。最后 `call _do_tty_interrupt` 进入行规程——汇编干完了，交给 C。（键盘
中断向量 0x21 注册在 `console.c` 第 683 行：`set_trap_gate(0x21,&keyboard_interrupt)`。）

### 10.5.6 一句话：serial.c / rs_io.s——串口与"远程终端"

`tty_table[1]、[2]` 那两个串口干嘛用？1991 年没有图形界面，机房里的做法是：
**主机拖几根串口线（COM1=0x3f8，COM2=0x2f8），每根接一台"哑终端"**——只有
屏幕和键盘的盒子，几个人就这样共用一台 Unix 主机。`serial.c` 第 37 行
`rs_init()` 注册两个中断（0x24/0x23）并把波特率设成 2400，`rs_io.s` 里的
`rs1_interrupt/rs2_interrupt` 在 UART 端口和 tty 队列之间搬字节。0.11 没有
网卡和网络栈——串口是它与外界互通的唯一"网线"。

## 10.6 中断处理的两半：快收数，慢处理

把前面两节并排看，会发现同一种节奏反复出现：

| | 快的一段（中断里） | 慢的一段（中断外） |
|---|---|---|
| 键盘 | `keyboard.S`：读 0x60、查表、入队、发 EOI | `copy_to_cooked` + 等待中的进程被唤醒后再慢慢消费 |
| 硬盘 | `read_intr`：把 512 字节搬进缓冲区，`end_request` | 唤醒的进程自己处理数据 |
| 串口 | `rs*_interrupt`：在端口与队列间搬字节 | 同上 |

为什么不分一步做完？因为**中断服务程序运行时，同级中断通常是关的**：在键盘
中断里慢吞吞处理一整行输入，敲键就可能丢。所以定下铁律：

> **上半部（中断处理程序）只做必须立刻做的事——收数据、答谢硬件；
> 一切能拖的活推到下半部，让进程或专门的机制慢慢处理。**

0.11 的"下半部"很朴素：唤醒进程，让它在调度器安排下继续。现代内核把它机制化
了——`softirq`（`linux-2.6.35/kernel/softirq.c`）和 `tasklet`（`interrupt.h`
第 450 行的 `tasklet_struct`）：中断收完数，登记一个"待办的下半部"，中断返回
前顺手执行；网络收包（NET_RX_SOFTIRQ）就是这么干的。术语在进化，思想没变。

## 10.7 中断收尾：EOI 与 8259A

10.6 的铁律说上半部要"答谢硬件"。答谢什么？PC 机的硬件中断都经 **8259A
中断控制器**转发，它有条规矩：**处理完一个中断，CPU 必须回一个 EOI（End
Of Interrupt）——把命令字节 0x20 写进 0x20 端口（主片）或 0xA0（从片）**。
不回话，控制器就认为中断还在处理中，同级及优先级更低的中断不再放行。

keyboard.S 怎么收尾？第 66~67 行就两句：`movb $0x20,%al`、`outb %al,$0x20`
——把命令字节 0x20 写进主片端口 0x20，答谢完才进 C 世界。那硬盘呢？翻开
`hd.c` 通篇找 `outb ... $0x20`——找不到：EOI 不在 C 驱动里，而在汇编入口
`_hd_interrupt`（`kernel/system_call.s` 第 233 行起）。硬盘是 IRQ14，挂在
**从片**上，要答谢**两次**：

```asm
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1
	jmp 1f			# give port chance to breathe
1:	jmp 1f
```

三个细节：先谢**从片**——原注释写 "#1"，其实 0xA0 是第二片（大师的注释也
会打盹）；第 242 行还有一句 `outb %al,$0x20` 谢**主片**，%al 里装的还是
第 233 行那个 0x20，顺路正好用上；那串 `jmp 1f` 是给 I/O 端口"喘口气"的
老式延时。再往下，就是 10.4.3 讲过的 `xchgl _do_hd,%edx` 和 `call *%edx`。

为什么谢两次？PC/AT 用**两片 8259A 级联**：主片管 IRQ0~7（时钟、键盘），
从片管 IRQ8~15（硬盘 IRQ14、协处理器 IRQ13），从片的输出接进主片的 IRQ2
引脚——15 路外部中断，代价是牺牲一条 IRQ2。向量号 = **0x20 + IRQ 号**：
键盘 0x21、串口 0x24/0x23、硬盘 0x2E，10.4.3 与 10.5.6 注册的向量号全对上。

忘了发 EOI 会怎样？8259A 内部的"在服务"位一直不清，这条中断线永远占线——
好比话务员转接完电话忘了挂回听筒，新来电再也进不来。现象是**第一个键之后
键盘彻底哑掉**，优先级更低的设备也一并遭殃。这是写驱动最经典的低级 bug
之一，0.11 把答案写在了每段汇编入口的固定位置。

## 10.8 一次按键的完整旅程（全章总串联）

把本章所有主角请到一张图里——从手指落到键帽，到 shell 醒来拿字符：

```
你的手指按下"a"　　键盘里的单片机把"a"编码成扫描码 0x1E 送出
   ▼
键盘控制器 8042        收下扫描码，拉高 IRQ1 引线
   ▼
8259A 主片             向 CPU 的 INTR 引脚举手，获准后送出向量号 0x21（10.7）
   ▼
CPU 查 IDT[0x21]       中断门进内核 → keyboard_interrupt（第 5 章）
   ▼
keyboard.S 上半部      inb 0x60 取扫描码 → key_table 查表得 "a"（10.5.5）
                       put_queue 入 read_q → 发 EOI → iret（10.7）
   ▼
copy_to_cooked 下半部  回显（write_q → con_write → 0xB8000，10.5.4）、
                       回车转 LF、退格抹字；凑齐一行才 data++ 记账（10.5.2）
   ▼
wake_up(secondary)     tty_read 里睡着的 shell 醒来——第 4 章的睡、
                       第 6 章的调度；第 8 章的管道、信号是同款机制
   ▼
read() 返回 "a\n"      shell 拿到一行命令，fork/exec 开工——
                       第 1.5 节"一个回车键的旅程"从这里重新接棒
```

按下的是 Ctrl-C，旅程在下半部改道：字节变信号（第 8 章）。是磁盘扇区就绪，
同一骨架换演员重演：8259A 从片 → 0x2E → `read_intr` 搬运 → `end_request`
唤醒 `bread` 里的进程（10.4）。**上半部快收数、EOI 报到；下半部慢慢加工、
唤醒等待者**——0.11 的驱动世界，翻来覆去就是这一支舞。

## 10.9 动手实验

### 实验 10-1：找设备文件，看主/次设备号

```bash
ls -l /dev/tty*        # macOS 的终端设备是 /dev/ttys00X
tty                    # 看看你当前这个终端是哪个设备文件
```

正常 `ls -l` 那一列应该是文件大小，设备文件那里却是**两个数字**：

```
crw--w----  1 liwenqiang  tty    4, 0   /dev/tty0        # Linux 示例
crw--w----  1 liwenqiang  tty  0x10000000  /dev/ttys000    # macOS 示例
```

第一个数字就是**主设备号**（Linux 的 tty 是 4——跟 0.11 fs.h 注释里的
"4 - /dev/ttyx"一脉相承！），第二个是**次设备号**；新版 macOS 用十六进制
显示这一列，`0x10000000` 解开就是主 0x10、次 0。

**macOS/Linux 差异**：Linux 终端多半在 `/dev/tty0`、`/dev/pts/0`，macOS 是
`/dev/ttys000`，编号完全是各自的约定——就像第 5 章的系统调用号。顺手看看块
设备：Linux 上 `ls -l /dev/sda*`，同一块磁盘的分区们共用主 8、次设备号各异。

### 实验 10-2：往终端文件里写字

设备既然是文件，就该能 `echo` 进去。先确信自己没开别的会占屏的程序：

```bash
echo hi > $(tty)     # 写进你自己的终端（Linux/macOS 通用）
```

"hi"会直接浮现在你的命令行里。回看这行字的路径：shell 打开设备文件 →
`write()` → `tty_write` 填 `write_q` → `con_write` → 显存 0xB8000，10.5 节
每一站都对上了。

### 实验 10-3：行缓冲现场（本章主实验）

```c
/* linebuf_demo.c —— 观察终端"行缓冲"何时把字符交给你 */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char buf[10];
    ssize_t n;
    int round = 0, i, j;

    printf("本程序每次最多向终端要 10 字节：read(0, buf, 10)\n");
    printf("逐个敲键，观察【第 x 轮】何时出现（输入 q 退出）\n\n");
    fflush(stdout);              /* 不冲洗的话，提示语可能滞留在C库缓冲区 */

    for (;;) {
        n = read(0, buf, 10);    /* 向内核要最多10字节 */
        if (n <= 0)              /* Ctrl-D（EOF）或出错 */
            break;
        round++;
        printf("【第 %d 轮】read 返回 %zd 字节：", round, n);
        for (i = 0; i < (int)n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c == '\n')              printf("\\n ");
            else if (c >= 32 && c < 127) printf("%c ", c);
            else                        printf("\\x%02x ", c);
        }
        printf("\n");
        fflush(stdout);
        for (j = 0; j < (int)n; j++)
            if (buf[j] == 'q') {
                printf("收到 q，再见。\n");
                return 0;
            }
    }
    printf("read 返回 0（EOF），程序结束。\n");
    return 0;
}
```

```bash
gcc linebuf_demo.c -o linebuf_demo    # macOS 上 clang 同样命令
./linebuf_demo
```

**第一轮（默认行模式，对应 ICANON|ECHO）**：随便敲 `a`、`b`、`c`——程序毫无
反应（行规程在扣字节，回显也是它干的），直到按下回车，`read` 一次返回 4 字节：
`a b c \n`。连敲 12 个字母再回车，第一次就返回整整 10 字节——那是
`read(0, buf, 10)` 的上限。

**第二轮（关掉行规程）**：程序退出后，在**同一个终端**里执行：

```bash
stty raw -echo
./linebuf_demo
stty sane            # 务必还原！
```

现在每敲一键 `read` 立刻返回 1 字节——`ICANON` 关了；屏幕不再显示你敲的键
——`ECHO` 也关了。点 Ctrl-C：程序没死，只打印出 `\x03`——`ISIG` 也关了，
0x03 从"信号"还原成普通字节，10.5.2 里 `tty_intr` 的开关被摘掉了。

> **警告**：`stty raw -echo` 之后终端不回显、不认 Ctrl-C，看起来像"死了"。
> 还原口令是 `stty sane`（盲打进去，回车）；实在不行，换一个终端窗口执行
> `stty sane < /dev/ttysXXX` 也能救回来。

**macOS/Linux 差异**：`read`、`stty`、`sane` 两边通用，无需任何改动，差别
只在设备名（实验 10-1 说过）——行规程是 Unix 传统，两家同宗同源。

### 实验 10-4：磁盘快慢——缓存命中前后

```bash
# 造一个 512MB 的文件（Linux bs=1M，macOS 用小写 bs=1m）
dd if=/dev/zero of=big.bin bs=1M count=512
time dd if=big.bin of=/dev/null bs=1M    # 第 1 次：冷缓存，慢
time dd if=big.bin of=/dev/null bs=1M    # 第 2 次：命中第 9 章的"缓冲区缓存"，飞快
# 想复现"冷"的感觉（Linux，需要 root）：
echo 3 | sudo tee /proc/sys/vm/drop_caches
time dd if=big.bin of=/dev/null bs=1M    # 又慢了
```

第 1 次和第 2 次的速度差（常见 10 倍以上）就是"绕过磁盘 I/O"的代价差：快的
那次根本没碰硬件，全在第 9 章讲过的缓冲区缓存里打转。**macOS 差异**：没有
`/proc`，清缓存的等价命令是 `sudo purge`；不做清理、直接对比前两次 `time`
的结果也完全成立。

### 实验 10-5：看 CPU 的"空闲"——中断驱动省下的 CPU

```bash
top          # Linux：看 %Cpu(s) 行的 id 列（idle）
top -l 1 | grep -i "CPU usage"   # macOS：看 idle 百分比
```

一台安静机器的 idle 常在 90% 以上。这正是"中断驱动"替 CPU 省下来的——**CPU
闲着，是在等中断，不是在傻等设备**。若是轮询式系统，光等你敲键盘这一件事，
那 90% 就得全额烧掉。

## 10.10 常见误区

**误区一：/dev 下的文件占磁盘空间。**
不占。设备文件的 inode 里只有主/次设备号两个数（0.11 里就是 `i_zone[0]`
那一个字），没有数据块。`ls -l` 看它"大小为 0"是对的——那不是大小，是地址。

**误区二：把主/次设备号当文件大小。**
`4, 0` 不是"4 KB、第 0 个版本"，是"主 4 次 0"这张设备户口。判断方法：看
权限位开头是 `c`（字符设备）还是 `b`（块设备）——是设备文件，那一列就该
读成主/次设备号。

**误区三：`cat /dev/zero` 无害。**

```bash
cat /dev/zero > oops.bin    # 千万别试：/dev/zero 源源不断吐 0，几秒写满磁盘！
```

它是个读不完的字节流，重定向到文件会撑爆磁盘，输出到终端会刷屏刷到怀疑
人生。想安全体验，永远加限制：`head -c 1024 /dev/zero | xxd`。

**误区四：CPU 一直在"执行我的程序"。**
实验 10-5 已经看到 idle 高悬。一个"正在运行"的进程，时间也被切成三份——用户
态计算、内核里处理中断与系统调用、以及在调度间隙等待。你以为的"连续执行"，是
内核与硬件联手演出的错觉。

## 10.11 对照现代：2.6.35 的驱动世界

打开 `linux-2.6.35/drivers/`：**87 个一级子目录、约 1.1 万个文件**——0.11
的全部驱动只有 `blk_drv/` 与 `chr_drv/` 共十来个文件。二十年膨胀的不是
"多几个驱动"，而是整个**驱动模型**：

- **统一设备模型**：`include/linux/kobject.h` 第 59 行的 `struct kobject`
  是一切设备的公共基座（设备、总线、驱动在 `/sys` 里长成一颗树），框架代码
  在 `drivers/base/`。0.11 的驱动各自为政；现代驱动纳入统一的注册、枚举、
  电源管理生命周期。
- **上半部/下半部真实化**：`kernel/softirq.c` 的软中断（`NET_RX_SOFTIRQ`、
  `BLOCK_SOFTIRQ` 等固定枚举表）与 `tasklet_struct`：中断里只标记，真正的
  活在中断返回路径上批量做。
- **DMA 普及**：0.11 里只有软盘用 DMA；现代高速设备（网卡、NVMe、显卡）标配
  DMA，甚至有专门的 DMA 引擎（`drivers/dma/`）。"CPU 搬字节"如今反需专门优化。
- **0.11 没有的东西如实说**：没有 USB（1991 年 USB 标准尚未诞生，USB 支持要等
  本仓库 `linux-2.4.0/drivers/usb/` 才出现）、没有网络驱动、没有即插即用。

但剥掉规模与框架，2.6.35 的一个网络包到来时做的事，仍是 0.11 键盘那条流水线
的中断版：中断 → 上半部收包 → 下半部处理 → 唤醒等待的进程。**读完 0.11 的
驱动，你读现代驱动的"剧情大纲"已经齐了。**

## 练习题

**10.1（概念）** 用餐厅比喻向室友解释：为什么读整张软盘时轮询会把整台电脑
拖死，而中断驱动不会？DMA 又进一步省掉了什么？
（提示：对照 10.2 节的表格逐句复述；别忘了 0.11 里软盘用的是 `setup_DMA`。）

**10.2（观察）** 在你的电脑上执行 `ls -l /dev/tty*`，再看某个块设备（Linux
找 `/dev/sda`，macOS 找 `/dev/disk0`），抄下它们的主/次设备号。同一块磁盘的
两个分区（如 `/dev/sda1`、`/dev/sda2`）哪一号相同、哪一号不同？
（提示：主设备号对应"哪一类驱动"，次设备号区分"哪一个"；macOS 新版显示十六进制。）

**10.3（读码）** 打开 `linux-0.11/kernel/blk_drv/ll_rw_blk.c` 第 110~118 行：
为什么写请求从 `request+((NR_REQUEST*2)/3)` 开始找空位，而读请求从
`request+NR_REQUEST` 开始？
（提示：`blk.h` 第 5~14 行的注释写得明明白白；想想"读的进程"和"写的进程"
谁更等不起。）

**10.4（思考）** 顺着一次按键把睡眠唤醒链讲完整：进程在 `tty_read` 里睡着 →
你按下"a"并回车 → 谁把它叫醒？中间经过哪几个函数？
（提示：`keyboard.S` 的 `put_queue` → `copy_to_cooked` 末尾的 `wake_up` → `tty_read` 的 `sleep_if_empty`；对照 10.4.4 的磁盘闭环，结构一模一样。）

**10.5（动手）** 在实验 10-3 的 raw 模式下分别敲 Ctrl-C 和 Ctrl-D，记录程序
打印的字节（`\x03`、`\x04`）；解释为什么这时终端"杀不死"程序。做完务必 `stty
sane` 还原。
（提示：`tty_table` 里的 `ISIG | ICANON | ECHO`，raw 模式下谁是 off；`copy_to_cooked` 里 `INTR_CHAR` 的处理还会不会执行。）

## 本章小结

1. I/O 比计算慢 3~6 个数量级。等设备的三种姿势——轮询、中断驱动、DMA，
   一种比一种省 CPU；0.11 的硬盘是"中断驱动 + CPU 搬运"，软盘反而用 DMA。
2. 设备文件是门牌不是容器：inode 的 `i_zone[0]` 存一个设备号，主设备号选
   驱动、次设备号选实例，`mknod` 造门牌，不占任何数据块。
3. 字符设备（字节流）与块设备（定长块）按数据形态分类，各走各的缓存策略。
4. 一次磁盘读：`bread` → 32 张请求单排队（读优先、电梯算法雏形）→
   `do_hd_request` 端口编程 → 中断里 `read_intr` 搬运 → `end_request` 唤醒。
5. `do_hd = &read_intr` 用函数指针预约下一次中断，汇编入口 `xchgl` 原子取出
   再 `call *%edx`——附录 B 招式 4 的活学活用。
6. 键盘三步：0x60 端口取扫描码 → `key_table` 查表 → 入 `read_q`；画字只是
   往显存 0xB8000 写两个字节。
7. 行规程 `copy_to_cooked`：回显、退格、回车转 LF、Ctrl-C 变信号，凑齐一行
   才 `data++` 记账、才唤醒读者——"行缓冲"从名词变成了代码。
8. 中断处理分两半：上半部快收数、下半部慢慢加工。0.11 用"唤醒进程"当下半部，
   现代内核机制化为 softirq/tasklet。
9. 每个硬件中断处理完必须发 EOI（命令字节 0x20 写主片 0x20 / 从片 0xA0）；
   两片 8259A 经 IRQ2 级联出 15 路，向量号 = 0x20 + IRQ；忘发则线永远占线。
10. tty 三个队列（read_q / write_q / secondary）加 `interruptible_sleep_on`：
    `tty_read` 睡、中断醒——与磁盘路径是同一个闭环。

## 自测清单

- [ ] 能用餐厅比喻把轮询、中断驱动、DMA 各讲一遍，并指出 0.11 里各归谁用；
- [ ] 能在自己机器上读出 `/dev/tty*` 的主/次设备号，说出两个数字各管什么；
- [ ] 能从 `bread()` 一路讲到 `end_request()`，指出睡着的进程在哪一行被叫醒；
- [ ] 能解释 `do_hd = &read_intr` 为什么必要，`xchgl` 的原子性防的是什么；
- [ ] 能画出两片 8259A 的级联图，说出键盘、硬盘各发几次 EOI、发到哪个端口；
- [ ] 能顺着一次按键从扫描码讲到 `tty_read` 醒来，指出上、下半部的分界线；
- [ ] 做完实验 10-3，并能解释 raw 模式下 Ctrl-C 为什么成了普通字节 0x03。

## 延伸阅读

- 《Linux内核完全注释 v5.0》（本仓库 `doc/` 目录）设备驱动相关章节（块设备与
  字符设备驱动两部分）——对 `ll_rw_blk.c`、`hd.c`、`tty_io.c`、`console.c`、
  `keyboard.S` 的逐行注释，与本章源码导读对照阅读最佳；
- 《Linux Device Drivers, 3rd Edition》（LDD3，可免费在线阅读）前三章——用现代
  内核写一个字符驱动的入门；其"Interrupt Handling"一章与 10.6 节互证；
- 好奇你的磁盘有多快：Linux 上 `sudo hdparm -t /dev/sda`，和实验 10-4 的
  `dd` 数字对照——现代硬盘带宽是 0.11 软盘的十万倍，但"中断 + 缓存 + DMA"
  三板斧，一件都没变。

[← 第 9 章](ch09-文件系统.md) | [第 11 章 →](ch11-网络编程.md)
