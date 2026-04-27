#pragma once
#include "feed_config.h"
#include "rss.h"
#include <3ds.h>
#include <string>
#include <vector>

struct RefreshAllResult {
    int idx = -1;
    Feed feed;
    std::string errMsg;
};

class RefreshAllLoader {
  public:
    static constexpr int MAX_PARALLEL = 2;
    static constexpr size_t WORKER_STACK = 64u * 1024u;

    RefreshAllLoader();
    ~RefreshAllLoader();
    RefreshAllLoader(const RefreshAllLoader&) = delete;
    RefreshAllLoader& operator=(const RefreshAllLoader&) = delete;

    void start(const std::vector<FeedConfig>& configs);
    void cancel();
    void stop();

    // 毎フレーム呼ぶ。完了スロットを join して次ジョブを投入。
    // 全ジョブ完了 or キャンセル後に全スレッド終了で true を返す
    bool tick();

    bool poll(RefreshAllResult& out);

    int totalCount() const;
    int completedCount() const;

  private:
    struct Slot {
        Thread thread = nullptr;
        int feedIdx = -1;
        RefreshAllResult result;
        bool done = false;
    };

    struct WorkerArg {
        RefreshAllLoader* self;
        int slotIdx;
    };

    static void trampoline(void* arg);
    void workerMain(int slotIdx);
    void dispatchToSlot(int slotIdx, int feedIdx); // lock_ 保持下で呼ぶ

    mutable LightLock lock_;
    std::vector<FeedConfig> configs_;
    std::vector<RefreshAllResult> readyResults_;

    Slot slots_[MAX_PARALLEL];
    WorkerArg workerArgs_[MAX_PARALLEL];

    int nextFeedIdx_ = 0;
    int completedCount_ = 0;
    int total_ = 0;
    bool cancelled_ = false;
};
