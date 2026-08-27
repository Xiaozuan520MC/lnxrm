//! Spinlock<T>: a safe mutual-exclusion primitive for kernel data.
//!
//! The guard dereferences to `&mut T`; unlocking happens on drop. Used by
//! the self-test in kalloc.rs and available to future Rust-side subsystems.

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::sync::atomic::{AtomicBool, Ordering};

pub struct Spinlock<T> {
    locked: AtomicBool,
    data: UnsafeCell<T>,
    _p: PhantomData<*const ()>, // !Send + !Sync marker unless T: Send
}

unsafe impl<T: Send> Sync for Spinlock<T> {}
unsafe impl<T: Send> Send for Spinlock<T> {}

pub struct SpinlockGuard<'a, T> {
    lock: &'a Spinlock<T>,
    _no_send: PhantomData<*mut ()>,
}

impl<'a, T> Drop for SpinlockGuard<'a, T> {
    fn drop(&mut self) {
        self.lock.locked.store(false, Ordering::SeqCst);
    }
}

impl<'a, T> core::ops::Deref for SpinlockGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.data.get() }
    }
}

impl<'a, T> core::ops::DerefMut for SpinlockGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<T> Spinlock<T> {
    pub const fn new(v: T) -> Self {
        Spinlock {
            locked: AtomicBool::new(false),
            data: UnsafeCell::new(v),
            _p: PhantomData,
        }
    }

    pub fn lock(&self) -> SpinlockGuard<'_, T> {
        while self
            .locked
            .compare_exchange_weak(false, true, Ordering::SeqCst, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinlockGuard {
            lock: self,
            _no_send: PhantomData,
        }
    }
}
