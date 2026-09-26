/* Global C++ runtime new/delete, routed to the kernel heap.
 * Freestanding build: no libc++ provides these symbols, so every C++
 * `new`/`delete` in the kernel lands here. */
#include <stddef.h>

extern "C" {
void *kmalloc(size_t n);
void kfree(void *p);
void panic(const char *fmt, ...);
}

void *operator new(size_t sz)
{
    void *p = kmalloc(sz);
    if (!p) panic("operator new: out of memory (%u bytes)", sz);
    return p;
}

void *operator new[](size_t sz)
{ return operator new(sz); }

void operator delete(void *p) noexcept
{ kfree(p); }

void operator delete(void *p, size_t) noexcept
{ kfree(p); }

void operator delete[](void *p) noexcept
{ kfree(p); }

void operator delete[](void *p, size_t) noexcept
{ kfree(p); }
