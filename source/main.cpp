#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include "net.h"
#include "feed_config.h"
#include "rss.h"
#include "ui.h"

int main() {
    // グラフィックス初期化
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

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

    // メインループ
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        // Loading 状態: 前フレームで Loading 画面が表示済み → フェッチ実行
        if (state.currentScreen == Screen::Loading) {
            int idx = state.selectedFeed;
            if (idx >= 0 && idx < (int)state.feedConfigs.size()) {
                if (state.feedLoaded[idx]) {
                    // キャッシュ済み
                    state.currentScreen = Screen::ArticleList;
                } else if (!netOk) {
                    state.statusMsg = "Network unavailable.";
                    state.currentScreen = Screen::FeedList;
                } else {
                    std::string errMsg;
                    std::string xml = httpGet(state.feedConfigs[idx].url, errMsg);
                    if (xml.empty()) {
                        state.statusMsg = std::string("Fetch failed: ") + errMsg;
                        state.currentScreen = Screen::FeedList;
                    } else {
                        state.feeds[idx] = parseFeed(xml, errMsg);
                        state.feedLoaded[idx] = true;
                        state.statusMsg = "";
                        state.currentScreen = Screen::ArticleList;
                    }
                }
            } else {
                state.currentScreen = Screen::FeedList;
            }
            // Loading 中は入力処理をスキップして描画のみ行う
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            uiDraw(state);
            C3D_FrameEnd(0);
            continue;
        }

        uiHandleInput(state, kDown, kHeld);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        uiDraw(state);
        C3D_FrameEnd(0);
    }

    // 終了処理（coding-patterns #2 の順序を遵守）
    uiExit();
    if (netOk) netExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
