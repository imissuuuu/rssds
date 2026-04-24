#include "article_loader.h"

struct ArtXferCtx {
    ArticleLoader* loader; // cppcheck-suppress unusedStructMember
};

static int artXferCb(void* ud, int64_t dltotal, int64_t dlnow) {
    static_cast<ArtXferCtx*>(ud)->loader->setProgress(dlnow, dltotal);
    return 0;
}

ArticleLoader::ArticleLoader() {
    LightLock_Init(&lock_);
    LightLock_Init(&progressLock_);
    LightEvent_Init(&wakeup_, RESET_STICKY);
}

ArticleLoader::~ArticleLoader() {
    stop();
}

void ArticleLoader::start() {
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

void ArticleLoader::stop() {
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

void ArticleLoader::submit(const std::string& url) {
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

bool ArticleLoader::poll(FetchedArticle& out, std::string& errMsg) {
    LightLock_Lock(&lock_);
    if (!resultReady_) {
        LightLock_Unlock(&lock_);
        return false;
    }
    out = std::move(resultArticle_);
    errMsg = std::move(resultErr_);
    resultReady_ = false;
    LightLock_Unlock(&lock_);
    return true;
}

float ArticleLoader::getProgress() {
    LightLock_Lock(&progressLock_);
    float p = progress_;
    LightLock_Unlock(&progressLock_);
    return p;
}

void ArticleLoader::setProgress(int64_t dlnow, int64_t dltotal) {
    float pct;
    if (dltotal > 0)
        pct = (float)dlnow / (float)dltotal;
    else
        pct = (float)dlnow / (float)MAX_HTML_BYTES;
    if (pct > 1.0f)
        pct = 1.0f;
    LightLock_Lock(&progressLock_);
    progress_ = pct;
    LightLock_Unlock(&progressLock_);
}

void ArticleLoader::trampoline(void* self) {
    static_cast<ArticleLoader*>(self)->workerMain();
}

void ArticleLoader::workerMain() {
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

        ArtXferCtx xctx{this};
        std::string errMsg;
        FetchedArticle art = fetchArticleBody2(url, errMsg, artXferCb, &xctx);

        LightLock_Lock(&lock_);
        resultArticle_ = std::move(art);
        resultErr_ = std::move(errMsg);
        resultReady_ = true;
        LightLock_Unlock(&lock_);
    }
}
