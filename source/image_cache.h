#pragma once
#include <citro2d.h>
#include <3ds.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include "image_loader.h"

enum class ImgState : uint8_t { None, Pending, Ready, Failed };

struct CachedImage {
    ImgState           state     = ImgState::None;
    C3D_Tex            tex       {};
    Tex3DS_SubTexture  sub       {};
    bool               texInited = false;
    int                imgW      = 0;
    int                imgH      = 0;
};

/**
 * Attach an ImageLoader worker to the cache so the render thread can submit and poll jobs.
 * @param loader Pointer to the ImageLoader instance to attach; may be nullptr to detach.
 */
/**
 * Prepare the cache for a new article: cancel in-flight worker jobs, absorb any results,
 * release all GPU textures, and replace the tracked URL list.
 * @param urls Vector of URLs that belong to the new article.
 */
/**
 * Advance the cache once per frame: submit newly visible URLs for loading, delete GPU
 * textures for URLs that became invisible, and poll at most one completed decode from
 * the attached worker for upload.
 * @param visible Set of URLs currently visible this frame.
 */
/**
 * Retrieve cached image metadata for a URL.
 * @param url URL of the image to look up.
 * @returns Pointer to the CachedImage for `url`, or `nullptr` if there is no cache entry.
 */
/**
 * Query download progress for a pending URL.
 * @param url URL whose download progress to query.
 * @returns Download progress in the range 0.0–1.0 for pending downloads; `-1.0f` if no
 * loader is attached or the URL has no progress information.
 */
class ImageCache {
public:
    ImageCache() = default;
    ~ImageCache() { releaseAll(); }

    ImageCache(const ImageCache&)            = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    void attach(ImageLoader* loader) { loader_ = loader; }

    // 記事遷移時。loader へ cancelAll を伝え、in-flight 結果を吸収して全 tex 解放。
    void resetForArticle(const std::vector<std::string>& urls);

    // 毎フレーム呼ぶ:
    //   1. visible にあって state==None のものを submit + Pending 化
    //   2. visible にないものは TexDelete + state=None (記憶は残す)
    //   3. loader->poll() を最大 1 件処理し TexInit + swizzle + Flush
    void tick(const std::unordered_set<std::string>& visible);

    const CachedImage* get(const std::string& url) const;

    /**
     * Get the download progress for a pending URL.
     *
     * @param url URL of the image to query.
     * @returns `0.0`–`1.0` progress for the URL, or `-1.0` if no ImageLoader is attached or progress is unavailable.
     */
    float getProgress(const std::string& url) {
        return loader_ ? loader_->getProgress(url) : 0.0f;
    }

private:
    void uploadOne(DecodedImage&& d, const std::unordered_set<std::string>& visible);
    void releaseAll();

    ImageLoader*                                  loader_ = nullptr;
    std::vector<std::string>                      urls_;
    std::unordered_map<std::string, CachedImage>  map_;
};
