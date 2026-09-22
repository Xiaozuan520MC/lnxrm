#pragma once
#include <stddef.h>

/* Fixed-size object cache: alloc/free of same-sized objects without
 * hitting the general heap every time. */
class SlabBase {
public:
    void *operator new(size_t sz);
    void operator delete(void *p);
};

template <typename T, int N>
class SlabCache {
    struct Slot {
        union {
            Slot *next;
            char storage[sizeof(T)];
        };
        bool used;
    };
    Slot pool[N];
    Slot *free_head = nullptr;

public:
    SlabCache()
    {
        free_head = &pool[0];
        for (int i = 0; i + 1 < N; i++) {
            pool[i].used = false;
            pool[i].next = &pool[i + 1];
        }
        pool[N - 1].next = nullptr;
        pool[N - 1].used = false;
    }

    T *alloc()
    {
        if (!free_head)
            return nullptr;
        Slot *s = free_head;
        free_head = s->next;
        s->used = true;
        return reinterpret_cast<T *>(&s->storage);
    }

    void free_obj(T *t)
    {
        /* O(1): compute index from pointer arithmetic into pool[] */
        Slot *s = reinterpret_cast<Slot *>(reinterpret_cast<char *>(t)
                  - offsetof(Slot, storage));
        s->used = false;
        s->next = free_head;
        free_head = s;
    }
};
