#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>

// worker thread が生成し render thread が消費するデコード済み画像。
// rgba は top-left 起点・4 byte (R,G,B,A) per pixel。
struct DecodedImage {
    std::string         url;
    std::vector<uint8_t> rgba;
    int                 imgW   = 0;
    int                 imgH   = 0;
    bool                failed = false;
};

// download/decode/resize を担う worker。libctru の thread API と LightLock のみ使用。
// 3DS allocator (linearAlloc/vramAlloc) や citro3d API は呼ばない。
class ImageLoader {
public:
    ImageLoader();
    ~ImageLoader();

    ImageLoader(const ImageLoader&)            = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // worker thread を起動。すでに起動済みなら no-op。
    void start();

    // 停止要求 → worker join → キュー破棄。再 start 可能。
    void stop();

    // URL を job キューへ投入。重複判定は呼び出し側責任。
    void submit(const std::string& url);

    // 未着手 job をすべて破棄。in-flight 1 件は完走を待つ。
    void cancelAll();

    // 完了結果を 1 件取り出す。なければ false。
    bool poll(DecodedImage& out);

private:
    void          workerMain();
    static void   trampoline(void* self);

    LightLock                lock_;
    LightEvent               wakeup_;
    std::deque<std::string>  jobs_;
    std::deque<DecodedImage> results_;
    Thread                   thread_  = nullptr;
    volatile bool            running_ = false;
    volatile bool            stop_    = false;
};
