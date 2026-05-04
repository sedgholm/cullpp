#pragma once
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <cstddef>
#include <atomic>


struct PoolBuf {
    wl_buffer*       wl   = nullptr;
    cairo_surface_t* cs   = nullptr;
    std::atomic<bool> busy = false;
};




struct Pool {
    wl_shm*      shm     = nullptr;
    wl_shm_pool* wl_pool = nullptr;
    int          fd      = -1;
    void*        data    = nullptr;
    size_t       size    = 0;   // current mmap size (= 2 * stride * h)

    PoolBuf bufs[2];
    int     idx    = 0;
    int     w      = 0;
    int     h      = 0;
    int     stride = 0;
};

static void pool_buf_release(void* d, wl_buffer*) {
    reinterpret_cast<PoolBuf*>(d)->busy = false;
}
static const wl_buffer_listener pool_buf_listener = { .release = pool_buf_release };

/* create or resize the pool for logical size w×h at the given HiDPI scale.
 * physical buffer = (w*scale) × (h*scale). */
[[nodiscard]] bool pool_init(Pool& p, wl_shm* shm, int w, int h, int scale);


void pool_destroy(Pool& p);


PoolBuf* pool_next_buf(Pool& p);
