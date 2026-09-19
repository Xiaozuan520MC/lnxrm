**lnxrm** | 一个类 Unix 的 x86-64 内核，使用汇编 + C + C++ + Rust 混合开发。

功能总览

| 子系统 | 说明 |
|---|---|
| 引导 | 标准 Linux bzImage 协议（HdrS 2.08）；实模式 stub 完成 A20/E820/建页表/长模式切换 |
| 内存 | E820 → 伙伴系统物理页分配器（order 0..10）；每进程独立地址空间（用户态独占 PML4 槽 255）；内核高半区 + 全 RAM 别名 |
| 堆 | 2 的幂分离空闲链 kmalloc/kfree + C++ SlabCache |
| SMP | AP 启动 trampoline（实模式 → 保护模式 → 长模式）；AP GDT 0x8800；共享数据页 0x9000；per-cpu 数据（GS base / rdmsr）；每 CPU 独立 runqueue + task_lock 自旋锁 |
| 进程 | fork/execve/waitpid/exit/nanosleep；基于 PIT 的时间片抢占调度；zombie 停驻 + 栈回收 |
| 信号 | POSIX 信号机制：SIGTERM/SIGKILL/SIGINT/SIGCHLD/SIGSTOP/SIGCONT；用户态信号 trampoline；信号投递在返回用户态时检查 |
| 中断 | IDT 全向量、IOAPIC + LAPIC（APIC MMIO 映射）、PIT 100Hz 时钟、PS/2 键盘 + COM1 串口 RX 中断；syscall 内 console_read 临时开中断实现键盘轮询 |
| 系统调用 | int 0x80 门（DPL3），29 个系统调用（0-28） |
| 用户态程序 | 每个命令是独立 ELF 二进制（`/bin/*`），通过 fork+execve 运行 |
| 文件系统 | VFS 路由层；ramfs（内嵌 cpio initramfs）；**FAT32**（读/写/创建，LFN 读） |
| 设备 | PCI 枚举（C++ Driver 注册框架）、**IDE PIO 磁盘**、**AHCI/SATA DMA 磁盘**、PS/2 键盘、16550 串口（Rust 实现） |
| 帧缓冲区 | VBE 640x480x256 色图形模式；Bochs DISPI I/O 端口从保护模式切换；内核态文本终端 80x30（8x16 字体，ANSI 转义支持）；用户态系统调用接口 |

系统调用

| 编号 | 名称 | 说明 |
|------|------|------|
| 0 | read | 读文件 |
| 1 | write | 写文件 |
| 2 | open | 打开文件 |
| 3 | close | 关闭文件 |
| 4 | lseek | 移动文件指针 |
| 5 | brk | 调整堆 |
| 6 | getdent | 读目录 |
| 7 | dup2 | 复制文件描述符 |
| 8 | nanosleep | 休眠 |
| 9 | getpid | 获取进程 ID |
| 10 | fork | 创建子进程 |
| 11 | execve | 加载程序 |
| 12 | exit | 退出 |
| 13 | wait4 | 等待子进程 |
| 14 | kill | 发送信号 |
| 15 | uname | 系统信息 |
| 16 | sigaction | 信号处理 |
| 17 | sigprocmask | 信号掩码 |
| 18 | getppid | 获取父进程 ID |
| 19 | ps | 进程列表 |
| 20 | sigreturn | 信号返回 |
| 21 | getcpu | 获取 CPU ID |
| 22 | diskinfo | 磁盘信息 |
| 23 | mkdir | 创建目录 |
| 24 | fb_info | 获取帧缓冲区信息 |
| 25 | fb_clear | 清屏 |
| 26 | fb_fill | 填充矩形 |
| 27 | fb_char | 绘制字符 |
| 28 | fb_puts | 绘制字符串 |

用户命令

| 命令 | 说明 |
|---|---|
| `sh` | 用户态终端 |
| `ls [dir]` | 列出目录 |
| `cat <file>` | 打印文件内容 |
| `touch <file>` | 创建空文件 |
| `echo [args]` | 输出参数 |
| `ps` | 列出进程 |
| `kill <pid>` | 发送信号 |
| `mkdir <dir>` | 创建目录 |
| `fdisk` | 列出块设备 |
| `clear` | 清屏 |
| `cpu` | CPU 信息 |
| `hello` | 打招呼 |
| `help` | 列出可用命令 |

设计要点

*bzImage 兼容**：setup 段完全位置无关（运行时自补丁远跳转/GDT 基址），
  兼容 SeaBIOS 把 setup 放在 0x10000 的现实行为。
*地址空间**：槽 0 = 低 RAM 恒等映射（所有地址空间共享，仅内核可访问），
  槽 255 = 进程私有用户区（0x7f8000000000 起），槽 511 = 物理内存高别名，
  且伙伴系统管理的全部物理帧都有高别名映射。
*SMP**：AP 启动流程经过实模式 stub → 保护模式 → 长模式三阶段，
  每个 AP 获取独立 per-cpu 数据结构（通过 GS base MSR），
  BSP 等待所有 AP 离线确认后继续。trampoline 代码编译为 flat binary（org 0x8000），
  AP GDT 放置在物理地址 0x8800（含 null/32-bit code/data/64-bit code/data 五项），
  共享数据结构（页表/栈/就绪标志/GDT 描述符）对齐到物理地址 0x9000。
*调度**：任务在阻塞点经 `swtch()` 停靠自身内核栈；PIT 定时器驱动时间片
  抢占（quantum=6 ticks）；`sched_maybe_preempt` 在返回用户态路径检查
  `need_resched` 标志；每 CPU 独立 runqueue 通过自旋锁同步。
*信号**：信号处理函数（trampoline）以物理页形式映射到用户态固定虚拟地址
  （SIG_TRAMPOLINE_VA），通过 FIXMAP 临时映射完成拷贝。
  `send_signal` 在投递信号时唤醒睡眠目标并加入 runqueue；
  信号在返回用户态时统一检查和投递。
*键盘输入**：int 0x80 syscall 执行期间 CPU 自动关中断（IF=0），导致 PS/2
  键盘 IRQ1 无法送达；`console_read` 忙等循环中用 `sti; pause; cli` 临时
  开中断，使键盘中断可送达并经 `input_push()` 写入环形缓冲区。
*帧缓冲区**：VBE 模式信息在 setup.asm 实模式阶段通过 INT 10h（AX=4F01）查询
  并保存在物理地址 0x8C00；模式切换延迟到 `fb_init()` 中通过 Bochs DISPI I/O
  端口（0x1CE/0x1CF）从保护模式执行，保证启动过程 VGA 文本模式可见。
  页表 PD_HI[104] 将 VGA VRAM（物理 0xFD000000）以 4KiB 页面映射到
  内核虚拟地址 0xFFFFFFFF8D000000。内核态帧缓冲终端使用 4096 字节环形缓冲区
  记录所有 `console_putc` 输出，`fb_init()` 时一次性回放到帧缓冲区，
  保证不丢失早期启动信息。支持 ESC[2J（清屏）、ESC[H（光标归位）、
  ESC[...m（颜色属性）等 ANSI 转义序列。帧缓冲区系统调用通过 copy_from_user
  从用户态接收绘图参数，内核直接操作帧缓冲内存完成绘制。

磁盘支持

* **IDE PIO**：内核态 IDE PIO 驱动（0x1F0 端口），支持 QEMU 原生 IDE 磁盘
* **AHCI/SATA**：AHCI DMA 驱动，支持 SATA 设备
* **FAT32**：读/写/创建文件，支持长文件名（LFN）
* **64MB 虚拟磁盘**：QEMU 启动时自动挂载 `build/disk.img` 到 `/mnt`

使用方法

```bash
# 构建内核 + 用户程序 + initramfs
make

# 运行 QEMU（自动挂载磁盘）
make run
```

已知限制
* 磁盘写入标记为实验性。
* MAX_CPUS = 8，QEMU 默认 -smp 2；AP 仅参与 idle 循环，未实现负载均衡。
* 帧缓冲区为 8 位 256 色模式，不支持 16/24/32 位真彩色。
