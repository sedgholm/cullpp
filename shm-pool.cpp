#include "shm-pool.hpp"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <algorithm>

/*hlprs*/
static constexpr size_t MAX_POOL_SIZE = 256 * 1024 * 1024; // 256 MB limit

static int make_shm_fd(size_t sz) {
    if (sz > MAX_POOL_SIZE) return -1;
    int fd = (int)syscall(SYS_memfd_create, "cullpp-pool", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)sz) < 0) { close(fd); return -1; }
    return fd;
}

static void pool_buf_destroy_one(PoolBuf& b) {
    if (b.cr) { cairo_destroy(b.cr); b.cr = nullptr; }
    if (b.cs) { cairo_surface_destroy(b.cs); b.cs = nullptr; }
    if (b.wl) { wl_buffer_destroy(b.wl);     b.wl = nullptr; }
    b.busy = false;
}



bool pool_init(Pool& p, wl_shm* shm, int w, int h, int scale) {
    if (w <= 0 || h <= 0 || scale <= 0) return false;
    const int pw = w * scale;
    const int ph = h * scale;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw);
    if (stride <= 0) return false;
    const size_t half = (size_t)stride * ph;
    if (half > SIZE_MAX / 2) return false;
    const size_t need = half * 2;


    pool_buf_destroy_one(p.bufs[0]);
    pool_buf_destroy_one(p.bufs[1]);


    if (p.fd < 0) {
        
        p.fd = make_shm_fd(need);
        if (p.fd < 0) return false;
        p.data = mmap(nullptr, need, PROT_READ|PROT_WRITE, MAP_SHARED, p.fd, 0);
        if (p.data == MAP_FAILED) {
            close(p.fd); p.fd = -1; p.data = nullptr;
            return false;
        }
        p.size = need;
        p.shm  = shm;
        p.wl_pool = wl_shm_create_pool(shm, p.fd, (int32_t)need);
        if (!p.wl_pool) {
            munmap(p.data, p.size);
            close(p.fd);
            p.data = nullptr;
            p.fd = -1;
            p.size = 0;
            return false;
        }
    } else if (need > p.size) {
       
        if (ftruncate(p.fd, (off_t)need) < 0) return false;
        void* old_data = p.data;
        size_t old_size = p.size;
        void* nd = mremap(p.data, p.size, need, MREMAP_MAYMOVE);
        if (nd == MAP_FAILED) {

            ftruncate(p.fd, (off_t)old_size);
            return false;
        }
        p.data = nd;
        p.size = need;

        if (madvise(old_data, old_size, MADV_DONTNEED) < 0) { /* ignore */ }
        wl_shm_pool_resize(p.wl_pool, (int32_t)need);
    }



    p.w      = pw;
    p.h      = ph;
    p.stride = stride;


    for (int i = 0; i < 2; i++) {
        const int32_t offset = (int32_t)(i * half);
        p.bufs[i].wl = wl_shm_pool_create_buffer(
            p.wl_pool, offset, pw, ph, stride, WL_SHM_FORMAT_ARGB8888);
        if (!p.bufs[i].wl) {

            for (int j = 0; j < i; j++) pool_buf_destroy_one(p.bufs[j]);
            return false;
        }
        wl_buffer_add_listener(p.bufs[i].wl, &pool_buf_listener, &p.bufs[i]);

 
        p.bufs[i].cs = cairo_image_surface_create_for_data(
            (unsigned char*)p.data + offset,
            CAIRO_FORMAT_ARGB32, pw, ph, stride);
        if (!p.bufs[i].cs ||
            cairo_surface_status(p.bufs[i].cs) != CAIRO_STATUS_SUCCESS) {
  
            for (int j = 0; j < i; j++) pool_buf_destroy_one(p.bufs[j]);
            return false;
        }
        p.bufs[i].cr = cairo_create(p.bufs[i].cs);
        if (!p.bufs[i].cr || cairo_status(p.bufs[i].cr) != CAIRO_STATUS_SUCCESS) {
            for (int j = 0; j <= i; j++) pool_buf_destroy_one(p.bufs[j]);
            return false;
        }
        p.bufs[i].busy = false;
    }

    p.idx = 0;
    return true;
}

void pool_destroy(Pool& p) {
    pool_buf_destroy_one(p.bufs[0]);
    pool_buf_destroy_one(p.bufs[1]);
    if (p.wl_pool) { wl_shm_pool_destroy(p.wl_pool); p.wl_pool = nullptr; }
    if (p.data && p.data != MAP_FAILED) {
        munmap(p.data, p.size); p.data = nullptr;
    }
    if (p.fd >= 0) { close(p.fd); p.fd = -1; }
    p.size = 0; p.w = 0; p.h = 0; p.stride = 0;
}

PoolBuf* pool_next_buf(Pool& p) {
    PoolBuf* candidate = &p.bufs[p.idx];
    bool expected = false;
    if (candidate->busy.compare_exchange_strong(expected, true)) {
        return candidate;
    }
    p.idx ^= 1;
    candidate = &p.bufs[p.idx];
    expected = false;
    if (candidate->busy.compare_exchange_strong(expected, true)) {
        return candidate;
    }
    return nullptr;   // both in-flight
}
