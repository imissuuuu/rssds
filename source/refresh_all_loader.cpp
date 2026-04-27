#include "refresh_all_loader.h"
#include "net.h"
#include "rss.h"

RefreshAllLoader::RefreshAllLoader() {
    LightLock_Init(&lock_);
    for (int i = 0; i < MAX_PARALLEL; i++) {
        slots_[i] = Slot{};
        workerArgs_[i] = WorkerArg{};
    }
}

RefreshAllLoader::~RefreshAllLoader() {
    stop();
}

void RefreshAllLoader::start(const std::vector<FeedConfig>& configs) {
    stop();

    LightLock_Lock(&lock_);
    configs_ = configs;
    total_ = (int)configs_.size();
    nextFeedIdx_ = 0;
    completedCount_ = 0;
    cancelled_ = false;
    readyResults_.clear();

    for (int i = 0; i < MAX_PARALLEL && nextFeedIdx_ < total_; i++)
        dispatchToSlot(i, nextFeedIdx_++);

    LightLock_Unlock(&lock_);
}

void RefreshAllLoader::cancel() {
    LightLock_Lock(&lock_);
    cancelled_ = true;
    LightLock_Unlock(&lock_);
}

void RefreshAllLoader::stop() {
    LightLock_Lock(&lock_);
    cancelled_ = true;
    LightLock_Unlock(&lock_);

    for (int i = 0; i < MAX_PARALLEL; i++) {
        if (slots_[i].thread) {
            threadJoin(slots_[i].thread, U64_MAX);
            threadFree(slots_[i].thread);
            slots_[i].thread = nullptr;
            slots_[i].done = false;
        }
    }

    LightLock_Lock(&lock_);
    nextFeedIdx_ = 0;
    completedCount_ = 0;
    total_ = 0;
    cancelled_ = false;
    readyResults_.clear();
    configs_.clear();
    LightLock_Unlock(&lock_);
}

bool RefreshAllLoader::tick() {
    for (int i = 0; i < MAX_PARALLEL; i++) {
        Thread toJoin = nullptr;
        int nextIdx = -1;
        RefreshAllResult res;
        bool storeResult = false;

        LightLock_Lock(&lock_);
        if (slots_[i].thread != nullptr && slots_[i].done) {
            toJoin = slots_[i].thread;
            slots_[i].thread = nullptr;
            slots_[i].done = false;
            res = std::move(slots_[i].result);
            storeResult = !cancelled_;
            completedCount_++;
            if (!cancelled_ && nextFeedIdx_ < total_)
                nextIdx = nextFeedIdx_++;
        }
        LightLock_Unlock(&lock_);

        if (!toJoin)
            continue;

        threadJoin(toJoin, U64_MAX);
        threadFree(toJoin);

        LightLock_Lock(&lock_);
        if (storeResult)
            readyResults_.push_back(std::move(res));
        if (nextIdx >= 0 && !cancelled_)
            dispatchToSlot(i, nextIdx);
        LightLock_Unlock(&lock_);
    }

    LightLock_Lock(&lock_);
    bool allIdle = true;
    for (int i = 0; i < MAX_PARALLEL; i++) {
        if (slots_[i].thread != nullptr) {
            allIdle = false;
            break;
        }
    }
    bool done = allIdle && (completedCount_ >= total_ || cancelled_);
    LightLock_Unlock(&lock_);
    return done;
}

bool RefreshAllLoader::poll(RefreshAllResult& out) {
    LightLock_Lock(&lock_);
    if (readyResults_.empty()) {
        LightLock_Unlock(&lock_);
        return false;
    }
    out = std::move(readyResults_.front());
    readyResults_.erase(readyResults_.begin());
    LightLock_Unlock(&lock_);
    return true;
}

int RefreshAllLoader::totalCount() const {
    LightLock_Lock(&lock_);
    int t = total_;
    LightLock_Unlock(&lock_);
    return t;
}

int RefreshAllLoader::completedCount() const {
    LightLock_Lock(&lock_);
    int c = completedCount_;
    LightLock_Unlock(&lock_);
    return c;
}

void RefreshAllLoader::trampoline(void* arg) {
    auto* wa = static_cast<WorkerArg*>(arg);
    wa->self->workerMain(wa->slotIdx);
}

void RefreshAllLoader::workerMain(int slotIdx) {
    // feedIdx と url は dispatchToSlot で設定済み。lock 不要（happens-before）
    int feedIdx = slots_[slotIdx].feedIdx;
    std::string url = configs_[feedIdx].url;

    std::string errMsg;
    std::string xml = httpGet(url, errMsg);

    RefreshAllResult res;
    res.idx = feedIdx;
    if (!xml.empty()) {
        res.feed = parseFeed(xml, res.errMsg);
    } else {
        res.errMsg = std::move(errMsg);
    }

    LightLock_Lock(&lock_);
    slots_[slotIdx].result = std::move(res);
    slots_[slotIdx].done = true; // 最後にセット（メモリ順序保証）
    LightLock_Unlock(&lock_);
}

void RefreshAllLoader::dispatchToSlot(int slotIdx, int feedIdx) {
    // 呼び出し元は lock_ を保持していること
    workerArgs_[slotIdx] = {this, slotIdx};
    slots_[slotIdx].feedIdx = feedIdx;
    slots_[slotIdx].done = false;
    slots_[slotIdx].result = RefreshAllResult{};

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    slots_[slotIdx].thread =
        threadCreate(trampoline, &workerArgs_[slotIdx], WORKER_STACK, prio + 1, -2, false);

    if (!slots_[slotIdx].thread) {
        // threadCreate 失敗: スロットはアイドルのまま完了カウントを進める
        completedCount_++;
        if (!cancelled_) {
            RefreshAllResult failRes;
            failRes.idx = feedIdx;
            failRes.errMsg = "threadCreate failed";
            readyResults_.push_back(std::move(failRes));
        }
        // 未処理フィードがあれば同スロットへ再ディスパッチ
        if (!cancelled_ && nextFeedIdx_ < total_)
            dispatchToSlot(slotIdx, nextFeedIdx_++);
    }
}
