#include "shm-pool.hpp"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>

#include <cstdint>
#include <fcntl.h>
#include <limits>

static constexpr size_t MAX_POOL_SIZE = 64 * 1024 * 1024;

static int make_shm_fd(size_t sz) {
    if (sz == 0 || sz > MAX_POOL_SIZE ||
        sz > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return -1;

    int fd = static_cast<int>(syscall(SYS_memfd_create, "cullpp-pool", MFD_CLOEXEC));
    if (fd < 0) return -1;

    if (posix_fallocate(fd, 0, static_cast<off_t>(sz)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void buffer_release(void* data, wl_buffer*) {
    auto* b = static_cast<ShmBuffer*>(data);
    b->busy.store(false, std::memory_order_release);
}

static const wl_buffer_listener buffer_listener = { .release = buffer_release };

static bool checked_scale(int n, int scale, int* out) {
    if (n <= 0 || scale <= 0) return false;

    const auto un = static_cast<size_t>(n);
    const auto us = static_cast<size_t>(scale);
    const auto max = static_cast<size_t>(std::numeric_limits<int>::max());
    if (un > max / us) return false;

    *out = static_cast<int>(un * us);
    return true;
}

static bool checked_mul(size_t a, size_t b, size_t* out) {
    const size_t max = std::numeric_limits<size_t>::max();
    if (a != 0 && b > max / a) return false;

    *out = a * b;
    return true;
}

static void destroy_storage(wl_shm_pool*& wl_pool, int& fd, void*& data, size_t& size) {
    if (wl_pool) {
        wl_shm_pool_destroy(wl_pool);
        wl_pool = nullptr;
    }
    if (data && data != MAP_FAILED) {
        munmap(data, size);
        data = nullptr;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    size = 0;
}

ShmPool::~ShmPool() {
    destroy();
}

bool ShmPool::bufferBusy(const std::unique_ptr<ShmBuffer>& b) {
    return b && b->busy.load(std::memory_order_acquire);
}

void ShmPool::destroyBuffer(std::unique_ptr<ShmBuffer>& b) {
    if (!b) return;

    if (b->cr) {
        cairo_destroy(b->cr);
        b->cr = nullptr;
    }
    if (b->cs) {
        cairo_surface_destroy(b->cs);
        b->cs = nullptr;
    }
    if (b->wl) {
        wl_buffer_destroy(b->wl);
        b->wl = nullptr;
    }

    b->busy.store(false, std::memory_order_release);
    b.reset();
}

bool ShmPool::retiredBusy(const Retired& r) {
    return bufferBusy(r.bufs[0]) || bufferBusy(r.bufs[1]);
}

void ShmPool::destroyRetired(Retired& r) {
    destroyBuffer(r.bufs[0]);
    destroyBuffer(r.bufs[1]);
    destroy_storage(r.wl_pool, r.fd, r.data, r.size);
}

void ShmPool::purgeRetired() {
    for (auto it = retired_.begin(); it != retired_.end(); ) {
        if (retiredBusy(*it)) {
            ++it;
            continue;
        }

        destroyRetired(*it);
        it = retired_.erase(it);
    }
}

void ShmPool::destroyActive() {
    destroyBuffer(bufs_[0]);
    destroyBuffer(bufs_[1]);
    destroy_storage(wl_pool_, fd_, data_, size_);

    idx_ = 0;
    width_ = 0;
    height_ = 0;
    stride_ = 0;
}

void ShmPool::retireActive() {
    if (!wl_pool_ && fd_ < 0 && !data_) return;

    if (!bufferBusy(bufs_[0]) && !bufferBusy(bufs_[1])) {
        destroyActive();
        return;
    }

    Retired old;
    old.wl_pool = wl_pool_;
    old.fd = fd_;
    old.data = data_;
    old.size = size_;
    old.bufs[0] = std::move(bufs_[0]);
    old.bufs[1] = std::move(bufs_[1]);

    wl_pool_ = nullptr;
    fd_ = -1;
    data_ = nullptr;
    size_ = 0;
    idx_ = 0;
    width_ = 0;
    height_ = 0;
    stride_ = 0;

    retired_.push_back(std::move(old));
}

bool ShmPool::reset(wl_shm* shm, int logical_width, int logical_height, int scale) {
    if (!shm) return false;

    int width = 0;
    int height = 0;
    if (!checked_scale(logical_width, scale, &width) ||
        !checked_scale(logical_height, scale, &height))
        return false;

    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (stride <= 0) return false;

    size_t half = 0;
    size_t need = 0;
    if (!checked_mul(static_cast<size_t>(stride), static_cast<size_t>(height), &half) ||
        !checked_mul(half, 2, &need) ||
        need > MAX_POOL_SIZE ||
        need > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return false;

    purgeRetired();

    if (fd_ >= 0 && shm_ == shm && width_ == width && height_ == height &&
        stride_ == stride && bufs_[0] && bufs_[1])
        return true;

    retireActive();

    fd_ = make_shm_fd(need);
    if (fd_ < 0) return false;

    data_ = mmap(nullptr, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        close(fd_);
        fd_ = -1;
        data_ = nullptr;
        return false;
    }

    size_ = need;
    shm_ = shm;
    wl_pool_ = wl_shm_create_pool(shm, fd_, static_cast<int32_t>(need));
    if (!wl_pool_) {
        destroyActive();
        return false;
    }

    width_ = width;
    height_ = height;
    stride_ = stride;

    for (int i = 0; i < 2; ++i) {
        const int32_t offset = static_cast<int32_t>(static_cast<size_t>(i) * half);

        bufs_[i] = std::make_unique<ShmBuffer>();
        ShmBuffer& b = *bufs_[i];

        b.wl = wl_shm_pool_create_buffer(
            wl_pool_, offset, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        if (!b.wl) {
            destroyActive();
            return false;
        }
        wl_buffer_add_listener(b.wl, &buffer_listener, &b);

        b.cs = cairo_image_surface_create_for_data(
            static_cast<unsigned char*>(data_) + offset,
            CAIRO_FORMAT_ARGB32, width, height, stride);
        if (!b.cs || cairo_surface_status(b.cs) != CAIRO_STATUS_SUCCESS) {
            destroyActive();
            return false;
        }

        b.cr = cairo_create(b.cs);
        if (!b.cr || cairo_status(b.cr) != CAIRO_STATUS_SUCCESS) {
            destroyActive();
            return false;
        }

        b.busy.store(false, std::memory_order_release);
    }

    idx_ = 0;
    return true;
}

void ShmPool::destroy() {
    destroyActive();

    for (auto& old : retired_)
        destroyRetired(old);
    retired_.clear();
}

ShmBuffer* ShmPool::nextBuffer() {
    purgeRetired();
    if (!bufs_[0] || !bufs_[1]) return nullptr;

    ShmBuffer* candidate = bufs_[idx_].get();
    bool expected = false;
    if (candidate->busy.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return candidate;

    idx_ ^= 1;
    candidate = bufs_[idx_].get();
    expected = false;
    if (candidate->busy.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return candidate;

    return nullptr;
}
