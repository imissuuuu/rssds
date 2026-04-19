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

struct XferCbCtx { ImageLoader* loader; const std::string* url; };

/**
 * @brief Transfer progress callback that records download progress for a specific URL.
 *
 * Casts the user data to an XferCbCtx and calls the associated ImageLoader's setProgress with
 * the current downloaded and total bytes for the URL contained in the context.
 *
 * @param ud Pointer to an XferCbCtx containing the target ImageLoader and URL.
 * @param dltotal Total number of bytes expected for the transfer (may be <= 0 if unknown).
 * @param dlnow  Number of bytes downloaded so far.
 * @return int Always returns 0 to indicate the transfer should continue.
 */
static int xferInfoCb(void* ud, int64_t dltotal, int64_t dlnow) {
    auto* ctx = static_cast<XferCbCtx*>(ud);
    ctx->loader->setProgress(*ctx->url, dlnow, dltotal);
    return 0;
}

static constexpr int    MAX_DIM      = 256;
static constexpr size_t MAX_BYTES    = 2u * 1024u * 1024u;
static constexpr size_t WORKER_STACK = 64u * 1024u;

/**
 * @brief Resize an RGBA image using bilinear interpolation.
 *
 * Resamples the source image to the destination dimensions with bilinear filtering,
 * producing RGBA output with per-channel values clamped to [0, 255].
 *
 * @param src Pointer to source pixel data in row-major order, 4 bytes per pixel (RGBA).
 *            Must contain at least sw * sh * 4 bytes.
 * @param sw Source image width in pixels (must be >= 1).
 * @param sh Source image height in pixels (must be >= 1).
 * @param dst Pointer to destination buffer, 4 bytes per pixel (RGBA).
 *            Must have space for at least dw * dh * 4 bytes.
 * @param dw Destination image width in pixels (must be >= 1).
 * @param dh Destination image height in pixels (must be >= 1).
 */
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

/**
 * @brief Downloads an image from a URL, decodes it to RGBA, and stores a resized result in `out`.
 *
 * Downloads up to MAX_BYTES from `url`, decodes the image forcing 4-channel RGBA, scales it so the
 * larger dimension is at most MAX_DIM while preserving aspect ratio (minimum dimension 1), and writes
 * pixels into `out.rgba` with `out.imgW`/`out.imgH` set accordingly. On failure `out.failed` is set.
 *
 * @param url Source URL of the image to download.
 * @param out Output container that will be populated with the decoded/resized image or marked failed.
 * @param xferFn Optional transfer-progress callback invoked during download; may be nullptr.
 * @param xferUd Opaque user pointer forwarded to `xferFn`.
 */
static void processOne(const std::string& url, DecodedImage& out,
                        XferInfoFn xferFn, void* xferUd, int maxDim) {
    std::string err;
    std::vector<uint8_t> bin = httpGetBinary(url, MAX_BYTES, err, xferFn, xferUd);
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
        dw = (w > maxDim) ? maxDim : w;
        dh = h * dw / w;
    } else {
        dh = (h > maxDim) ? maxDim : h;
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

/**
 * @brief Initializes the ImageLoader and its synchronization primitives.
 *
 * Initializes internal locks used to protect job/result and progress state,
 * and initializes the wakeup event used to coordinate the worker thread.
 */
ImageLoader::ImageLoader() {
    LightLock_Init(&lock_);
    LightLock_Init(&progressLock_);
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

/**
 * @brief Cancel all pending image load requests and clear progress state.
 *
 * Removes all queued jobs and resets stored per-URL progress percentages.
 * This operation does not abort a job that is already being processed by the worker.
 *
 * @note The function acquires the loader's internal lock while clearing the pending job queue.
 */
void ImageLoader::cancelAll() {
    LightLock_Lock(&lock_);
    jobs_.clear();
    LightLock_Unlock(&lock_);
    clearProgress();
}

/**
 * Retrieves the next completed decoded image from the loader's result queue, if any.
 *
 * If a result is available, moves it into `out` and removes it from the internal queue.
 *
 * @param[out] out Destination for the next DecodedImage when one is available.
 * @return `true` if `out` was set to a moved result, `false` if no result was available.
 */
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

/**
 * @brief Worker loop that processes queued image download and decode jobs.
 *
 * Continuously consumes URLs from the internal job queue, downloads and decodes
 * each image (with progress reported), and appends the resulting DecodedImage to
 * the results queue until the loader is stopped.
 *
 * This method blocks waiting on the internal wakeup event when no jobs are
 * available and respects the loader's stop flag to terminate promptly.
 */
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
        XferCbCtx pctx { this, &url };
        processOne(url, out, xferInfoCb, &pctx, maxDim_);

        LightLock_Lock(&lock_);
        results_.push_back(std::move(out));
        LightLock_Unlock(&lock_);
    }
}

/**
 * @brief Update stored download progress for a specific URL as a fraction between 0 and 1.
 *
 * Computes a progress fraction `pct` from `dlnow` and `dltotal` (uses `dlnow / dltotal` when
 * `dltotal > 0`, otherwise approximates with `dlnow / MAX_BYTES`), clamps `pct` to a maximum
 * of 1.0, and stores it in the loader's per-URL progress map under a lock.
 *
 * @param url      The resource URL whose progress is being reported.
 * @param dlnow    Number of bytes downloaded so far for this URL.
 * @param dltotal  Total number of bytes expected for this URL; if <= 0 the function will approximate
 *                 the total using `MAX_BYTES`.
 */
void ImageLoader::setProgress(const std::string& url, int64_t dlnow, int64_t dltotal) {
    float pct;
    if (dltotal > 0)
        pct = (float)dlnow / (float)dltotal;
    else
        pct = (float)dlnow / (float)MAX_BYTES;  // Content-Length なし: 上限比で近似
    if (pct > 1.0f) pct = 1.0f;
    LightLock_Lock(&progressLock_);
    progressMap_[url] = { pct };
    LightLock_Unlock(&progressLock_);
}

/**
 * @brief Retrieves the current download progress for a given URL.
 *
 * Looks up the stored progress percentage for the specified URL and returns it.
 *
 * @param url URL whose download progress is queried.
 * @return float Progress fraction between 0.0 and 1.0, or 0.0 if no progress is recorded.
 */
float ImageLoader::getProgress(const std::string& url) {
    LightLock_Lock(&progressLock_);
    auto it = progressMap_.find(url);
    float result = 0.0f;
    if (it != progressMap_.end())
        result = it->second.pct;
    LightLock_Unlock(&progressLock_);
    return result;
}

/**
 * @brief Clears all stored per-URL download progress entries.
 *
 * After calling this, subsequent calls to getProgress(url) will return 0.0
 * for any URL that previously had progress recorded.
 *
 * This operation is safe to call concurrently with other ImageLoader
 * operations.
 */
void ImageLoader::clearProgress() {
    LightLock_Lock(&progressLock_);
    progressMap_.clear();
    LightLock_Unlock(&progressLock_);
}
