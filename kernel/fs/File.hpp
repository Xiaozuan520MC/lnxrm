#pragma once
#include <types.h>
#include <vfs.h>

/* RAII file wrapper over the VFS layer (kernel-side convenience). */
class File {
public:
    File() = default;
    explicit File(const char *path, int flags = O_RDONLY)
    {
        if (vfs_open_file(path, flags, &f_) == 0)
            ok_ = true;
    }
    ~File()
    {
        if (ok_)
            vfs_close_file(f_);
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;

    bool ok() const { return ok_; }
    size_t size() const { return vfs_file_size(f_); }

    long read(void *buf, size_t n)
    {
        return ok_ ? vfs_read_file(f_, buf, n) : -1;
    }
    long write(const void *buf, size_t n)
    {
        return ok_ ? f_->ops->write(f_, buf, n) : -1;
    }

private:
    struct file *f_ = nullptr;
    bool ok_ = false;
};
