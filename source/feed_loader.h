#pragma once
#include "rss.h"
#include <3ds.h>
#include <string>
#include <cstdint>

struct FetchedFeed {
    Feed        feed;
    std::string errMsg;
};

class FeedLoader {
public:
    FeedLoader();
    ~FeedLoader();

    FeedLoader(const FeedLoader&)            = delete;
    FeedLoader& operator=(const FeedLoader&) = delete;

    void start();
    void stop();

    void submit(const std::string& url);
    bool poll(FetchedFeed& out);

    float getProgress();
    void  setProgress(int64_t dlnow, int64_t dltotal);

private:
    static void trampoline(void* self);
    void workerMain();

    LightLock  lock_;
    LightLock  progressLock_;
    LightEvent wakeup_;

    std::string pendingUrl_;
    bool        hasJob_      = false;

    FetchedFeed result_;
    bool        resultReady_ = false;

    float       progress_    = 0.0f;

    volatile bool stop_    = false;
    volatile bool running_ = false;
    Thread        thread_  = nullptr;

    static constexpr size_t WORKER_STACK   = 64u * 1024u;
    static constexpr size_t MAX_FEED_BYTES = 512u * 1024u;
};
