#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include "net.h"
#include "feed_config.h"
#include "rss.h"
#include "ui.h"

// テスト用ハードコードURL（feeds.txt が空の場合に使用）
static const char* TEST_URLS[] = {
    "https://news.ycombinator.com/rss",
    "https://arstechnica.com/feed/",
    nullptr
};

int main() {
    // グラフィックス初期化
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    uiInit();

    AppState state;
    state.statusMsg = "Initializing network...";

    // ネットワーク初期化
    bool netOk = netInit();
    if (!netOk) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "SOC init failed: 0x%08lX", (unsigned long)netLastError());
        state.statusMsg = errBuf;
    } else {
        state.statusMsg = "Loading feeds...";

        // フィードURL読み込み
        std::vector<std::string> urls =
            loadFeedUrls("sdmc:/3ds/rssreader/feeds.txt");

        if (urls.empty()) {
            for (int i = 0; TEST_URLS[i]; ++i)
                urls.emplace_back(TEST_URLS[i]);
        }

        // フィード取得・パース
        for (const auto& url : urls) {
            std::string errMsg;
            std::string xml = httpGet(url, errMsg);
            if (xml.empty()) {
                // 取得失敗は無視して次へ（statusMsgに最後のエラーを残す）
                state.statusMsg = std::string("Fetch failed: ") + errMsg;
                continue;
            }
            Feed feed = parseFeed(xml, errMsg);
            if (!errMsg.empty()) {
                state.statusMsg = std::string("Parse failed: ") + errMsg;
                continue;
            }
            state.feeds.push_back(std::move(feed));
        }

        if (state.feeds.empty()) {
            state.statusMsg = "No feeds loaded. Check network/URLs.";
        } else {
            state.statusMsg = "";
        }
    }

    // メインループ
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

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
