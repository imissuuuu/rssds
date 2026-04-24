#include "feed_loader.h"
#include "net.h"
#include "rss.h"

struct FeedXferCtx {
    FeedLoader* loader;
};

static int feedXferCb(void* ud, int64_t dltotal, int64_t dlnow) {
    static_cast<FeedXferCtx*>(ud)->loader->setProgress(dlnow, dltotal);
    return 0;
}

FeedLoader::FeedLoader() {
    LightLock_Init(&lock_);
    LightLock_Init(&progressLock_);
    LightEvent_Init(&wakeup_, RESET_STICKY);
}

FeedLoader::~FeedLoader() {
    stop();
}

void FeedLoader::start() {
    if (running_)
        return;
    stop_ = false;
    running_ = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thread_ = threadCreate(trampoline, this, WORKER_STACK, prio + 1, -2, false);
    if (!thread_)
        running_ = false;
}

void FeedLoader::stop() {
    if (!running_)
        return;
    stop_ = true;
    LightEvent_Signal(&wakeup_);
    if (thread_) {
        threadJoin(thread_, U64_MAX);
        threadFree(thread_);
        thread_ = nullptr;
    }
    running_ = false;
    LightLock_Lock(&lock_);
    hasJob_ = false;
    resultReady_ = false;
    LightLock_Unlock(&lock_);
}

void FeedLoader::submit(const std::string& url) {
    LightLock_Lock(&lock_);
    pendingUrl_ = url;
    hasJob_ = true;
    resultReady_ = false;
    LightLock_Unlock(&lock_);

    LightLock_Lock(&progressLock_);
    progress_ = 0.0f;
    LightLock_Unlock(&progressLock_);

    LightEvent_Signal(&wakeup_);
}

bool FeedLoader::poll(FetchedFeed& out) {
    LightLock_Lock(&lock_);
    if (!resultReady_) {
        LightLock_Unlock(&lock_);
        return false;
    }
    out = std::move(result_);
    resultReady_ = false;
    LightLock_Unlock(&lock_);
    return true;
}

float FeedLoader::getProgress() {
    LightLock_Lock(&progressLock_);
    float p = progress_;
    LightLock_Unlock(&progressLock_);
    return p;
}

void FeedLoader::setProgress(int64_t dlnow, int64_t dltotal) {
    float pct;
    if (dltotal > 0)
        pct = (float)dlnow / (float)dltotal;
    else
        pct = (float)dlnow / (float)MAX_FEED_BYTES;
    if (pct > 1.0f)
        pct = 1.0f;
    LightLock_Lock(&progressLock_);
    progress_ = pct;
    LightLock_Unlock(&progressLock_);
}

void FeedLoader::trampoline(void* self) {
    static_cast<FeedLoader*>(self)->workerMain();
}

void FeedLoader::workerMain() {
    while (!stop_) {
        std::string url;
        LightLock_Lock(&lock_);
        if (!hasJob_) {
            LightLock_Unlock(&lock_);
            LightEvent_Wait(&wakeup_);
            LightEvent_Clear(&wakeup_);
            continue;
        }
        url = pendingUrl_;
        hasJob_ = false;
        LightLock_Unlock(&lock_);

        if (stop_)
            return;

        FeedXferCtx xctx{this};
        std::string errMsg;
        std::string xml = httpGet(url, errMsg, feedXferCb, &xctx);

        FetchedFeed res;
        if (xml.empty()) {
            res.errMsg = std::move(errMsg);
        } else {
            res.feed = parseFeed(xml, res.errMsg);
        }

        LightLock_Lock(&lock_);
        result_ = std::move(res);
        resultReady_ = true;
        LightLock_Unlock(&lock_);
    }
}
