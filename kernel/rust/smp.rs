//! SMP primitives: trampoline shared data, memory barriers.
//!
//! The trampoline code (arch/trampoline.S) runs at physical 0x8000 and
//! communicates with the BSP via shared data at physical 0x9000. This
//! module provides safe volatile access to that shared data, replacing
//! the raw C struct with proper memory ordering guarantees.

use core::ptr;
use core::sync::atomic::{fence, Ordering};

/// Physical address of trampoline shared data.
pub const TRAMPOLINE_DATA_PHYS: usize = 0x9000;

/// Shared data between BSP and AP at physical 0x9000.
/// Layout must match trampoline.S exactly — all fields u64-aligned.
#[repr(C)]
pub struct TrampolineData {
    pub page_dir: u64,       // 0x00 - physical PML4 address
    pub stack_top: u64,      // 0x08 - 64-bit virtual stack top
    pub ready: u64,          // 0x10 - AP sets to 1 when initialized
    pub ap_id: u64,          // 0x18 - logical CPU id from BSP
    pub gdt_packed: u64,     // 0x20 - limit:u16 | base:u48
    pub ap_main_phys: u64,   // 0x28 - physical address of ap_main()
    _rsvd0: u64,             // 0x30
    _rsvd1: u64,             // 0x38
}

/// Safe wrapper for trampoline shared data.
///
/// All accesses use volatile read/write with SeqCst fences to ensure
/// the BSP and AP see consistent state regardless of cache topology.
pub struct Trampoline;

impl Trampoline {
    /// Get pointer to shared data (identity-mapped at physical 0x9000).
    ///
    /// # Safety
    /// The identity map for low 1 GiB must be active. This is guaranteed
    /// after early boot and remains active for the kernel lifetime.
    #[inline]
    unsafe fn ptr() -> *mut TrampolineData {
        TRAMPOLINE_DATA_PHYS as *mut TrampolineData
    }

    /// Prepare shared data for AP startup (BSP calls this before IPI).
    ///
    /// Writes all fields with volatile stores and issues a SeqCst fence
    /// to ensure the AP sees consistent data when it starts executing.
    pub fn setup(pml4: u64, stack_top: u64, ap_id: u32, gdt_packed: u64, ap_main: u64) {
        let p = unsafe { Self::ptr() };
        unsafe {
            ptr::write_volatile(&mut (*p).page_dir, pml4);
            ptr::write_volatile(&mut (*p).stack_top, stack_top);
            ptr::write_volatile(&mut (*p).ready, 0);
            ptr::write_volatile(&mut (*p).ap_id, ap_id as u64);
            ptr::write_volatile(&mut (*p).gdt_packed, gdt_packed);
            ptr::write_volatile(&mut (*p).ap_main_phys, ap_main);
        }
        // Ensure all writes are visible before the AP is signaled
        fence(Ordering::SeqCst);
    }

    /// Check if AP has finished initialization (BSP polls this).
    #[inline]
    pub fn is_ready() -> bool {
        let p = unsafe { Self::ptr() };
        unsafe { ptr::read_volatile(&(*p).ready) != 0 }
    }

    /// Signal readiness from AP side.
    ///
    /// Issues a SeqCst fence before writing to ensure all AP-side
    /// initialization stores are visible to the BSP.
    pub fn signal_ready() {
        let p = unsafe { Self::ptr() };
        fence(Ordering::SeqCst);
        unsafe {
            ptr::write_volatile(&mut (*p).ready, 1);
        }
    }

    /// Clear all fields (used during BSP initialization).
    pub fn clear() {
        let p = unsafe { Self::ptr() };
        unsafe {
            ptr::write_volatile(&mut (*p).page_dir, 0);
            ptr::write_volatile(&mut (*p).stack_top, 0);
            ptr::write_volatile(&mut (*p).ready, 0);
            ptr::write_volatile(&mut (*p).ap_id, 0);
            ptr::write_volatile(&mut (*p).gdt_packed, 0);
            ptr::write_volatile(&mut (*p).ap_main_phys, 0);
            ptr::write_volatile(&mut (*p)._rsvd0, 0);
            ptr::write_volatile(&mut (*p)._rsvd1, 0);
        }
    }
}

// ===== C-callable FFI functions =====
// These are called from kernel/smp.cpp via extern "C".

/// Prepare trampoline data for AP startup.
#[no_mangle]
pub extern "C" fn trampoline_setup(
    pml4: u64,
    stack_top: u64,
    ap_id: u32,
    gdt_packed: u64,
    ap_main_phys: u64,
) {
    Trampoline::setup(pml4, stack_top, ap_id, gdt_packed, ap_main_phys);
}

/// Check if AP has signaled readiness. Returns 1 if ready, 0 otherwise.
#[no_mangle]
pub extern "C" fn trampoline_is_ready() -> u32 {
    if Trampoline::is_ready() {
        1
    } else {
        0
    }
}

/// Signal readiness from AP side.
#[no_mangle]
pub extern "C" fn trampoline_signal_ready() {
    Trampoline::signal_ready();
}

/// Clear all trampoline data fields.
#[no_mangle]
pub extern "C" fn trampoline_clear() {
    Trampoline::clear();
}
