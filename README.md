**lnxrm** | 一个类 Unix 的 x86-64 内核，使用汇编 + C + C++ + Rust 混合开发。

功能总览

| 子系统 | 说明 |
|---|---|
| 引导 | 标准 Linux bzImage 协议（HdrS 2.08）；实模式 stub 完成 A20/E820/建页表/长模式切换；另含 UEFI stub（PE32+） |
| 内存 | E820 → 伙伴系统物理页分配器（order 0..10）；每进程独立地址空间（用户态独占 PML4 槽 255）；内核高半区 + 全 RAM 别名 |
| 堆 | 2 的幂分离空闲链 kmalloc/kfree + C++ SlabCache |
| SMP | AP 启动 trampoline（实模式 → 保护模式 → 长模式）；AP GDT 0x8800；共享数据页 0x9000；per-cpu 数据（GS base / rdmsr）；每 CPU 独立 runqueue + task_lock 自旋锁 |
| 进程 | fork/execve/waitpid/exit/nanosleep；基于 PIT 的时间片抢占调度；zombie 停驻 + 栈回收 |
| 信号 | POSIX 信号机制：SIGTERM/SIGKILL/SIGINT/SIGCHLD/SIGSTOP/SIGCONT；用户态信号 trampoline；信号投递在返回用户态时检查 |
| 中断 | IDT 全向量、IOAPIC + LAPIC（APIC MMIO 映射）、PIT 100Hz 时钟、PS/2 键盘 + COM1 串口 RX 中断；syscall 内 console_read 临时开中断实现键盘轮询 |
| 系统调用 | int 0x80 门（DPL3），22 个系统调用：read/write/open/close/lseek/brk/getdent/dup2/nanosleep/getpid/fork/execve/exit/wait4/kill/uname/sigaction/sigprocmask/getppid/ps/getcpu/diskinfo |
| 用户态程序 | 每个命令是独立 ELF 二进制（`/bin/*`），通过 fork+execve 运行 |
| 文件系统 | VFS 路由层；ramfs（内嵌 cpio initramfs）；**FAT32**（读/写/创建，LFN 读） |
| 设备 | PCI 枚举（C++ Driver 注册框架）、**IDE PIO 磁盘**（QEMU 原生支持）、**AHCI/SATA DMA 磁盘**、PS/2 键盘（VGA 光标同步）、16550 串口（Rust 实现） |

用户命令

| 命令 | 说明 |
|---|---|
| `sh` | 用户态终端，fork/execve 运行命令 |
| `ls [dir]` | 列出目录，目录带 `/` 后缀 |
| `cat <file>` | 打印文件内容 |
| `touch <file>` | 创建空文件 |
| `echo [args]` | 输出参数 |
| `ps` | 列出进程 |
| `kill <pid>` | 发送信号 |
| `uname` | 系统信息 |
| `fdisk` | 列出块设备 |
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
*中断帧完整性**：callee-saved RBX 已纳入中断帧保存/恢复；
  `pid_counter` 附带金丝雀自检（`sched_tick` 每 tick 校验影子副本）。
*键盘输入**：int 0x80 syscall 执行期间 CPU 自动关中断（IF=0），导致 PS/2
  键盘 IRQ1 无法送达；`console_read` 忙等循环中用 `sti; pause; cli` 临时
  开中断，使键盘中断可送达并经 `input_push()` 写入环形缓冲区。

磁盘支持

* **IDE PIO**：内核态 IDE PIO 驱动（0x1F0 端口），支持 QEMU 原生 IDE 磁盘
* **AHCI/SATA**：AHCI DMA 驱动，支持 SATA 设备
* **FAT32**：读/写/创建文件，支持长文件名（LFN）
* **64MB 虚拟磁盘**：QEMU 启动时自动挂载 `build/disk.img` 到 `/mnt`

使用方法

```bash
# 构建内核
make

# 运行 QEMU（自动挂载磁盘）
make run
```

已知限制
* 磁盘写入标记为实验性。
* 仅支持 4 核（BSP + 3 AP）；AP 仅参与 idle 循环，未实现负载均衡。
