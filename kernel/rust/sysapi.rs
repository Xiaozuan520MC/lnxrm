//! Typed syscall surface mirroring syscall.h.
//!
//! The kernel itself does not issue syscalls; this module documents the ABI
//! in one place and provides the raw invocation helpers used by tooling and
//! by any Rust userland components.

pub const SYS_READ: usize = 0;
pub const SYS_WRITE: usize = 1;
pub const SYS_OPEN: usize = 2;
pub const SYS_CLOSE: usize = 3;
pub const SYS_BRK: usize = 12;
pub const SYS_NANOSLEEP: usize = 35;
pub const SYS_GETPID: usize = 39;
pub const SYS_DUP2: usize = 33;
pub const SYS_FORK: usize = 57;
pub const SYS_EXECVE: usize = 59;
pub const SYS_EXIT: usize = 60;
pub const SYS_WAIT4: usize = 61;
pub const SYS_GETPPID: usize = 110;

#[inline]
unsafe fn syscall1(nr: usize, a: usize) -> isize {
    let ret: isize;
    core::arch::asm!(
        "int 0x80",
        inlateout("rax") nr => ret,
        in("rdi") a,
        lateout("rcx") _, lateout("r11") _
    );
    ret
}

/// Write a byte slice to an fd. Returns bytes written or negative errno.
pub fn write(fd: usize, buf: &[u8]) -> isize {
    unsafe { syscall3(SYS_WRITE, fd, buf.as_ptr() as usize, buf.len()) }
}

#[inline]
unsafe fn syscall3(nr: usize, a: usize, b: usize, c: usize) -> isize {
    let ret: isize;
    core::arch::asm!(
        "int 0x80",
        inlateout("rax") nr => ret,
        in("rdi") a,
        in("rsi") b,
        in("rdx") c,
        lateout("rcx") _, lateout("r11") _
    );
    ret
}
