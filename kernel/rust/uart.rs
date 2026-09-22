//! 16550 UART driver -- the kernel's primary console (COM1).
//!
//! Owned entirely by Rust; C reaches it through the `lnxrm_uart_*` ABI below.

use core::sync::atomic::{AtomicBool, Ordering};

const COM1: u16 = 0x3F8;

mod reg {
    pub const DATA: u16 = 0; // RBR / THR
    pub const IER: u16 = 1;
    pub const FCR: u16 = 2;
    pub const LCR: u16 = 3;
    pub const MCR: u16 = 4;
    pub const LSR: u16 = 5;
}

#[inline]
fn out(port: u16, val: u8) {
    unsafe { core::arch::asm!("out dx, al", in("dx") port, in("al") val) }
}

#[inline]
fn inn(port: u16) -> u8 {
    let v: u8;
    unsafe {
        core::arch::asm!("in al, dx", out("al") v, in("dx") port, options(nomem));
    }
    v
}

static INITED: AtomicBool = AtomicBool::new(false);

pub fn init() {
    out(COM1 + reg::IER, 0x00); // no irq while programming
    out(COM1 + reg::LCR, 0x80); // DLAB
    out(COM1 + reg::DATA, 0x01); // divisor low: 115200 baud
    out(COM1 + reg::IER, 0x00); // divisor high
    out(COM1 + reg::LCR, 0x03); // 8N1
    out(COM1 + reg::FCR, 0xC7); // FIFO on
    out(COM1 + reg::MCR, 0x0B);
    INITED.store(true, Ordering::SeqCst);
}

pub fn send_byte(b: u8) {
    if !INITED.load(Ordering::SeqCst) {
        return;
    }
    while inn(COM1 + reg::LSR) & 0x20 == 0 {
        core::hint::spin_loop();
    }
    out(COM1 + reg::DATA, b);
    // settle: a couple of ISA-post style dummy reads keep QEMU's chardev
    // backpressure happy during long bursts
    let _ = inn(0x84);
}

pub fn write_str(s: &str) {
    for b in s.bytes() {
        if b == b'\n' {
            send_byte(b'\r');
        }
        send_byte(b);
    }
}

pub fn recv_ready() -> bool {
    INITED.load(Ordering::SeqCst) && inn(COM1 + reg::LSR) & 0x01 != 0
}

pub fn recv_byte() -> u8 {
    inn(COM1 + reg::DATA)
}

// ---------------------------------------------------------------- C ABI --
#[no_mangle]
pub extern "C" fn lnxrm_uart_init() {
    init();
}

#[no_mangle]
pub extern "C" fn lnxrm_uart_putc(c: char) {
    let b = c as u8;
    if b == b'\n' {
        send_byte(b'\r');
    }
    send_byte(b);
}

#[no_mangle]
pub extern "C" fn lnxrm_uart_trygetc() -> i32 {
    if recv_ready() {
        recv_byte() as i32
    } else {
        -1
    }
}

/// enable the RX interrupt so IRQ4 pushes bytes into the input queue
#[no_mangle]
pub extern "C" fn lnxrm_uart_irq_enable() -> i32 {
    if !INITED.load(Ordering::SeqCst) {
        return -1;
    }
    out(COM1 + reg::IER, 0x01);
    0
}
