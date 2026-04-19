#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

#include "image_loader.h"
#include "net.h"
#include <cstring>

static constexpr int    MAX_DIM      = 256;
static constexpr size_t MAX_BYTES    = 2u * 1024u * 1024u;
static constexpr size_t WORKER_STACK = 64u * 1024u;

static void bilinearResize(const uint8_t* src, int sw, int sh,
                            uint8_t* dst, int dw, int dh) {
    for (int dy = 0; dy < dh; ++dy) {
        float fy = ((float)dy + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = (int)fy; if (y0 < 0) y0 = 0; if (y0 > sh - 1) y0 = sh - 1;
        int y1 = y0 + 1;  if (y1 > sh - 1) y1 = sh - 1;
        float wy = fy - (float)y0; if (wy < 0.0f) wy = 0.0f; if (wy > 1.0f) wy = 1.0f;
        for (int dx = 0; dx < dw; ++dx) {
            float fx = ((float)dx + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = (int)fx; if (x0 < 0) x0 = 0; if (x0 > sw - 1) x0 = sw - 1;
            int x1 = x0 + 1;  if (x1 > sw - 1) x1 = sw - 1;
            float wx = fx - (float)x0; if (wx < 0.0f) wx = 0.0f; if (wx > 1.0f) wx = 1.0f;
            const uint8_t* p00 = src + (y0 * sw + x0) * 4;
            const uint8_t* p10 = src + (y0 * sw + x1) * 4;
            const uint8_t* p01 = src + (y1 * sw + x0) * 4;
            const uint8_t* p11 = src + (y1 * sw + x1) * 4;
            uint8_t* d = dst + (dy * dw + dx) * 4;
            for (int c = 0; c < 4; ++c) {
                float v = (float)p00[c] * (1.0f - wx) * (1.0f - wy)
                        + (float)p10[c] * wx          * (1.0f - wy)
                        + (float)p01[c] * (1.0f - wx) * wy
                        + (float)p11[c] * wx          * wy;
                int iv = (int)(v + 0.5f);
                if (iv < 0)   iv = 0;
                if (iv > 255) iv = 255;
                d[c] = (uint8_t)iv;
            }
        }
    }
}

static void processOne(const std::string& url, DecodedImage& out) {
    std::string err;
    std::vector<uint8_t> bin = httpGetBinary(url, MAX_BYTES, err);
    if (bin.empty()) { out.failed = true; return; }

    int w = 0, h = 0, ch = 0;
    uint8_t* px = stbi_load_from_memory(bin.data(), (int)bin.size(),
                                         &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        out.failed = true;
        return;
    }

    int dw, dh;
    if (w >= h) {
        dw = (w > MAX_DIM) ? MAX_DIM : w;
        dh = h * dw / w;
    } else {
        dh = (h > MAX_DIM) ? MAX_DIM : h;
        dw = w * dh / h;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    out.imgW = dw;
    out.imgH = dh;
    out.rgba.resize((size_t)dw * (size_t)dh * 4u);
    if (dw == w && dh == h) {
        memcpy(out.rgba.data(), px, (size_t)dw * (size_t)dh * 4u);
    } else {
        bilinearResize(px, w, h, out.rgba.data(), dw, dh);
    }
    stbi_image_free(px);
}

ImageLoader::ImageLoader() {
    LightLock_Init(&lock_);
    LightEvent_Init(&wakeup_, RESET_STICKY);
}

ImageLoader::~ImageLoader() {
    stop();
}

void ImageLoader::start() {
    if (running_) return;
    stop_    = false;
    running_ = true;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thread_ = threadCreate(trampoline, this, WORKER_STACK, prio + 1, -2, false);
    if (!thread_) {
        running_ = false;
    }
}

void ImageLoader::stop() {
    if (!running_) return;
    stop_ = true;
    LightEvent_Signal(&wakeup_);
    if (thread_) {
        threadJoin(thread_, U64_MAX);
        threadFree(thread_);
        thread_ = nullptr;
    }
    running_ = false;

    LightLock_Lock(&lock_);
    jobs_.clear();
    results_.clear();
    LightLock_Unlock(&lock_);
}

void ImageLoader::submit(const std::string& url) {
    LightLock_Lock(&lock_);
    jobs_.push_back(url);
    LightLock_Unlock(&lock_);
    LightEvent_Signal(&wakeup_);
}

void ImageLoader::cancelAll() {
    LightLock_Lock(&lock_);
    jobs_.clear();
    LightLock_Unlock(&lock_);
}

bool ImageLoader::poll(DecodedImage& out) {
    LightLock_Lock(&lock_);
    if (results_.empty()) {
        LightLock_Unlock(&lock_);
        return false;
    }
    out = std::move(results_.front());
    results_.pop_front();
    LightLock_Unlock(&lock_);
    return true;
}

void ImageLoader::trampoline(void* self) {
    static_cast<ImageLoader*>(self)->workerMain();
}

void ImageLoader::workerMain() {
    while (!stop_) {
        std::string url;
        LightLock_Lock(&lock_);
        if (jobs_.empty()) {
            LightLock_Unlock(&lock_);
            LightEvent_Wait(&wakeup_);
            LightEvent_Clear(&wakeup_);
            continue;
        }
        url = std::move(jobs_.front());
        jobs_.pop_front();
        LightLock_Unlock(&lock_);

        if (stop_) return;

        DecodedImage out;
        out.url = url;
        processOne(url, out);

        LightLock_Lock(&lock_);
        results_.push_back(std::move(out));
        LightLock_Unlock(&lock_);
    }
}
