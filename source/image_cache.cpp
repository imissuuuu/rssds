#include "image_cache.h"
#include <cstring>

static int nextPOT(int v) {
    int r = 8;
    while (r < v) r <<= 1;
    return r;
}

static inline uint32_t morton8(int x, int y) {
    return ((uint32_t)(x & 1) << 0) | ((uint32_t)(y & 1) << 1)
         | ((uint32_t)(x & 2) << 1) | ((uint32_t)(y & 2) << 2)
         | ((uint32_t)(x & 4) << 2) | ((uint32_t)(y & 4) << 3);
}

// PDF: A,B,G,R byte 順 + 8x8 タイル Morton swizzle。
// dst は texW*texH*4 byte 既確保。imgW/imgH はパディング前の有効領域。
static void swizzleABGR(const uint8_t* src, int imgW, int imgH,
                        uint8_t* dst, int texW, int texH) {
    for (int y = 0; y < texH; ++y) {
        int ty = y >> 3;
        int py = y & 7;
        for (int x = 0; x < texW; ++x) {
            int tx = x >> 3;
            int px = x & 7;
            int dstIdx = (ty * (texW >> 3) + tx) * 64 + (int)morton8(px, py);
            uint8_t* d = dst + dstIdx * 4;
            if (x < imgW && y < imgH) {
                const uint8_t* s = src + (y * imgW + x) * 4;
                d[0] = s[3]; // A
                d[1] = s[2]; // B
                d[2] = s[1]; // G
                d[3] = s[0]; // R
            } else {
                d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0;
            }
        }
    }
}

void ImageCache::releaseAll() {
    for (auto& kv : map_) {
        if (kv.second.texInited) {
            C3D_TexDelete(&kv.second.tex);
            kv.second.texInited = false;
        }
    }
    map_.clear();
}

void ImageCache::resetForArticle(const std::vector<std::string>& urls) {
    if (loader_) {
        loader_->cancelAll();
        // in-flight 1件分を含む既存結果を破棄
        DecodedImage d;
        while (loader_->poll(d)) { /* discard */ }
    }
    releaseAll();
    urls_ = urls;
    for (const auto& u : urls_) {
        map_[u] = CachedImage{};
    }
}

void ImageCache::tick(const std::unordered_set<std::string>& visible) {
    // 1. 不要なものを evict (state=None に戻して再取得を許可)
    for (auto& kv : map_) {
        if (visible.count(kv.first)) continue;
        if (kv.second.texInited) {
            C3D_TexDelete(&kv.second.tex);
            kv.second.texInited = false;
        }
        // Failed は再試行させない (永続失敗扱い)
        if (kv.second.state == ImgState::Ready) {
            kv.second.state = ImgState::None;
        }
    }

    // 2. 必要なものを submit
    if (loader_) {
        for (const auto& u : visible) {
            auto it = map_.find(u);
            if (it == map_.end()) continue;  // 別記事の URL は無視
            if (it->second.state == ImgState::None) {
                loader_->submit(u);
                it->second.state = ImgState::Pending;
            }
        }
    }

    // 3. 完了を 1 件 upload
    if (loader_) {
        DecodedImage d;
        if (loader_->poll(d)) {
            uploadOne(std::move(d), visible);
        }
    }
}

void ImageCache::uploadOne(DecodedImage&& d,
                            const std::unordered_set<std::string>& visible) {
    auto it = map_.find(d.url);
    if (it == map_.end()) return;  // 別記事の残骸

    CachedImage& c = it->second;

    if (d.failed) {
        c.state = ImgState::Failed;
        return;
    }

    // 既に evict されたものは破棄して再取得対象に戻す
    if (!visible.count(d.url)) {
        c.state = ImgState::None;
        return;
    }

    int texW = nextPOT(d.imgW);
    int texH = nextPOT(d.imgH);
    if (texW > 1024 || texH > 1024 || d.imgW <= 0 || d.imgH <= 0) {
        c.state = ImgState::Failed;
        return;
    }

    if (!C3D_TexInit(&c.tex, (u16)texW, (u16)texH, GPU_RGBA8)) {
        c.state = ImgState::Failed;
        return;
    }
    c.texInited = true;

    swizzleABGR(d.rgba.data(), d.imgW, d.imgH,
                static_cast<uint8_t*>(c.tex.data), texW, texH);
    C3D_TexSetFilter(&c.tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexFlush(&c.tex);

    c.imgW = d.imgW;
    c.imgH = d.imgH;
    c.sub.width  = (u16)d.imgW;
    c.sub.height = (u16)d.imgH;
    c.sub.left   = 0.0f;
    c.sub.right  = (float)d.imgW / (float)texW;
    c.sub.top    = 1.0f;
    c.sub.bottom = 1.0f - (float)d.imgH / (float)texH;
    c.state = ImgState::Ready;
}

const CachedImage* ImageCache::get(const std::string& url) const {
    auto it = map_.find(url);
    return (it == map_.end()) ? nullptr : &it->second;
}
