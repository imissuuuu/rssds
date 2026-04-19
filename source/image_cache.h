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

// render thread からのみ操作。ImageLoader (worker) と疎結合。
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

    // Pending 中の URL のダウンロード進捗 (0.0-1.0)。
    float getProgress(const std::string& url) {
        return loader_ ? loader_->getProgress(url) : -1.0f;
    }

private:
    void uploadOne(DecodedImage&& d, const std::unordered_set<std::string>& visible);
    void releaseAll();

    ImageLoader*                                  loader_ = nullptr;
    std::vector<std::string>                      urls_;
    std::unordered_map<std::string, CachedImage>  map_;
};
