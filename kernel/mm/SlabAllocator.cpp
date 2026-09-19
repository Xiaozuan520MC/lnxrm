/* C++ slab allocator: fixed-size object caches on top of kmalloc. */
#include "SlabAllocator.hpp"

extern "C" {
void *kmalloc(size_t n);
void kfree(void *p);
}

void *SlabBase::operator new(size_t sz)
{
    return kmalloc(sz);
}

void SlabBase::operator delete(void *p)
{
    kfree(p);
}

/* ---- global new/delete -> kernel heap (no exceptions path) ---- */
extern "C" void panic(const char *fmt, ...);

void *operator new(size_t sz)
{
    void *p = kmalloc(sz);
    if (!p)
        panic("operator new: out of memory (%u bytes)", sz);
    return p;
}

void *operator new[](size_t sz)
{
    return operator new(sz);
}

void operator delete(void *p) noexcept
{
    kfree(p);
}

void operator delete(void *p, size_t) noexcept
{
    kfree(p);
}

void operator delete[](void *p, size_t) noexcept
{
    kfree(p);
}

void operator delete[](void *p) noexcept
{
    kfree(p);
}
