#include "ui.h"
#include "html_strip.h"
#include "article.h"
#include <citro2d.h>
#include <cstdio>
#include <cstring>

static constexpr int CONTENT_SHORT_THRESHOLD = 200;

// 画面サイズ定数（1箇所に集約）
static constexpr int   TOP_W          = 400;
static constexpr int   TOP_H          = 240;
static constexpr int   BOT_W          = 320;
static constexpr int   BOT_H          = 240;

// テキスト設定
static constexpr float TEXT_SCALE     = 0.5f;
static constexpr float LINE_HEIGHT    = 16.0f;
static constexpr float TEXT_MARGIN_X  = 6.0f;
static constexpr float TEXT_MARGIN_Y  = 6.0f;


// テキスト折り返し用ピクセル幅
// wrapText の高速推定用。citro2d による実幅検証で最終的に正確にトリムされる
static constexpr int   TOP_WRAP_PX         = 375;
static constexpr int   BOT_WRAP_PX         = 308;
static constexpr float HEADING_SCALE_FACTOR = 1.3f;

// 記事一覧タイトルの水平スクロール刻み幅(px)
// 将来: AppConfig::title_scroll_step に置き換え可能なよう1箇所に集約
static constexpr int   TITLE_SCROLL_STEP_PX = 50;

static constexpr int TOP_MAX_LINES =
    (int)((TOP_H - TEXT_MARGIN_Y * 2) / LINE_HEIGHT);
static constexpr int BOT_MAX_LINES =
    (int)((BOT_H - 30.0f) / LINE_HEIGHT);  // 30px は下部ガイド

// カラー
static constexpr u32 CLR_BG     = C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF);
static constexpr u32 CLR_PANEL  = C2D_Color32(0x16, 0x21, 0x3e, 0xFF);
static constexpr u32 CLR_TEXT   = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
static constexpr u32 CLR_SEL_BG = C2D_Color32(0x0f, 0x3d, 0x60, 0xFF);
static constexpr u32 CLR_HINT   = C2D_Color32(0xA0, 0xA0, 0xA0, 0xFF);
static constexpr u32 CLR_TITLE  = C2D_Color32(0x5c, 0xd4, 0xff, 0xFF);
static constexpr u32 CLR_ERROR  = C2D_Color32(0xFF, 0x80, 0x80, 0xFF);

static C3D_RenderTarget* topTarget  = nullptr;
static C3D_RenderTarget* botTarget  = nullptr;
static C2D_TextBuf       textBuf    = nullptr;
static C2D_TextBuf       measureBuf = nullptr;  // 幅測定専用バッファ

// テキストスタイル（将来: Heading2, Bold, Caption 等を追加可能）
enum class TextStyle { Body, Heading };

struct StyleParams {
    float scale;
    u32   color;
};

static StyleParams resolveStyle(TextStyle style) {
    switch (style) {
        case TextStyle::Heading: return { TEXT_SCALE * 1.3f, CLR_TITLE };
        case TextStyle::Body:
        default:                 return { TEXT_SCALE,        CLR_TEXT  };
    }
}

void uiInit() {
    topTarget  = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    botTarget  = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf    = C2D_TextBufNew(4096);
    measureBuf = C2D_TextBufNew(512);
}

void uiExit() {
    C2D_TextBufDelete(measureBuf);
    measureBuf = nullptr;
    C2D_TextBufDelete(textBuf);
    textBuf = nullptr;
}

// --- ヘルパー ---

static void drawText(const char* str, float x, float y, float z,
                     float sx, float sy, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, textBuf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, z, sx, sy, color);
}

static void drawStyledText(const char* str, float x, float y,
                           float z, TextStyle style) {
    StyleParams p = resolveStyle(style);
    drawText(str, x, y, z, p.scale, p.scale, p.color);
}

// UTF-8先頭バイトからピクセル幅を推定（TEXT_SCALE=0.5 前提）
// ASCII 半角: 6px, 2バイト文字: 8px, CJK 全角: 12px
static int utf8CharPx(unsigned char lead) {
    if (lead < 0x80)               return 6;
    if ((lead & 0xE0) == 0xC0)     return 8;
    if ((lead & 0xF0) == 0xE0)     return 12;
    return 12; // 4バイト（絵文字等）: フルwidth相当で保守的に推定
}

// citro2d の実幅（scale=1.0 時の width）を返す
static float measureStr(const char* str, float scale) {
    C2D_TextBufClear(measureBuf);
    C2D_Text t;
    C2D_TextParse(&t, measureBuf, str);
    return t.width * scale;
}

// str の末尾 UTF-8 文字を削除しながら maxPx 以内に収める
static void trimToWidth(std::string& str, int maxPx, float scale) {
    while (!str.empty() && measureStr(str.c_str(), scale) > (float)maxPx) {
        size_t i = str.size() - 1;
        while (i > 0 && ((unsigned char)str[i] & 0xC0) == 0x80) --i;
        str.erase(i);
    }
}

static std::vector<std::string> wrapText(const std::string& src, int maxPixels,
                                          float scale = TEXT_SCALE) {
    const float scaleRatio = scale / TEXT_SCALE;
    std::vector<std::string> lines;
    size_t pos = 0;
    const size_t len = src.size();
    while (pos < len) {
        size_t nl   = src.find('\n', pos);
        size_t hard = (nl != std::string::npos) ? nl : len;

        // hard まで何ピクセル入るか走査（スケール補正済みの高速推定）
        int    px    = 0;
        size_t split = pos;
        while (split < hard) {
            unsigned char c = (unsigned char)src[split];
            int bytes = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            int cpx   = (int)(utf8CharPx(c) * scaleRatio + 0.5f);
            if (px + cpx > maxPixels) break;
            px    += cpx;
            split += bytes;
        }

        std::string line;
        if (split == hard) {
            line = src.substr(pos, hard - pos);
            pos  = hard + (nl != std::string::npos ? 1 : 0);
        } else {
            line = src.substr(pos, split - pos);
            pos  = split;
        }

        // 推定が上限の90%以上の場合のみ実幅検証（大半の行はスキップ）
        if (px * 10 >= maxPixels * 9)
            trimToWidth(line, maxPixels, scale);
        lines.push_back(std::move(line));
    }
    return lines;
}

// --- 各画面の描画 ---

static void drawLoadingScreen(const AppState& state) {
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawText("Loading...", TEXT_MARGIN_X, TOP_H / 2.0f - 14.0f,
             0.5f, TEXT_SCALE * 1.2f, TEXT_SCALE * 1.2f, CLR_TITLE);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X,
                 TOP_H / 2.0f + 4.0f, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);
}

static void drawFeedList(const AppState& state) {
    // 上画面: アプリタイトル
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);

    drawStyledText("3DS RSS Reader", TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f,
                   TextStyle::Heading);
    drawText("Select a feed on the bottom screen.", TEXT_MARGIN_X, 40.0f, 0.5f,
             TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X, 70.0f, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_ERROR);
    }

    // 下画面: フィード一覧（feedConfigs から名前を取得）
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    int total = (int)state.feedConfigs.size();
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
        snprintf(label, sizeof(label), "%s",
                 state.feedConfigs[i].name.c_str());
        drawText(label, TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    if (total == 0) {
        drawText("No feeds. Add feeds.json to SD card.", TEXT_MARGIN_X,
                 TEXT_MARGIN_Y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }

    drawText("Up/Down:move  A:open  START:quit", TEXT_MARGIN_X, BOT_H - 16.0f,
             0.5f, 0.42f, 0.42f, CLR_HINT);
}

static void drawArticleList(const AppState& state) {
    int idx = state.selectedFeed;
    const Feed& feed = state.feeds[idx];

    // 上画面: フィードタイトル（feeds に title がなければ feedConfigs の name を使用）
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    const std::string& title = feed.title.empty()
        ? state.feedConfigs[idx].name
        : feed.title;
    drawStyledText(title.c_str(), TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f,
                   TextStyle::Heading);

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

        const std::string& fullTitle = feed.articles[i].title;

        if (i == state.selectedArticle) {
            // 選択行: 生タイトルを描画し、水平スクロールを適用。
            // タイトル末尾が右端に到達したところで止まる（タイトル全体が見える）。
            float textW    = measureStr(fullTitle.c_str(), TEXT_SCALE);
            float displayW = (float)BOT_WRAP_PX;
            float maxScroll = (textW > displayW) ? (textW - displayW) : 0.0f;
            float effScroll = (float)state.articleListScrollX;
            if (effScroll > maxScroll) effScroll = maxScroll;
            drawText(fullTitle.c_str(), TEXT_MARGIN_X - effScroll, y, 0.5f,
                     TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
        } else {
            // 非選択行: 先頭の1行のみを表示（現状維持）
            auto lw = wrapText(fullTitle, BOT_WRAP_PX);
            std::string label = lw.empty() ? "" : lw.front();
            drawText(label.c_str(), TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
        }
    }

    if (total == 0) {
        drawText("No articles.", TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }

    drawText("Up/Down:move  A:read  B:back", TEXT_MARGIN_X, BOT_H - 16.0f,
             0.5f, 0.42f, 0.42f, CLR_HINT);
}

static void drawArticleView(const AppState& state) {
    const Article& art =
        state.feeds[state.selectedFeed].articles[state.selectedArticle];

    std::string plain = stripHtml(art.content);
    std::vector<std::string> lines = wrapText(plain, TOP_WRAP_PX);

    // 上画面: 本文
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);

    int totalLines = (int)lines.size();
    int maxScroll  = totalLines > TOP_MAX_LINES ? totalLines - TOP_MAX_LINES : 0;
    int scroll     = state.scrollY < maxScroll ? state.scrollY : maxScroll;

    for (int i = 0; i < TOP_MAX_LINES && (scroll + i) < totalLines; ++i) {
        float y = TEXT_MARGIN_Y + (float)i * LINE_HEIGHT;
        drawText(lines[scroll + i].c_str(), TEXT_MARGIN_X, y, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    // 下画面: 記事タイトル + 操作ガイド
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    std::vector<std::string> titleLines = wrapText(art.title, BOT_WRAP_PX,
                                                    TEXT_SCALE * HEADING_SCALE_FACTOR);
    for (int i = 0; i < (int)titleLines.size() && i < 2; ++i) {
        drawStyledText(titleLines[i].c_str(), TEXT_MARGIN_X,
                       TEXT_MARGIN_Y + (float)i * LINE_HEIGHT, 0.5f,
                       TextStyle::Heading);
    }

    char scrollInfo[32];
    snprintf(scrollInfo, sizeof(scrollInfo), "Line %d / %d", scroll + 1, totalLines);
    drawText(scrollInfo, TEXT_MARGIN_X, BOT_H - 30.0f, 0.5f,
             TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    const char* guide = (!art.fullFetched && !art.link.empty())
        ? "Up/Down:scroll  A:full article  B:back"
        : "Up/Down:scroll  B:back";
    drawText(guide, TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f, 0.42f, 0.42f, CLR_HINT);
}

// --- フェッチヘルパー ---

// art.link からHTMLを取得してart.contentを更新する。成功時trueを返す。
static bool doFetchArticle(Article& art, AppState& state, const char* loadingMsg) {
    state.statusMsg = loadingMsg;
    std::string errMsg;
    std::string body = fetchArticleBody(art.link, errMsg);
    if (!body.empty()) {
        art.content     = std::move(body);
        state.statusMsg = "";
        return true;
    }
    state.statusMsg = std::string("Fetch failed: ") + errMsg;
    return false;
}

// --- パブリック関数 ---

void uiDraw(const AppState& state) {
    C2D_TextBufClear(textBuf);

    switch (state.currentScreen) {
        case Screen::FeedList:    drawFeedList(state);    break;
        case Screen::Loading:     drawLoadingScreen(state); break;
        case Screen::ArticleList: drawArticleList(state); break;
        case Screen::ArticleView: drawArticleView(state); break;
    }
}

void uiHandleInput(AppState& state, u32 kDown, u32 kHeld, u32 kRepeat) {
    (void)kHeld;
    // D-pad は kRepeat を使用（libctru 組み込みの長押しリピート）。
    // A/B/START などの確定ボタンは引き続き kDown を使用する。

    switch (state.currentScreen) {
        case Screen::FeedList: {
            int total = (int)state.feedConfigs.size();
            if ((kRepeat & KEY_DOWN) && state.selectedFeed < total - 1)
                ++state.selectedFeed;
            if ((kRepeat & KEY_UP) && state.selectedFeed > 0)
                --state.selectedFeed;
            if ((kDown & KEY_A) && total > 0) {
                const FeedConfig& cfg = state.feedConfigs[state.selectedFeed];
                state.statusMsg = cfg.name.empty() ? cfg.url : cfg.name;
                state.currentScreen   = Screen::Loading;
                state.selectedArticle = 0;
                kDown &= ~KEY_A;  // coding-patterns #6
            }
            break;
        }
        case Screen::Loading:
            // main.cpp のループで処理するため入力は無視
            break;
        case Screen::ArticleList: {
            int idx   = state.selectedFeed;
            int total = (int)state.feeds[idx].articles.size();
            if (total > 0) {
                if ((kRepeat & KEY_DOWN) && state.selectedArticle < total - 1) {
                    ++state.selectedArticle;
                    state.articleListScrollX = 0;
                }
                if ((kRepeat & KEY_UP) && state.selectedArticle > 0) {
                    --state.selectedArticle;
                    state.articleListScrollX = 0;
                }
                // LEFT/RIGHT: 選択タイトルの水平スクロール。
                // 上限は選択タイトルの実幅に依存するので、ここで算出してクランプする
                // （state の値が青天井にならないようにするため）。
                if (kRepeat & (KEY_LEFT | KEY_RIGHT)) {
                    const std::string& title =
                        state.feeds[idx].articles[state.selectedArticle].title;
                    float textW    = measureStr(title.c_str(), TEXT_SCALE);
                    float displayW = (float)BOT_WRAP_PX;
                    int maxScroll  = (textW > displayW)
                        ? (int)(textW - displayW + 0.5f) : 0;
                    if (kRepeat & KEY_RIGHT) {
                        state.articleListScrollX += TITLE_SCROLL_STEP_PX;
                        if (state.articleListScrollX > maxScroll)
                            state.articleListScrollX = maxScroll;
                    }
                    if (kRepeat & KEY_LEFT) {
                        state.articleListScrollX -= TITLE_SCROLL_STEP_PX;
                        if (state.articleListScrollX < 0)
                            state.articleListScrollX = 0;
                    }
                }
            }
            if ((kDown & KEY_A) && total > 0) {
                state.currentScreen = Screen::ArticleView;
                state.scrollY       = 0;
                kDown &= ~KEY_A;  // coding-patterns #6

                Article& art = state.feeds[idx].articles[state.selectedArticle];
                if ((int)art.content.size() < CONTENT_SHORT_THRESHOLD
                    && !art.link.empty()) {
                    doFetchArticle(art, state, "Loading article...");
                }
            }
            if (kDown & KEY_B) {
                state.currentScreen      = Screen::FeedList;
                state.articleListScrollX = 0;
                kDown &= ~KEY_B;
            }
            break;
        }
        case Screen::ArticleView: {
            if (kRepeat & KEY_DOWN) ++state.scrollY;
            if ((kRepeat & KEY_UP) && state.scrollY > 0) --state.scrollY;
            if ((kDown & KEY_A)) {
                Article& art = state.feeds[state.selectedFeed]
                                    .articles[state.selectedArticle];
                if (!art.fullFetched && !art.link.empty()) {
                    if (doFetchArticle(art, state, "Loading full article...")) {
                        art.fullFetched = true;
                        state.scrollY   = 0;
                    }
                }
                kDown &= ~KEY_A;
            }
            if (kDown & KEY_B) {
                state.currentScreen = Screen::ArticleList;
                state.statusMsg     = "";
                kDown &= ~KEY_B;
            }
            break;
        }
    }
}
