#include "shm-pool.hpp"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>

#include <cstdint>
#include <fcntl.h>
#include <algorithm>
#include <limits>

static constexpr size_t kMaxPoolSize = 256 * 1024 * 1024;

static int createShmFd(size_t size_bytes) {
    if (size_bytes == 0 || size_bytes > kMaxPoolSize ||
        size_bytes > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return -1;

    int fd = static_cast<int>(syscall(SYS_memfd_create, "cullpp-pool", MFD_CLOEXEC));
    if (fd < 0) {
        const int e = errno;
        LOG_ERR("memfd_create", "", "\"cullpp-pool\"  = -1 %s (%s)", dlog::errName(e), strerror(e));
        return -1;
    }

    const double t0 = dlog::level >= 1 ? dlog::nowMs() : 0.0;
    const int fa = posix_fallocate(fd, 0, static_cast<off_t>(size_bytes));   /* returns the errno itself */
    if (fa != 0) {
        LOG_ERR("fallocate", "", "%zu  = -1 %s (%s)", size_bytes, dlog::errName(fa), strerror(fa));
        close(fd);
        return -1;
    }
    LOG_V("fallocate", "", "%zu  = 0  <%.6f>", size_bytes, dlog::since(t0));

    return fd;
}

static void onBufferRelease(void* data, wl_buffer*) {
    auto* b = static_cast<ShmBuffer*>(data);
    b->busy.store(false, std::memory_order_release);
}

static const wl_buffer_listener buffer_listener = { .release = onBufferRelease };

static bool tryScaleDimension(int value, int scale, int* result) {
    if (value <= 0 || scale <= 0) return false;

    const auto unsigned_value = static_cast<size_t>(value);
    const auto unsigned_scale = static_cast<size_t>(scale);
    const auto max_value = static_cast<size_t>(std::numeric_limits<int>::max());
    if (unsigned_value > max_value / unsigned_scale) return false;

    *result = static_cast<int>(unsigned_value * unsigned_scale);
    return true;
}

static bool tryMultiplySize(size_t a, size_t b, size_t* result) {
    const size_t max_value = std::numeric_limits<size_t>::max();
    if (a != 0 && b > max_value / a) return false;

    *result = a * b;
    return true;
}

static void destroyStorage(wl_shm_pool*& shm_pool_handle, int& fd, void*& data, size_t& size) {
    if (shm_pool_handle) {
        wl_shm_pool_destroy(shm_pool_handle);
        shm_pool_handle = nullptr;
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

bool ShmPool::isBufferBusy(const std::unique_ptr<ShmBuffer>& b) {
    return b && b->busy.load(std::memory_order_acquire);
}

void ShmPool::destroyBuffer(std::unique_ptr<ShmBuffer>& b) {
    if (!b) return;

    if (b->cr) {
        cairo_destroy(b->cr);
        b->cr = nullptr;
    }
    if (b->cairo_surface) {
        cairo_surface_destroy(b->cairo_surface);
        b->cairo_surface = nullptr;
    }
    if (b->wl_buffer_handle) {
        wl_buffer_destroy(b->wl_buffer_handle);
        b->wl_buffer_handle = nullptr;
    }

    b->busy.store(false, std::memory_order_release);
    b.reset();
}

bool ShmPool::isRetiredBusy(const Retired& r) {
    for (int i = 0; i < kBufferCount; ++i)
        if (isBufferBusy(r.buffers[i])) return true;
    return false;
}

void ShmPool::destroyRetired(Retired& r) {
    for (int i = 0; i < kBufferCount; ++i)
        destroyBuffer(r.buffers[i]);
    destroyStorage(r.shm_pool_handle, r.fd, r.data, r.size);
}

void ShmPool::purgeRetired() {
    for (auto it = retired_.begin(); it != retired_.end(); ) {
        if (isRetiredBusy(*it)) {
            ++it;
            continue;
        }

        destroyRetired(*it);
        it = retired_.erase(it);
    }
}

void ShmPool::destroyActive() {
    for (int i = 0; i < kBufferCount; ++i)
        destroyBuffer(active_buffers_[i]);
    destroyStorage(shm_pool_handle_, fd_, data_, size_);

    next_index_ = 0;
    width_ = 0;
    height_ = 0;
    stride_ = 0;
}

void ShmPool::retireActive() {
    if (!shm_pool_handle_ && fd_ < 0 && !data_) return;

    bool any_busy = false;
    for (int i = 0; i < kBufferCount; ++i)
        any_busy |= isBufferBusy(active_buffers_[i]);

    if (!any_busy) {
        destroyActive();
        return;
    }

    Retired old;
    old.shm_pool_handle = shm_pool_handle_;
    old.fd = fd_;
    old.data = data_;
    old.size = size_;
    for (int i = 0; i < kBufferCount; ++i)
        old.buffers[i] = std::move(active_buffers_[i]);

    shm_pool_handle_ = nullptr;
    fd_ = -1;
    data_ = nullptr;
    size_ = 0;
    next_index_ = 0;
    width_ = 0;
    height_ = 0;
    stride_ = 0;

    retired_.push_back(std::move(old));
}

bool ShmPool::reset(wl_shm* shm, int logical_width, int logical_height, int scale) {
    if (!shm) return false;

    int width = 0;
    int height = 0;
    if (!tryScaleDimension(logical_width, scale, &width) ||
        !tryScaleDimension(logical_height, scale, &height))
        return false;

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (stride <= 0) return false;

    // align stride to 256 for optimal compositor udmabuf import
		// see: https://zamundaaa.github.io/wayland/2026/05/06/making-wl-shm-fast.html
		// see MR: https://invent.kde.org/plasma/kwin/-/merge_requests/9178
    constexpr int kStrideAlign = 256;
    if (stride % kStrideAlign != 0)
        stride = ((stride / kStrideAlign) + 1) * kStrideAlign;

    size_t per_buffer_size = 0;
    size_t total_pool_size = 0;
    if (!tryMultiplySize(static_cast<size_t>(stride), static_cast<size_t>(height), &per_buffer_size) ||
        !tryMultiplySize(per_buffer_size, kBufferCount, &total_pool_size))
        return false;

    // align per-buffer size to page boundary for compositor zero-copy import.
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0 && per_buffer_size % static_cast<size_t>(page_size) != 0)
        per_buffer_size = ((per_buffer_size + static_cast<size_t>(page_size) - 1) / static_cast<size_t>(page_size)) * static_cast<size_t>(page_size);
    total_pool_size = per_buffer_size * kBufferCount;

    if (total_pool_size > kMaxPoolSize ||
        total_pool_size > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return false;

    purgeRetired();

    if (fd_ >= 0 && shm_ == shm && width_ == width && height_ == height &&
        stride_ == stride &&
        active_buffers_[0] && active_buffers_[1] && active_buffers_[2])
        return true;

    retireActive();

    fd_ = createShmFd(total_pool_size);
    if (fd_ < 0) return false;

    data_ = mmap(nullptr, total_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        const int e = errno;
        LOG_ERR("mmap", "", "%zu  = -1 %s (%s)", total_pool_size, dlog::errName(e), strerror(e));
        close(fd_);
        fd_ = -1;
        data_ = nullptr;
        return false;
    }

    LOG_V("mmap", "", "%zu  = %p", total_pool_size, data_);
    size_ = total_pool_size;
    shm_ = shm;
    shm_pool_handle_ = wl_shm_create_pool(shm, fd_, static_cast<int32_t>(total_pool_size));
    if (!shm_pool_handle_) {
        destroyActive();
        return false;
    }

    width_ = width;
    height_ = height;
    stride_ = stride;

    for (int i = 0; i < kBufferCount; ++i) {
        const int32_t offset = static_cast<int32_t>(static_cast<size_t>(i) * per_buffer_size);

        active_buffers_[i] = std::make_unique<ShmBuffer>();
        ShmBuffer& b = *active_buffers_[i];
        b.index = i;

        b.wl_buffer_handle = wl_shm_pool_create_buffer(
            shm_pool_handle_, offset, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        if (!b.wl_buffer_handle) {
            destroyActive();
            return false;
        }
        wl_buffer_add_listener(b.wl_buffer_handle, &buffer_listener, &b);

        b.cairo_surface = cairo_image_surface_create_for_data(
            static_cast<unsigned char*>(data_) + offset,
            CAIRO_FORMAT_ARGB32, width, height, stride);
        if (!b.cairo_surface || cairo_surface_status(b.cairo_surface) != CAIRO_STATUS_SUCCESS) {
            destroyActive();
            return false;
        }

        b.cr = cairo_create(b.cairo_surface);
        if (!b.cr || cairo_status(b.cr) != CAIRO_STATUS_SUCCESS) {
            destroyActive();
            return false;
        }


        if (cairo_font_options_t* fo = cairo_font_options_create()) {
            cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
            cairo_set_font_options(b.cr, fo);
            cairo_font_options_destroy(fo);
        }

        b.busy.store(false, std::memory_order_release);
    }

    next_index_ = 0;
    return true;
}

void ShmPool::destroy() {
    destroyActive();

    for (auto& old : retired_)
        destroyRetired(old);
    retired_.clear();
}

ShmBuffer* ShmPool::acquireNextBuffer() {
    purgeRetired();
    for (int i = 0; i < kBufferCount; ++i)
        if (!active_buffers_[i]) return nullptr;

    for (int i = 0; i < kBufferCount; ++i) {
        const int idx = (next_index_ + i) % kBufferCount;
        ShmBuffer* candidate = active_buffers_[idx].get();
        bool expected = false;
        if (candidate->busy.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return candidate;
    }

    return nullptr;
}

void ShmPool::repaintRegionFor(const ShmBuffer& buffer, const pixman_region32_t* damage,
                               int width, int height, pixman_region32_t* out) const {
    if (!buffer.content_valid) {
        pixman_region32_clear(out);
        if (width > 0 && height > 0)
            pixman_region32_union_rect(out, out, 0, 0,
                static_cast<unsigned>(width), static_cast<unsigned>(height));
        return;
    }
    pixman_region32_union(out, damage, &buffer.repair);
}

void ShmPool::recordCommit(ShmBuffer* committed, const pixman_region32_t* damage) {
    for (auto& buffer : active_buffers_) {
        if (!buffer || buffer.get() == committed) continue;
        if (buffer->content_valid)
            pixman_region32_union(&buffer->repair, &buffer->repair, damage);
    }

    if (committed) {
        committed->content_valid = true;
        pixman_region32_clear(&committed->repair);
    }
}
