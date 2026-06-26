#pragma once

#include <wayland-client.h>
#include <cairo/cairo.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

struct ShmBuffer {
    wl_buffer*       wl   = nullptr;
    cairo_surface_t* cs   = nullptr;
    cairo_t*         cr   = nullptr;
    std::atomic<bool> busy = false;
};

class ShmPool {
public:
    ShmPool() = default;
    ~ShmPool();

    ShmPool(const ShmPool&) = delete;
    ShmPool& operator=(const ShmPool&) = delete;

    [[nodiscard]] bool reset(wl_shm* shm, int logical_width, int logical_height, int scale);
    void destroy();

    [[nodiscard]] ShmBuffer* nextBuffer();
    void advance() { purgeRetired(); idx_ ^= 1; }

private:
    struct Retired {
        wl_shm_pool* wl_pool = nullptr;
        int          fd      = -1;
        void*        data    = nullptr;
        size_t       size    = 0;

        std::unique_ptr<ShmBuffer> bufs[2];
    };

    wl_shm*      shm_     = nullptr;
    wl_shm_pool* wl_pool_ = nullptr;
    int          fd_      = -1;
    void*        data_    = nullptr;
    size_t       size_    = 0;

    std::unique_ptr<ShmBuffer> bufs_[2];
    std::vector<Retired> retired_;

    int idx_    = 0;
    int width_  = 0;
    int height_ = 0;
    int stride_ = 0;

    void destroyActive();
    void retireActive();
    void purgeRetired();

    static bool bufferBusy(const std::unique_ptr<ShmBuffer>& b);
    static void destroyBuffer(std::unique_ptr<ShmBuffer>& b);
    static bool retiredBusy(const Retired& r);
    static void destroyRetired(Retired& r);
};
