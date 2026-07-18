#pragma once

#include <wayland-client.h>
#include <cairo/cairo.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

struct ShmDamage {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0; }
};

struct ShmBuffer {
    wl_buffer*        wl_buffer_handle = nullptr;
    cairo_surface_t*  cairo_surface    = nullptr;
    cairo_t*          cr               = nullptr;
    std::atomic<bool> busy             = false;
    bool              content_valid    = false;
    ShmDamage         repair_damage;
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
    [[nodiscard]] ShmDamage repairDamageFor(const ShmBuffer& buffer,
                                            const ShmDamage& current_damage,
                                            int width,
                                            int height) const;
    void recordCommit(ShmBuffer* committed, const ShmDamage& damage);
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
