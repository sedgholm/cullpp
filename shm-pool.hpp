#pragma once
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include <wayland-client.h>
#include <cairo/cairo.h>
#include <pixman.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

/* event logstderr columns time:  event:  where:  rest: */
namespace dlog {

inline int    level = 0;      // 0=0 1=1
inline double t_zero = 0.0;

inline double nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
}

inline void start() { t_zero = nowMs(); }

inline double since(double t_ms) { return (nowMs() - t_ms) / 1000.0; }

inline const char* errName(int e) {
    switch (e) {
    case EPERM:  return "EPERM";   case ENOENT: return "ENOENT"; case EINTR:  return "EINTR";
    case EIO:    return "EIO";     case EBADF:  return "EBADF";   case EAGAIN: return "EAGAIN";
    case ENOMEM: return "ENOMEM";  case EACCES: return "EACCES";  case EBUSY:  return "EBUSY";
    case ENODEV: return "ENODEV";  case EINVAL: return "EINVAL";  case ENFILE: return "ENFILE";
    case EMFILE: return "EMFILE";  case EFBIG:  return "EFBIG";   case ENOSPC: return "ENOSPC";
    case ENOSYS: return "ENOSYS";  case EOPNOTSUPP: return "EOPNOTSUPP";
    default:     return "E?";
    }
}

constexpr int kEventWidth = 9;
constexpr int kWhereWidth = 11;   // "HEADLESS-1" p1

/* one write(2). stays well under PIPE_BUF (4096). */
__attribute__((format(printf, 3, 4)))
inline void line(const char* event, const char* where, const char* fmt, ...) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%9.3f  %-*s %-*s ",
                     nowMs() - t_zero, kEventWidth, event, kWhereWidth, where);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof buf)) n = static_cast<int>(sizeof buf) - 1;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - static_cast<size_t>(n), fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n >= static_cast<int>(sizeof buf) - 1) {            // truncated: mark it, keep the newline
        n = static_cast<int>(sizeof buf) - 1;
        memcpy(buf + n - 3, "...", 3);
    }
    while (n > 0 && buf[n - 1] == ' ') --n;                  // no trailing blanks when 'rest' is empty
    buf[n++] = '\n';

    const char* p = buf;
    while (n > 0) {
        ssize_t w = write(STDERR_FILENO, p, static_cast<size_t>(n));
        if (w < 0) { if (errno == EINTR) continue; return; }
        p += w; n -= static_cast<int>(w);
    }
}

} // namespace dlog

// when -v
#define LOG_V(event, where, ...)  do { if (dlog::level >= 1) dlog::line(event, where, __VA_ARGS__); } while (0)

#define LOG_ERR(event, where, ...) dlog::line(event, where, __VA_ARGS__)

struct ShmBuffer {
    int               index            = 0;   // slot in the pool, for the log
    wl_buffer*        wl_buffer_handle = nullptr;
    cairo_surface_t*  cairo_surface    = nullptr;
    cairo_t*          cr               = nullptr;
    std::atomic<bool> busy             = false;
    bool              content_valid    = false;
    /* damage committed to *other* buffers since this one was last painted ("buffer age") */
    pixman_region32_t repair;

    ShmBuffer()  { pixman_region32_init(&repair); }
    ~ShmBuffer() { pixman_region32_fini(&repair); }
    ShmBuffer(const ShmBuffer&) = delete;
    ShmBuffer& operator=(const ShmBuffer&) = delete;
};

class ShmPool {
    static constexpr int kBufferCount = 3;
public:
    ShmPool() = default;
    ~ShmPool();

    ShmPool(const ShmPool&) = delete;
    ShmPool& operator=(const ShmPool&) = delete;

    [[nodiscard]] bool reset(wl_shm* shm, int logical_width, int logical_height, int scale);
    void destroy();

    [[nodiscard]] ShmBuffer* acquireNextBuffer();



    void repaintRegionFor(const ShmBuffer& buffer, const pixman_region32_t* damage,
                          int width, int height, pixman_region32_t* out) const;
    void recordCommit(ShmBuffer* committed, const pixman_region32_t* damage);
    void advanceIndex() { purgeRetired(); if (++next_index_ >= kBufferCount) next_index_ = 0; }

private:
    struct Retired {
        wl_shm_pool* shm_pool_handle = nullptr;
        int          fd              = -1;
        void*        data            = nullptr;
        size_t       size            = 0;

        std::unique_ptr<ShmBuffer> buffers[kBufferCount];
    };

    wl_shm*      shm_     = nullptr;
    wl_shm_pool* shm_pool_handle_ = nullptr;
    int          fd_      = -1;
    void*        data_    = nullptr;
    size_t       size_    = 0;

    std::unique_ptr<ShmBuffer> active_buffers_[kBufferCount];
    std::vector<Retired> retired_;

    int next_index_ = 0;
    int width_      = 0;
    int height_     = 0;
    int stride_     = 0;

    void destroyActive();
    void retireActive();
    void purgeRetired();

    static bool isBufferBusy(const std::unique_ptr<ShmBuffer>& b);
    static void destroyBuffer(std::unique_ptr<ShmBuffer>& b);
    static bool isRetiredBusy(const Retired& r);
    static void destroyRetired(Retired& r);
};
