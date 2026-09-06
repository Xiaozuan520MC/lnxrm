**lnxrm** | 一个类 Unix 的 x86-64 内核，使用汇编 + C + C++ + Rust 混合开发。

功能总览

| 子系统 | 说明 |
|---|---|
| 引导 | 标准 Linux bzImage 协议（HdrS 2.08）；实模式 stub 完成 A20/E820/建页表/长模式切换；另含 UEFI stub（PE32+） |
| 内存 | E820 → 伙伴系统物理页分配器（order 0..10）；每进程独立地址空间（用户态独占 PML4 槽 255）；内核高半区 + 全 RAM 别名 |
| 堆 | 2 的幂分离空闲链 kmalloc/kfree + C++ SlabCache |
| 进程 | fork/execve/waitpid/exit/nanosleep；协作式调度（抢占路径保留可启用）；zombie 停驻 + 栈回收 |
| 中断 | IDT 全向量、8259 PIC、PIT 100Hz 时钟、PS/2 键盘 + COM1 串口 RX 中断；syscall 内 console_read 临时开中断实现键盘轮询 |
| 系统调用 | int 0x80 门（DPL3），参数 rdi/rsi/r10，Linux 风格编号（RBX 已纳入帧保存）|
| 用户态终端 | `/bin/sh`：内建 echo/ls/cat/ps/uname/clear/help，`>` 重定向，fork+execve 运行 /bin/* 程序；支持 BS/DEL 退格 |
| 文件系统 | VFS 路由层；ramfs（内嵌 cpio initramfs）；**FAT32**（读/写/创建，LFN 读） |
| 设备 | PCI 枚举（C++ Driver 注册框架）、**AHCI/SATA DMA 磁盘**、PS/2 键盘（VGA 光标同步）、16550 串口（Rust 实现） |

系统调用（int 0x80）

| nr | 名称 | nr | 名称 |
|----|------|----|------|
| 0 | read | 33 | dup2 |
| 1 | write | 35 | nanosleep |
| 2 | open | 39 | getpid |
| 3 | close | 57 | fork |
| 8 | lseek | 59 | execve |
| 12 | brk | 60/61 | exit/wait4 |
| 17 | getdent | 110 | getppid |
| 63 | uname | 200 | ps |

设计要点

*bzImage 兼容**：setup 段完全位置无关（运行时自补丁远跳转/GDT 基址），
  兼容 SeaBIOS 把 setup 放在 0x10000 的现实行为。
*地址空间**：槽 0 = 低 RAM 恒等映射（所有地址空间共享，仅内核可访问），
  槽 255 = 进程私有用户区（0x7f8000000000 起），槽 511 = 物理内存高别名，
  且伙伴系统管理的全部物理帧都有高别名映射。
*调度**：任务在阻塞点经 `swtch()` 停靠自身内核栈；阻塞返回采用 POSIX 式
  假唤醒语义；抢占路径保留在源码中可启用。
*中断帧完整性**：callee-saved RBX 已纳入中断帧保存/恢复；
  `pid_counter` 附带金丝雀自检（`sched_tick` 每 tick 校验影子副本）。
*键盘输入**：int 0x80 syscall 执行期间 CPU 自动关中断（IF=0），导致 PS/2
  键盘 IRQ1 无法送达；`console_read` 忙等循环中用 `sti; pause; cli` 临时
  开中断，使键盘中断可送达并经 `input_push()` 写入环形缓冲区。

已知限制
* AHCI 写入后立刻读回偶发读到旧数据（命令间等待不充分），读路径与宿主预写
  文件完全稳定；磁盘写入标记为实验性。
* 单核；无信号、无 swap。
