#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include "net.h"
#include "feed_config.h"
#include "rss.h"
#include "ui.h"
#include "settings.h"

// ---- 時間ベースキーリピート ----
// hidKeysDownRepeat() はフレーム単位カウントのため、描画負荷によってスクロール速度が変わる。
// osGetTime()（ミリ秒）を使って独自計算することでフレームレート非依存にする。

static const u32 REPEAT_KEYS[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
static const int NUM_REPEAT_KEYS = 4;

struct KeyRepeatState {
    u64  holdStart[NUM_REPEAT_KEYS];   // 各キーを押し始めた時刻 (ms)
    u64  lastFired[NUM_REPEAT_KEYS];   // 直近リピート発火時刻 (ms)
    bool inRepeat[NUM_REPEAT_KEYS];    // 初動遅延を過ぎてリピート中か

    KeyRepeatState() {
        for (int i = 0; i < NUM_REPEAT_KEYS; ++i) {
            holdStart[i] = lastFired[i] = 0;
            inRepeat[i]  = false;
        }
    }
};

// kDown/kHeld から時間ベースのリピートビットマスクを生成する。
// 戻り値: kDown（初回押下）＋リピート発火したキーのビットOR
static u32 computeRepeat(u32 kDown, u32 kHeld,
                          KeyRepeatState& rs,
                          u32 delayMs, u32 intervalMs) {
    u64 now    = osGetTime();
    u32 result = 0;

    for (int i = 0; i < NUM_REPEAT_KEYS; ++i) {
        u32 key = REPEAT_KEYS[i];

        if (kDown & key) {
            // 初回押下: 即時発火＋タイマーリセット
            result          |= key;
            rs.holdStart[i]  = now;
            rs.lastFired[i]  = now;
            rs.inRepeat[i]   = false;
        } else if (kHeld & key) {
            u64 held = now - rs.holdStart[i];
            if (!rs.inRepeat[i]) {
                if (held >= delayMs) {
                    // 初動遅延を超えた → リピート開始
                    rs.inRepeat[i]  = true;
                    rs.lastFired[i] = now;
                    result         |= key;
                }
            } else if (now - rs.lastFired[i] >= intervalMs) {
                // リピート発火
                rs.lastFired[i] = now;
                result         |= key;
            }
        } else {
            // キー離し: 状態リセット
            rs.inRepeat[i] = false;
        }
    }
    return result;
}

int main() {
    // グラフィックス初期化
    gfxInitDefault();
    romfsInit();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    ptmuInit();

    uiInit();

    AppState state;

    // ネットワーク初期化
    bool netOk = netInit();
    if (!netOk) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "SOC init failed: 0x%08lX",
                 (unsigned long)netLastError());
        state.statusMsg = errBuf;
    }

    // フィード設定をローカルから読み込む（ネットワーク不要・即座）
    std::vector<FeedConfig> configs =
        loadFeedConfig("sdmc:/3ds/rssreader/feeds.json");

    if (configs.empty()) {
        configs.push_back({"https://news.ycombinator.com/rss", "Hacker News", false});
        configs.push_back({"https://arstechnica.com/feed/",    "Ars Technica",  false});
    }

    state.feedConfigs = std::move(configs);
    state.feeds.resize(state.feedConfigs.size());
    state.feedLoaded.resize(state.feedConfigs.size(), false);

    settingsLoad(state.settings);
    state.readHistory.load();
    state.bookmarkStore.load();
    state.imgViewLoader.setMaxDim(1024);
    state.articleLoader.start();
    state.feedLoader.start();

    KeyRepeatState repeatState;

    // メインループ
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        // 画面に応じてリピートパラメータを切り替え
        u32 kRepeat;
        if (state.currentScreen == Screen::ArticleView) {
            kRepeat = computeRepeat(kDown, kHeld, repeatState,
                                    (u32)state.settings.scrollRepeatDelayMs,
                                    (u32)state.settings.scrollRepeatIntervalMs);
        } else {
            kRepeat = computeRepeat(kDown, kHeld, repeatState, 200, 167);
        }

        // LoadingArticle 状態: 記事本文の非同期ダウンロード完了待ち
        if (state.currentScreen == Screen::LoadingArticle) {
            FetchedArticle result;
            std::string errMsg;
            if (state.articleLoader.poll(result, errMsg)) {
                if (!result.body.empty()) {
                    if (state.pendingFetchFeed == -2) {
                        // ブックマーク一覧から開いた一時記事
                        state.bookmarkTempArticle.content   = std::move(result.body);
                        state.bookmarkTempArticle.imageUrls = std::move(result.imageUrls);
                        if (state.pendingFetchFullArticle) state.bookmarkTempArticle.fullFetched = true;
                        state.viewingBookmark       = true;
                    } else {
                        auto& art = state.feeds[state.pendingFetchFeed]
                                          .articles[state.pendingFetchArticle];
                        art.content   = std::move(result.body);
                        art.imageUrls = std::move(result.imageUrls);
                        if (state.pendingFetchFullArticle) art.fullFetched = true;
                        state.selectedFeed    = state.pendingFetchFeed;
                        state.selectedArticle = state.pendingFetchArticle;
                    }
                    state.cachedLineContentSize = 0;  // 本文行キャッシュを強制無効化
                    state.cachedImagesArticle = -1;  // 画像キャッシュ再初期化
                    state.scrollY         = 0;
                    state.statusMsg       = "";
                    state.currentScreen   = Screen::ArticleView;
                } else {
                    if (state.pendingFetchFeed == -2) state.viewingBookmark = false;
                    state.statusMsg     = std::string("Fetch failed: ") + errMsg;
                    state.currentScreen = state.pendingReturnScreen;
                }
            }
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            uiDraw(state);
            C3D_FrameEnd(0);
            continue;
        }

        // LoadingAll 状態: 全フィードを順番にリフレッシュ
        if (state.currentScreen == Screen::LoadingAll) {
            int idx = state.refreshIdx;
            if (!netOk || idx >= (int)state.feedConfigs.size()) {
                state.statusMsg     = netOk ? "" : "Network unavailable.";
                state.currentScreen = Screen::FeedList;
            } else {
                const std::string& name = state.feedConfigs[idx].name;
                char buf[128];
                snprintf(buf, sizeof(buf), "Refreshing %d/%d: %s",
                         idx + 1, (int)state.feedConfigs.size(),
                         name.empty() ? state.feedConfigs[idx].url.c_str() : name.c_str());
                state.statusMsg = buf;
                C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
                uiDraw(state);
                C3D_FrameEnd(0);

                std::string errMsg;
                std::string xml = httpGet(state.feedConfigs[idx].url, errMsg);
                if (!xml.empty()) {
                    state.feeds[idx]      = parseFeed(xml, errMsg);
                    state.feedLoaded[idx] = true;
                }
                ++state.refreshIdx;
            }
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            uiDraw(state);
            C3D_FrameEnd(0);
            continue;
        }

        // Loading 状態: フィードの非同期フェッチ完了待ち
        if (state.currentScreen == Screen::Loading) {
            int idx = state.selectedFeed;
            if (idx < 0 || idx >= (int)state.feedConfigs.size()) {
                state.currentScreen = Screen::FeedList;
            } else if (state.feedLoaded[idx]) {
                // キャッシュ済み: ローダー未使用なので即遷移
                state.statusMsg     = "";
                state.currentScreen = Screen::ArticleList;
            } else if (!netOk) {
                state.statusMsg     = "Network unavailable.";
                state.currentScreen = Screen::FeedList;
            } else {
                if (!state.feedJobSubmitted) {
                    state.feedLoader.submit(state.feedConfigs[idx].url);
                    state.feedJobSubmitted = true;
                }
                FetchedFeed result;
                if (state.feedLoader.poll(result)) {
                    if (result.errMsg.empty()) {
                        state.feeds[idx]      = std::move(result.feed);
                        state.feedLoaded[idx] = true;
                        state.statusMsg       = "";
                        state.currentScreen   = Screen::ArticleList;
                    } else {
                        state.statusMsg     = std::string("Fetch failed: ") + result.errMsg;
                        state.currentScreen = Screen::FeedList;
                    }
                }
            }
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            uiDraw(state);
            C3D_FrameEnd(0);
            continue;
        }

        uiHandleInput(state, kDown, kHeld, kRepeat);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        uiDraw(state);
        C3D_FrameEnd(0);
    }

    // 終了処理（coding-patterns #2 の順序を遵守）
    // worker thread を最初に停止 → 画像テクスチャを C3D 生存中に解放
    state.feedLoader.stop();
    state.articleLoader.stop();
    state.imgViewLoader.stop();
    state.imgViewCache.resetForArticle({});
    state.imgLoader.stop();
    state.imgCache.resetForArticle({});
    uiExit();
    ptmuExit();
    if (netOk) netExit();
    C2D_Fini();
    C3D_Fini();
    romfsExit();
    gfxExit();
    return 0;
}
