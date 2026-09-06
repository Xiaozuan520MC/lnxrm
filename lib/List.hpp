#pragma once
#include <stddef.h>

/* Intrusive singly-linked list + tiny vector for kernel C++ code.
 * No exceptions, no RTTI, placement-new only. kmalloc/kfree are the C
 * kernel heap entry points (declared here once for all users). */

extern "C" {
void *kmalloc(size_t n);
void kfree(void *p);
}

/* freestanding placement new */
inline void *operator new(size_t, void *p) noexcept { return p; }

template <typename T>
class IntrusiveNode {
public:
    T *next_ = nullptr;
};

template <typename T, IntrusiveNode<T> T::*Member>
class IntrusiveList {
    T *head_ = nullptr;

public:
    void push_front(T *t) { (t->*Member).next_ = head_; head_ = t; }

    template <typename F>
    void for_each(F &&fn)
    {
        for (T *p = head_; p; p = (p->*Member).next_)
            fn(p);
    }
    bool empty() const { return head_ == nullptr; }
};

template <typename T>
class SimpleVec {
    T *data_ = nullptr;
    size_t len_ = 0, cap_ = 0;

public:
    ~SimpleVec()
    {
        if (data_)
            kfree(data_);
    }
    bool push_back(const T &v)
    {
        if (len_ == cap_) {
            size_t nc = cap_ ? cap_ * 2 : 8;
            T *nd = static_cast<T *>(kmalloc(nc * sizeof(T)));
            if (!nd)
                return false;
            for (size_t i = 0; i < len_; i++)
                new (&nd[i]) T(data_[i]);
            if (data_)
                kfree(data_);
            data_ = nd;
            cap_ = nc;
        }
        new (&data_[len_]) T(v);
        len_++;
        return true;
    }
    size_t size() const { return len_; }
    T &operator[](size_t i) { return data_[i]; }
};
