#pragma once
#include "article.h"
#include <3ds.h>
#include <cstdint>
#include <string>

class ArticleLoader {
  public:
    ArticleLoader();
    ~ArticleLoader();

    ArticleLoader(const ArticleLoader&) = delete;
    ArticleLoader& operator=(const ArticleLoader&) = delete;

    void start();
    void stop();

    // フェッチするURLを投入。前回の結果・進捗はリセットされる。
    void submit(const std::string& url);

    // 完了していれば結果を移動して true を返す。
    bool poll(FetchedArticle& out, std::string& errMsg);

    // 0.0–1.0 のダウンロード進捗を返す。
    float getProgress();

    // worker からのみ呼ぶ。
    void setProgress(int64_t dlnow, int64_t dltotal);

  private:
    static void trampoline(void* self);
    void workerMain();

    LightLock lock_;
    LightLock progressLock_;
    LightEvent wakeup_;

    std::string pendingUrl_;
    bool hasJob_ = false;

    FetchedArticle resultArticle_;
    std::string resultErr_;
    bool resultReady_ = false;

    float progress_ = 0.0f;

    volatile bool stop_ = false;
    volatile bool running_ = false;
    Thread thread_ = nullptr;

    static constexpr size_t WORKER_STACK = 64u * 1024u;
    // Content-Length なし時の進捗推定用: 記事HTML の典型的上限
    static constexpr size_t MAX_HTML_BYTES = 200u * 1024u;
};
