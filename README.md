**lnxrm** | 一个类 Unix 的 x86-64 内核，使用汇编 + C + C++ + Rust 混合开发。

功能总览

| 子系统 | 说明 |
|---|---|
| 引导 | 标准 Linux bzImage 协议（HdrS 2.08）；实模式 stub 完成 A20/E820/建页表/长模式切换；另含 UEFI stub（PE32+） |
| 内存 | E820 → 伙伴系统物理页分配器（order 0..10）；每进程独立地址空间（用户态独占 PML4 槽 255）；内核高半区 + 全 RAM 别名 |
| 堆 | 2 的幂分离空闲链 kmalloc/kfree + C++ SlabCache |
| 进程 | fork/execve/waitpid/exit/nanosleep；协作式调度（抢占路径保留可启用）；zombie 停驻 + 栈回收 |
| 中断 | IDT 全向量、8259 PIC、PIT 100Hz 时钟、PS/2 键盘 + COM1 串口 RX 中断 |
| 系统调用 | int 0x80 门（DPL3），参数 rdi/rsi/r10，Linux 风格编号（RBX 已纳入帧保存）|
| 用户态终端 | `/bin/sh`：内建 echo/ls/cat/ps/uname/clear/help，`>` 重定向，fork+execve 运行 /bin/* 程序 |
| 文件系统 | VFS 路由层；ramfs（内嵌 cpio initramfs）；**FAT32**（读/写/创建，LFN 读） |
| 设备 | PCI 枚举（C++ Driver 注册框架）、**AHCI/SATA DMA 磁盘**、PS/2 键盘、16550 串口（Rust 实现） |

目录结构 / 语言分工

| 语言 | 模块 | 位置 |
|---|---|---|
| **汇编** (NASM) | bzImage 头 + 实模式→长模式引导 | `arch/setup.asm` |
| | 长模式入口 / IDT 桩 / 上下文切换 swtch | `arch/entry64.S` |
| | GDT/IDT/TSS/PIC/PIT/CPU 初始化 | `arch/cpu.c` |
| **C** | 启动与子系统初始化 | `kernel/main.c` |
| | 伙伴系统物理内存管理 | `kernel/mm/pmm.c` |
| | 页表 / 地址空间 | `kernel/mm/vmm.c` |
| | 内核堆 kmalloc/kfree | `kernel/mm/kheap.c` |
| | 进程 fork/exec/wait/exit | `kernel/task.c` |
| | 调度器 | `kernel/sched.c` |
| | 中断分发 | `kernel/isr.c` |
| | 系统调用分发 | `kernel/syscall.c` |
| | ELF64 加载器 | `kernel/elf.c` |
| | VFS 层 | `kernel/fs/vfs.c` |
| | ramfs + cpio initramfs | `kernel/fs/ramfs.c` |
| | FAT32 文件系统 | `kernel/fs/fat32.c` |
| | PCI 扫描 | `kernel/drivers/pci.c` |
| | AHCI/SATA DMA 驱动 | `kernel/drivers/ahci.c` |
| | 字符串库 | `lib/string.c` |
| **C++** | Device/Driver 抽象基类 + 注册框架 | `kernel/drivers/{Device.hpp,Driver.cpp}` |
| | File RAII 封装 | `kernel/fs/File.hpp` |
| | Process 封装 | `kernel/Process.hpp` |
| | SlabAllocator（对象缓存） | `kernel/mm/SlabAllocator.{hpp,cpp}` |
| | IntrusiveList / SimpleVec 容器 | `lib/List.hpp` |
| **Rust** | UART 控制台驱动（内核主控制台） | `kernel/rust/uart.rs` |
| | Spinlock<T> 安全自旋锁 | `kernel/rust/sync.rs` |
| | KBox<T>（kmalloc 安全封装）+ 自测 | `kernel/rust/kalloc.rs` |
| | 系统调用常量与安全接口 | `kernel/rust/sysapi.rs` |
| | crate 入口 / panic handler | `kernel/rust/lib.rs` |
| **用户态** | init、sh 终端、hello、crt0、mini-libc | `usr/` |
| 头文件 | 内核公共接口（扁平，无子目录） | `include/*.h` |
| **脚本** | cpio newc 打包 / bzImage 头修补 | `scripts/` |

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

已知限制
* AHCI 写入后立刻读回偶发读到旧数据（命令间等待不充分），读路径与宿主预写
  文件完全稳定；磁盘写入标记为实验性。
* 单核；无信号、无 swap。
