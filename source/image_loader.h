#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
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
/**
 * Initialize the ImageLoader and prepare internal state for starting the worker.
 */
 
/**
 * Stop the worker if running and release any held resources.
 */
 
/**
 * Start the worker thread if it is not already running.
 */
 
/**
 * Request the worker to stop, join the worker thread, and discard queued results and jobs.
 * After completion, the loader may be started again.
 */
 
/**
 * Enqueue a URL as a decoding job.
 * @param url Source identifier to be processed; callers are responsible for avoiding duplicate submissions.
 */
 
/**
 * Remove all jobs that have not yet started. If a job is currently in flight, wait for it to finish.
 */
 
/**
 * Retrieve one completed decoded result from the results queue.
 * @param out Destination for the next available DecodedImage.
 * @returns `true` if a result was written to `out`, `false` if no completed result was available.
 */
 
/**
 * Get the completion progress for a specific URL.
 * @param url URL whose progress to query.
 * @returns A value in the range 0.0 to 1.0 representing completion percentage; returns 0.0 if the URL is not registered.
 */
 
/**
 * Update progress for a URL using downloaded-now and downloaded-total counters.
 * Intended to be called only from the worker thread.
 * @param url URL whose progress should be updated.
 * @param dlnow Number of bytes downloaded so far for this URL.
 * @param dltotal Total number of bytes expected for this URL (may be zero or unknown).
 */
 
/**
 * Clear all entries from the progress map.
 */
 
/**
 * Main loop executed by the worker thread; consumes jobs, produces DecodedImage results, and updates progress.
 */
 
/**
 * Thread entry trampoline that dispatches to the instance's workerMain().
 * @param self Pointer to the ImageLoader instance.
 */
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

    // render thread: 0.0-1.0 を返す。未登録時は 0.0。
    float getProgress(const std::string& url);

    // ダウンロード後のリサイズ上限 (デフォルト 256)。start() 前に呼ぶこと。
    void setMaxDim(int d) { maxDim_ = d; }

    // worker thread からのみ呼ぶ。
    void setProgress(const std::string& url, int64_t dlnow, int64_t dltotal);

    // プログレスマップをクリア (cancelAll / resetForArticle 時)。
    void clearProgress();

private:
    void          workerMain();
    static void   trampoline(void* self);

    struct ProgressData { float pct = 0.0f; };

    int                                                maxDim_  = 256;
    LightLock                                          lock_;
    LightLock                                          progressLock_;
    LightEvent                                         wakeup_;
    std::deque<std::string>                            jobs_;
    std::deque<DecodedImage>                           results_;
    std::unordered_map<std::string, ProgressData>      progressMap_;
    Thread                                             thread_  = nullptr;
    volatile bool                                      running_ = false;
    volatile bool                                      stop_    = false;
};
