//! Safe wrappers over the C kernel heap (kmalloc/kfree) and a spinlock
//! self-test that the kernel runs at boot to prove the Rust/C boundary.

extern "C" {
    fn kmalloc(size: usize) -> *mut u8;
    fn kfree(ptr: *mut u8);
}

/// RAII heap allocation, `Box`-like but backed by the C kernel allocator.
pub struct KBox<T> {
    ptr: *mut T,
}

impl<T> KBox<T> {
    /// # Safety
    /// T must be trivially constructible; contents are uninitialized until
    /// written by the caller.
    pub unsafe fn new_uninit() -> Option<KBox<T>> {
        let raw = kmalloc(core::mem::size_of::<T>());
        if raw.is_null() {
            None
        } else {
            Some(KBox { ptr: raw as *mut T })
        }
    }

    pub fn new(v: T) -> Option<KBox<T>> {
        unsafe {
            let mut b = Self::new_uninit()?;
            core::ptr::write(b.ptr, v);
            Some(b)
        }
    }

    pub fn leak(b: KBox<T>) -> *mut T {
        let p = b.ptr;
        core::mem::forget(b);
        p
    }
}

impl<T> core::ops::Deref for KBox<T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.ptr }
    }
}

impl<T> core::ops::DerefMut for KBox<T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.ptr }
    }
}

impl<T> Drop for KBox<T> {
    fn drop(&mut self) {
        unsafe {
            core::ptr::drop_in_place(self.ptr);
            kfree(self.ptr as *mut u8);
        }
    }
}

use crate::sync::Spinlock;

static TEST_LOCK: Spinlock<u64> = Spinlock::new(0);

/// Runs at boot via rust_hello(): exercises KBox + Spinlock end-to-end.
/// Returns 42 when everything works (checked by C).
#[no_mangle]
pub extern "C" fn rust_selftest() -> u64 {
    let mut b = match KBox::new(0x1234_5678u32) {
        Some(b) => b,
        None => return 0,
    };
    {
        let mut g = TEST_LOCK.lock();
        *g += 1;
        assert!(*g == 1);
    }
    if *b != 0x1234_5678 {
        return 0;
    }
    *b += 1;
    if *b != 0x1234_5679 {
        return 0;
    }
    42
}
