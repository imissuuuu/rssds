#include "ui.h"
#include "html_strip.h"
#include "article.h"
#include <citro2d.h>
#include <cstdio>
#include <cstring>

static constexpr int CONTENT_SHORT_THRESHOLD = 200; // これ未満なら本文取得を試みる

// 画面サイズ
static constexpr int TOP_W    = 400;
static constexpr int TOP_H    = 240;
static constexpr int BOT_W    = 320;
static constexpr int BOT_H    = 240;

// テキスト設定
static constexpr float TEXT_SCALE     = 0.5f;
static constexpr float LINE_HEIGHT    = 14.0f;
static constexpr float TEXT_MARGIN_X  = 6.0f;
static constexpr float TEXT_MARGIN_Y  = 6.0f;
static constexpr int   TOP_MAX_LINES  = (int)((TOP_H - TEXT_MARGIN_Y * 2) / LINE_HEIGHT);

// リスト表示行数（下画面）
static constexpr int   BOT_MAX_LINES  = (int)((BOT_H - 30.0f) / LINE_HEIGHT); // 30px は下部ガイド

// カラー
static constexpr u32 CLR_BG      = C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF);
static constexpr u32 CLR_PANEL   = C2D_Color32(0x16, 0x21, 0x3e, 0xFF);
static constexpr u32 CLR_TEXT    = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
static constexpr u32 CLR_SEL_BG  = C2D_Color32(0x0f, 0x3d, 0x60, 0xFF);
static constexpr u32 CLR_HINT    = C2D_Color32(0xA0, 0xA0, 0xA0, 0xFF);
static constexpr u32 CLR_TITLE   = C2D_Color32(0x5c, 0xd4, 0xff, 0xFF);

static C3D_RenderTarget* topTarget = nullptr;
static C3D_RenderTarget* botTarget = nullptr;
static C2D_TextBuf       textBuf   = nullptr;

void uiInit() {
    topTarget = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    botTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf   = C2D_TextBufNew(4096);
}

void uiExit() {
    C2D_TextBufDelete(textBuf);
    textBuf = nullptr;
}

// --- ヘルパー: テキスト描画（毎回 TextParse — coding-patterns #7） ---
static void drawText(const char* str, float x, float y, float z,
                     float sx, float sy, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, textBuf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, z, sx, sy, color);
}

// UTF-8文字の先頭バイトかどうか（継続バイト 0x80-0xBF は先頭ではない）
static bool isUtf8Lead(unsigned char c) {
    return (c & 0xC0) != 0x80;
}

// UTF-8文字境界を考慮して maxBytes バイト以内に切り詰める
static std::string utf8Truncate(const std::string& s, int maxBytes) {
    if ((int)s.size() <= maxBytes) return s;
    int end = maxBytes;
    while (end > 0 && !isUtf8Lead((unsigned char)s[end]))
        --end;
    return s.substr(0, end);
}

// 文字列を指定バイト幅で折り返す。UTF-8文字境界で分割する。
static std::vector<std::string> wrapText(const std::string& src, int maxBytes) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < src.size()) {
        // 改行文字があれば優先
        size_t nl = src.find('\n', pos);
        if (nl != std::string::npos && (int)(nl - pos) <= maxBytes) {
            lines.push_back(src.substr(pos, nl - pos));
            pos = nl + 1;
            continue;
        }

        if ((int)(src.size() - pos) <= maxBytes) {
            lines.push_back(src.substr(pos));
            break;
        }

        // maxBytes バイト目がUTF-8継続バイトなら先頭バイトまで戻る
        size_t split = pos + maxBytes;
        while (split > pos && !isUtf8Lead((unsigned char)src[split]))
            --split;

        lines.push_back(src.substr(pos, split - pos));
        pos = split;
    }
    return lines;
}

// --- 各画面の描画 ---

static void drawFeedList(const AppState& state) {
    // 上画面: アプリタイトル
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawText("3DS RSS Reader", TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f,
             TEXT_SCALE * 1.2f, TEXT_SCALE * 1.2f, CLR_TITLE);
    drawText("Select a feed on the bottom screen.", TEXT_MARGIN_X, 40.0f, 0.5f,
             TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X, 70.0f, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, C2D_Color32(0xFF, 0x80, 0x80, 0xFF));
    }

    // 下画面: フィード一覧
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    int total = (int)state.feeds.size();
    // 表示開始インデックス（カーソルが画面内に収まるようスクロール）
    int start = state.selectedFeed - BOT_MAX_LINES / 2;
    if (start < 0) start = 0;
    if (start > total - BOT_MAX_LINES && total > BOT_MAX_LINES)
        start = total - BOT_MAX_LINES;

    for (int i = start; i < total && (i - start) < BOT_MAX_LINES; ++i) {
        float y = TEXT_MARGIN_Y + (float)(i - start) * LINE_HEIGHT;
        if (i == state.selectedFeed) {
            C2D_DrawRectSolid(0, y - 1.0f, 0.0f, BOT_W, LINE_HEIGHT + 1.0f, CLR_SEL_BG);
        }
        char label[256];
        snprintf(label, sizeof(label), "%s", state.feeds[i].title.c_str());
        drawText(label, TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    if (total == 0) {
        drawText("No feeds loaded.", TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }

    // 操作ガイド
    drawText("Up/Down:move  A:open  START:quit", TEXT_MARGIN_X, BOT_H - 16.0f,
             0.5f, 0.42f, 0.42f, CLR_HINT);
}

static void drawArticleList(const AppState& state) {
    const Feed& feed = state.feeds[state.selectedFeed];

    // 上画面: フィードタイトル
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawText(feed.title.c_str(), TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f,
             TEXT_SCALE * 1.1f, TEXT_SCALE * 1.1f, CLR_TITLE);

    // 下画面: 記事一覧
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    int total = (int)feed.articles.size();
    int start = state.selectedArticle - BOT_MAX_LINES / 2;
    if (start < 0) start = 0;
    if (start > total - BOT_MAX_LINES && total > BOT_MAX_LINES)
        start = total - BOT_MAX_LINES;

    for (int i = start; i < total && (i - start) < BOT_MAX_LINES; ++i) {
        float y = TEXT_MARGIN_Y + (float)(i - start) * LINE_HEIGHT;
        if (i == state.selectedArticle) {
            C2D_DrawRectSolid(0, y - 1.0f, 0.0f, BOT_W, LINE_HEIGHT + 1.0f, CLR_SEL_BG);
        }
        std::string label = utf8Truncate(feed.articles[i].title, 45);
        drawText(label.c_str(), TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    if (total == 0) {
        drawText("No articles.", TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }

    drawText("Up/Down:move  A:read  B:back", TEXT_MARGIN_X, BOT_H - 16.0f,
             0.5f, 0.42f, 0.42f, CLR_HINT);
}

static void drawArticleView(const AppState& state) {
    const Article& art = state.feeds[state.selectedFeed].articles[state.selectedArticle];

    // コンテンツをテキスト変換してキャッシュ（この画面でのみ使用する一時変数）
    std::string plain = stripHtml(art.content);

    // 折り返し: TOP_W / (6px/char at scale 0.5) ≈ 65文字
    static constexpr int CHARS_PER_LINE = 65;
    std::vector<std::string> lines = wrapText(plain, CHARS_PER_LINE);

    // 上画面: 本文
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);

    int totalLines = (int)lines.size();
    int maxScroll  = totalLines > TOP_MAX_LINES ? totalLines - TOP_MAX_LINES : 0;
    int scroll     = state.scrollY;
    if (scroll > maxScroll) scroll = maxScroll;

    for (int i = 0; i < TOP_MAX_LINES && (scroll + i) < totalLines; ++i) {
        float y = TEXT_MARGIN_Y + (float)i * LINE_HEIGHT;
        drawText(lines[scroll + i].c_str(), TEXT_MARGIN_X, y, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    // 下画面: 記事タイトル + 操作ガイド
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    // タイトルを折り返して最大2行表示
    std::vector<std::string> titleLines = wrapText(art.title, 45);
    for (int i = 0; i < (int)titleLines.size() && i < 2; ++i) {
        drawText(titleLines[i].c_str(), TEXT_MARGIN_X,
                 TEXT_MARGIN_Y + (float)i * LINE_HEIGHT, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_TITLE);
    }

    char scrollInfo[32];
    snprintf(scrollInfo, sizeof(scrollInfo), "Line %d / %d", scroll + 1, totalLines);
    drawText(scrollInfo, TEXT_MARGIN_X, BOT_H - 30.0f, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    drawText("Up/Down:scroll  B:back", TEXT_MARGIN_X, BOT_H - 16.0f,
             0.5f, 0.42f, 0.42f, CLR_HINT);
}

// --- パブリック関数 ---

void uiDraw(const AppState& state) {
    C2D_TextBufClear(textBuf);  // 毎フレームクリア（coding-patterns #7）

    switch (state.currentScreen) {
        case Screen::FeedList:    drawFeedList(state);    break;
        case Screen::ArticleList: drawArticleList(state); break;
        case Screen::ArticleView: drawArticleView(state); break;
    }
}

void uiHandleInput(AppState& state, u32 kDown, u32 kHeld) {
    (void)kHeld;

    switch (state.currentScreen) {
        case Screen::FeedList: {
            int total = (int)state.feeds.size();
            if ((kDown & KEY_DOWN) && state.selectedFeed < total - 1)
                ++state.selectedFeed;
            if ((kDown & KEY_UP) && state.selectedFeed > 0)
                --state.selectedFeed;
            if ((kDown & KEY_A) && total > 0) {
                state.currentScreen  = Screen::ArticleList;
                state.selectedArticle = 0;
                kDown &= ~KEY_A;  // 同フレーム二重処理防止 (coding-patterns #6)
            }
            break;
        }
        case Screen::ArticleList: {
            const Feed& feed = state.feeds[state.selectedFeed];
            int total = (int)feed.articles.size();
            if ((kDown & KEY_DOWN) && state.selectedArticle < total - 1)
                ++state.selectedArticle;
            if ((kDown & KEY_UP) && state.selectedArticle > 0)
                --state.selectedArticle;
            if ((kDown & KEY_A) && total > 0) {
                state.currentScreen = Screen::ArticleView;
                state.scrollY       = 0;
                kDown &= ~KEY_A;  // coding-patterns #6

                // 本文が短い場合はリンク先HTMLから取得
                Article& art = state.feeds[state.selectedFeed]
                                          .articles[state.selectedArticle];
                if ((int)art.content.size() < CONTENT_SHORT_THRESHOLD
                    && !art.link.empty()) {
                    state.statusMsg = "Loading article...";
                    std::string errMsg;
                    std::string body = fetchArticleBody(art.link, errMsg);
                    if (!body.empty()) {
                        art.content = std::move(body);
                        state.statusMsg = "";
                    } else {
                        state.statusMsg = std::string("Fetch failed: ") + errMsg;
                    }
                }
            }
            if (kDown & KEY_B) {
                state.currentScreen = Screen::FeedList;
                kDown &= ~KEY_B;
            }
            break;
        }
        case Screen::ArticleView: {
            if (kDown & KEY_DOWN) ++state.scrollY;
            if (kDown & KEY_UP && state.scrollY > 0) --state.scrollY;
            if (kDown & KEY_B) {
                state.currentScreen = Screen::ArticleList;
                kDown &= ~KEY_B;
            }
            break;
        }
    }
}
