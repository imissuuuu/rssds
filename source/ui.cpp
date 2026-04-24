#include "ui.h"
#include "article.h"
#include "html_strip.h"
#include "settings.h"
#include <algorithm>
#include <citro2d.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <numeric>
#include <unordered_set>

static constexpr int CONTENT_SHORT_THRESHOLD = 200;

// 画面サイズ定数（1箇所に集約）
static constexpr int TOP_W = 400;
static constexpr int TOP_H = 240;
static constexpr int BOT_W = 320;
static constexpr int BOT_H = 240;

// テキスト設定
static constexpr float TEXT_SCALE = 0.5f;
static constexpr float LINE_HEIGHT = 16.0f;
static constexpr float TEXT_MARGIN_X = 6.0f;
static constexpr float TEXT_MARGIN_Y = 6.0f;

// ステータスバー
static constexpr float STATUSBAR_H = 14.0f;
static constexpr float TOP_CONTENT_Y = STATUSBAR_H + TEXT_MARGIN_Y; // 20.0f

// インライン画像（LINE_HEIGHT=16 の整数倍）
static constexpr int IMG_INLINE_H = 192;
static constexpr int IMG_INLINE_LINES = 12; // IMG_INLINE_H / LINE_HEIGHT

// テキスト折り返し用ピクセル幅
// wrapText の高速推定用。citro2d による実幅検証で最終的に正確にトリムされる
static constexpr int TOP_WRAP_PX = 388;
static constexpr int BOT_WRAP_PX = 308;
static constexpr float HEADING_SCALE_FACTOR = 1.3f;

// 記事一覧タイトルの水平スクロール刻み幅(px)
// 将来: AppConfig::title_scroll_step に置き換え可能なよう1箇所に集約
static constexpr int TITLE_SCROLL_STEP_PX = 50;

static constexpr int TOP_MAX_LINES = (int)((TOP_H - STATUSBAR_H - TEXT_MARGIN_Y * 2) / LINE_HEIGHT);
static constexpr int BOT_MAX_LINES = (int)((BOT_H - 30.0f) / LINE_HEIGHT); // 30px は下部ガイド

// カラー
static constexpr u32 CLR_BG = C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF);
static constexpr u32 CLR_PANEL = C2D_Color32(0x16, 0x21, 0x3e, 0xFF);
static constexpr u32 CLR_TEXT = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
static constexpr u32 CLR_SEL_BG = C2D_Color32(0x0f, 0x3d, 0x60, 0xFF);
static constexpr u32 CLR_HINT = C2D_Color32(0xA0, 0xA0, 0xA0, 0xFF);
static constexpr u32 CLR_TITLE = C2D_Color32(0x5c, 0xd4, 0xff, 0xFF);
static constexpr u32 CLR_ERROR = C2D_Color32(0xFF, 0x80, 0x80, 0xFF);

static C3D_RenderTarget* topTarget = nullptr;
static C3D_RenderTarget* botTarget = nullptr;
static C2D_TextBuf textBuf = nullptr;
static C2D_TextBuf measureBuf = nullptr; // 幅測定専用バッファ
static C2D_Font fallbackFont = nullptr;

// ステータスバー用キャッシュ（5秒ごとに更新）
static u8 s_battLevel = 3;
static u8 s_battCharging = 0;
static u8 s_wifiStatus = 0; // 0-3: osGetWifiStrength()
static u64 s_statusLastMs = 0;

// テキストスタイル（将来: Heading2, Bold, Caption 等を追加可能）
enum class TextStyle { Body, Heading };

struct StyleParams {
    float scale;
    u32 color;
};

static StyleParams resolveStyle(TextStyle style) {
    switch (style) {
    case TextStyle::Heading:
        return {TEXT_SCALE * 1.3f, CLR_TITLE};
    case TextStyle::Body:
    default:
        return {TEXT_SCALE, CLR_TEXT};
    }
}

void uiInit() {
    topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    botTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf = C2D_TextBufNew(4096);
    measureBuf = C2D_TextBufNew(512);
    fallbackFont = C2D_FontLoad("romfs:/fallback.bcfnt");
    if (!fallbackFont) {
        fprintf(stderr, "[ui] fallback font not loaded\n");
    }
}

void uiExit() {
    if (fallbackFont) {
        C2D_FontFree(fallbackFont);
        fallbackFont = nullptr;
    }
    C2D_TextBufDelete(measureBuf);
    measureBuf = nullptr;
    C2D_TextBufDelete(textBuf);
    textBuf = nullptr;
}

// --- ヘルパー ---

// UTF-8 1文字をデコードしコードポイントとバイト数を返す。
// 不正バイトは U+FFFD として 1 バイト進める。
static uint32_t utf8Decode(const char* s, int& outBytes) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        outBytes = 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        outBytes = 2;
        return ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {
        outBytes = 3;
        return ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
               ((unsigned char)s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0) {
        outBytes = 4;
        return ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
               (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
    }
    outBytes = 1;
    return 0xFFFD;
}

// システムフォントにグリフがあるか（alterCharIndex と一致しなければあり）
static bool systemHasGlyph(uint32_t cp) {
    CFNT_s* sys = fontGetSystemFont();
    int alter = sys->finf.alterCharIndex;
    return fontGlyphIndexFromCodePoint(sys, cp) != alter;
}

static void drawText(const char* str, float x, float y, float z, float sx, float sy, u32 color) {
    if (!fallbackFont) {
        C2D_Text text;
        C2D_TextParse(&text, textBuf, str);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, x, y, z, sx, sy, color);
        return;
    }
    float cursorX = x;
    size_t n = strlen(str), i = 0;
    while (i < n) {
        int bytes;
        uint32_t cp = utf8Decode(str + i, bytes);
        bool useSys = systemHasGlyph(cp);
        size_t j = i + bytes;
        while (j < n) {
            int nb;
            uint32_t ncp = utf8Decode(str + j, nb);
            if (systemHasGlyph(ncp) != useSys)
                break;
            j += nb;
        }
        std::string chunk(str + i, j - i);
        C2D_Text text;
        if (useSys)
            C2D_TextParse(&text, textBuf, chunk.c_str());
        else
            C2D_TextFontParse(&text, fallbackFont, textBuf, chunk.c_str());
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, cursorX, y, z, sx, sy, color);
        cursorX += text.width * sx;
        i = j;
    }
}

static void drawStyledText(const char* str, float x, float y, float z, TextStyle style) {
    StyleParams p = resolveStyle(style);
    drawText(str, x, y, z, p.scale, p.scale, p.color);
}

// UTF-8先頭バイトからピクセル幅を推定（TEXT_SCALE=0.5 前提）
// ASCII 半角: 6px, 2バイト文字: 8px, CJK 全角: 12px
static int utf8CharPx(unsigned char lead) {
    if (lead < 0x80)
        return 6;
    if ((lead & 0xE0) == 0xC0)
        return 8;
    if ((lead & 0xF0) == 0xE0)
        return 12;
    return 12; // 4バイト（絵文字等）: フルwidth相当で保守的に推定
}

// citro2d の実幅（scale=1.0 時の width）を返す
static float measureStr(const char* str, float scale) {
    C2D_TextBufClear(measureBuf);
    if (!fallbackFont) {
        C2D_Text t;
        C2D_TextParse(&t, measureBuf, str);
        return t.width * scale;
    }
    float total = 0.0f;
    size_t n = strlen(str), i = 0;
    while (i < n) {
        int bytes;
        uint32_t cp = utf8Decode(str + i, bytes);
        bool useSys = systemHasGlyph(cp);
        size_t j = i + bytes;
        while (j < n) {
            int nb;
            uint32_t ncp = utf8Decode(str + j, nb);
            if (systemHasGlyph(ncp) != useSys)
                break;
            j += nb;
        }
        std::string chunk(str + i, j - i);
        C2D_Text t;
        if (useSys)
            C2D_TextParse(&t, measureBuf, chunk.c_str());
        else
            C2D_TextFontParse(&t, fallbackFont, measureBuf, chunk.c_str());
        total += t.width;
        i = j;
    }
    return total * scale;
}

// str の末尾 UTF-8 文字を削除しながら maxPx 以内に収める
static void trimToWidth(std::string& str, int maxPx, float scale) {
    while (!str.empty() && measureStr(str.c_str(), scale) > (float)maxPx) {
        size_t i = str.size() - 1;
        while (i > 0 && ((unsigned char)str[i] & 0xC0) == 0x80)
            --i;
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
        size_t nl = src.find('\n', pos);
        size_t hard = (nl != std::string::npos) ? nl : len;

        // hard まで何ピクセル入るか走査（スケール補正済みの高速推定）
        int px = 0;
        size_t split = pos;
        while (split < hard) {
            unsigned char c = (unsigned char)src[split];
            int bytes = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            int cpx = (int)(utf8CharPx(c) * scaleRatio + 0.5f);
            if (px + cpx > maxPixels)
                break;
            px += cpx;
            split += bytes;
        }

        std::string line;
        bool softWrap = (split != hard);
        if (!softWrap) {
            line = src.substr(pos, hard - pos);
            pos = hard + (nl != std::string::npos ? 1 : 0);
        } else {
            line = src.substr(pos, split - pos);
            pos = split;
        }

        // 全行で実幅検証（高速推定の過小評価によるオーバーフローを防止）
        // trimToWidth で削除したバイトを pos に戻す（soft/hard 両経路で消失防止）
        size_t lineBytes = line.size();
        trimToWidth(line, maxPixels, scale);
        size_t trimmed = lineBytes - line.size();
        if (trimmed > 0 && trimmed < lineBytes)
            pos -= trimmed;
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    return lines;
}

// 本文（\x01URL\x01 マーカー入り）を ContentLine ベクタに分割する。
// テキスト区間は wrapText でラップ、マーカーは Image 行に変換する。
static std::vector<ContentLine> parseContentLines(const std::string& body, int maxPx) {
    std::vector<ContentLine> result;
    size_t i = 0, n = body.size();
    while (i <= n) {
        size_t m = body.find('\x01', i);
        if (m == std::string::npos)
            m = n;

        // テキスト区間 [i, m)
        if (m > i) {
            std::string seg = body.substr(i, m - i);
            for (auto& tl : wrapText(seg, maxPx)) {
                ContentLine cl;
                cl.kind = LineKind::Text;
                cl.text = std::move(tl);
                result.push_back(std::move(cl));
            }
        }
        if (m >= n)
            break;

        // 画像マーカー \x01URL\x01
        size_t urlStart = m + 1;
        size_t urlEnd = body.find('\x01', urlStart);
        if (urlEnd == std::string::npos) {
            i = urlStart;
            continue;
        }
        std::string url = body.substr(urlStart, urlEnd - urlStart);
        if (!url.empty()) {
            ContentLine cl;
            cl.kind = LineKind::Image;
            cl.imageUrl = std::move(url);
            result.push_back(std::move(cl));
        }
        i = urlEnd + 1;
    }
    return result;
}

// --- 設定画面用定数 ---

static const int SCROLL_DELAY_OPTIONS[] = {200, 300, 400, 500};
static const int SCROLL_INTERVAL_OPTIONS[] = {50, 80, 120, 160};
static const int NUM_DELAY_OPTS = 4;
static const int NUM_INTERVAL_OPTS = 4;

// arr[0..n-1] 内で val のインデックスを返す。見つからなければ 0。
static int findIndex(const int* arr, int n, int val) {
    for (int i = 0; i < n; ++i)
        if (arr[i] == val)
            return i;
    return 0;
}

// --- ステータスバー ---

static void drawStatusBar() {
    // バッテリー・WiFi: 5秒ごとに更新
    u64 now = osGetTime();
    if (now - s_statusLastMs > 5000) {
        PTMU_GetBatteryLevel(&s_battLevel);
        PTMU_GetBatteryChargeState(&s_battCharging);
        s_wifiStatus = osGetWifiStrength();
        s_statusLastMs = now;
    }

    // 現在時刻
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char timeBuf[8];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);

    // WiFi バー (■=U+25A0, □=U+25A1 — ホワイトリスト済み)
    static const char* WIFI_BARS[4] = {
        "\xe2\x96\xa1\xe2\x96\xa1\xe2\x96\xa1", // □□□
        "\xe2\x96\xa0\xe2\x96\xa1\xe2\x96\xa1", // ■□□
        "\xe2\x96\xa0\xe2\x96\xa0\xe2\x96\xa1", // ■■□
        "\xe2\x96\xa0\xe2\x96\xa0\xe2\x96\xa0", // ■■■
    };
    u8 wifiIdx = s_wifiStatus < 4 ? s_wifiStatus : 3;

    // バッテリー文字列（0-5段階 → 20%単位）
    char battBuf[12];
    int pct = (int)s_battLevel * 20;
    if (s_battCharging)
        snprintf(battBuf, sizeof(battBuf), "%d%%+", pct);
    else
        snprintf(battBuf, sizeof(battBuf), "%d%%", pct);

    // 右端表示: WiFi バー + スペース + バッテリー
    char rightBuf[32];
    snprintf(rightBuf, sizeof(rightBuf), "%s %s", WIFI_BARS[wifiIdx], battBuf);

    // 背景帯
    C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, STATUSBAR_H, CLR_PANEL);

    // 時刻（左端）
    drawText(timeBuf, TEXT_MARGIN_X, 1.0f, 0.5f, 0.42f, 0.42f, CLR_HINT);

    // WiFi + バッテリー（右端）
    float rightW = measureStr(rightBuf, 0.42f);
    drawText(rightBuf, TOP_W - rightW - TEXT_MARGIN_X, 1.0f, 0.5f, 0.42f, 0.42f, CLR_HINT);
}

// --- 各画面の描画 ---

static void drawLoadingScreen(const AppState& state) {
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();
    drawText("Loading...", TEXT_MARGIN_X, TOP_H / 2.0f - 14.0f + STATUSBAR_H / 2.0f, 0.5f,
             TEXT_SCALE * 1.2f, TEXT_SCALE * 1.2f, CLR_TITLE);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X, TOP_H / 2.0f + 4.0f + STATUSBAR_H / 2.0f,
                 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }
    float pct = 0.0f;
    if (state.currentScreen == Screen::LoadingArticle)
        pct = state.articleLoader.getProgress();
    else if (state.currentScreen == Screen::Loading)
        pct = state.feedLoader.getProgress();

    if (state.currentScreen == Screen::LoadingArticle || state.currentScreen == Screen::Loading) {
        constexpr float BAR_H = 10.0f;
        constexpr float BAR_W = TOP_W - TEXT_MARGIN_X * 2.0f;
        float barY = TOP_H / 2.0f + 20.0f + STATUSBAR_H / 2.0f;
        C2D_DrawRectSolid(TEXT_MARGIN_X, barY, 0.5f, BAR_W, BAR_H,
                          C2D_Color32(0x40, 0x40, 0x60, 0xFF));
        if (pct > 0.0f)
            C2D_DrawRectSolid(TEXT_MARGIN_X, barY, 0.5f, BAR_W * pct, BAR_H, CLR_TITLE);
        char pctBuf[20];
        snprintf(pctBuf, sizeof(pctBuf), "Loading... %d%%", (int)(pct * 100.0f));
        drawText(pctBuf, TEXT_MARGIN_X, barY + BAR_H + 3.0f, 0.5f, TEXT_SCALE, TEXT_SCALE,
                 CLR_HINT);
    }
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);
}

static void drawFeedList(const AppState& state) {
    // 上画面: アプリタイトル
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();

    drawStyledText("3DS RSS Reader", TEXT_MARGIN_X, TOP_CONTENT_Y, 0.5f, TextStyle::Heading);
    drawText("Select a feed on the bottom screen.", TEXT_MARGIN_X, 40.0f + STATUSBAR_H, 0.5f,
             TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X, 70.0f + STATUSBAR_H, 0.5f, TEXT_SCALE,
                 TEXT_SCALE, CLR_ERROR);
    }

    // 下画面: フィード一覧 + Settings エントリ（feedConfigs から名前を取得）
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    int feedCount = (int)state.feedConfigs.size();
    int total = feedCount + 2; // +1 for Bookmarks, +1 for Settings
    int start = state.selectedFeed - BOT_MAX_LINES / 2;
    if (start < 0)
        start = 0;
    if (start > total - BOT_MAX_LINES && total > BOT_MAX_LINES)
        start = total - BOT_MAX_LINES;

    char label[256];
    for (int i = start; i < total && (i - start) < BOT_MAX_LINES; ++i) {
        float y = TEXT_MARGIN_Y + (float)(i - start) * LINE_HEIGHT;
        if (i == state.selectedFeed) {
            C2D_DrawRectSolid(0, y - 1.0f, 0.0f, BOT_W, LINE_HEIGHT + 1.0f, CLR_SEL_BG);
        }
        if (i < feedCount)
            snprintf(label, sizeof(label), "%s", state.feedConfigs[i].name.c_str());
        else if (i == feedCount)
            snprintf(label, sizeof(label), "\xe2\x98\x85 Bookmarks");
        else
            snprintf(label, sizeof(label), "[Settings]");
        drawText(label, TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    if (feedCount == 0) {
        drawText("No feeds. Add feeds.json to SD card.", TEXT_MARGIN_X, TEXT_MARGIN_Y + LINE_HEIGHT,
                 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }

    drawText("Up/Down:move  A:open/enter  Y:refresh  START:quit", TEXT_MARGIN_X, BOT_H - 16.0f,
             0.5f, 0.42f, 0.42f, CLR_HINT);
}

static void drawArticleList(const AppState& state) {
    int idx = state.selectedFeed;
    const Feed& feed = state.feeds[idx];

    // 上画面: フィードタイトル（feeds に title がなければ feedConfigs の name を使用）
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();
    const std::string& title = feed.title.empty() ? state.feedConfigs[idx].name : feed.title;
    drawStyledText(title.c_str(), TEXT_MARGIN_X, TOP_CONTENT_Y, 0.5f, TextStyle::Heading);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X, 40.0f + STATUSBAR_H, 0.5f, TEXT_SCALE,
                 TEXT_SCALE, CLR_HINT);
    }

    // 下画面: 記事一覧
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    int total = (int)feed.articles.size();
    int start = state.selectedArticle - BOT_MAX_LINES / 2;
    if (start < 0)
        start = 0;
    if (start > total - BOT_MAX_LINES && total > BOT_MAX_LINES)
        start = total - BOT_MAX_LINES;

    for (int i = start; i < total && (i - start) < BOT_MAX_LINES; ++i) {
        float y = TEXT_MARGIN_Y + (float)(i - start) * LINE_HEIGHT;
        if (i == state.selectedArticle) {
            C2D_DrawRectSolid(0, y - 1.0f, 0.0f, BOT_W, LINE_HEIGHT + 1.0f, CLR_SEL_BG);
        }

        const Article& article = feed.articles[i];
        const std::string& fullTitle = article.title;
        bool isRead = state.readHistory.isRead(ReadHistory::keyFor(article.link, article.title));
        bool isBm = state.bookmarkStore.isBookmarked(article.link, article.title);
        u32 textClr = isRead ? CLR_HINT : CLR_TEXT;
        // ★ プレフィックス（ブックマーク済み）
        std::string displayTitle = isBm ? std::string("\xe2\x98\x85 ") + fullTitle : fullTitle;

        if (i == state.selectedArticle) {
            // 選択行: 生タイトルを描画し、水平スクロールを適用。
            // タイトル末尾が右端に到達したところで止まる（タイトル全体が見える）。
            float textW = measureStr(displayTitle.c_str(), TEXT_SCALE);
            float displayW = (float)BOT_WRAP_PX;
            float maxScroll = (textW > displayW) ? (textW - displayW) : 0.0f;
            float effScroll = (float)state.articleListScrollX;
            if (effScroll > maxScroll)
                effScroll = maxScroll;
            drawText(displayTitle.c_str(), TEXT_MARGIN_X - effScroll, y, 0.5f, TEXT_SCALE,
                     TEXT_SCALE, textClr);
        } else {
            // 非選択行: 先頭の1行のみを表示
            auto lw = wrapText(displayTitle, BOT_WRAP_PX);
            std::string label = lw.empty() ? "" : lw.front();
            drawText(label.c_str(), TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, textClr);
        }
    }

    if (total == 0) {
        drawText("No articles.", TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f, TEXT_SCALE, TEXT_SCALE,
                 CLR_HINT);
    }

    drawText("A:read  SEL:bm  Y:refresh  B:back", TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f, 0.42f, 0.42f,
             CLR_HINT);
}

/**
 * @brief Render the article view: draws the article body and images on the top screen and the title
 * plus controls on the bottom screen.
 *
 * Recomputes wrapped text and (re)initializes the article image cache when the selected feed,
 * article, or article content size changes. Computes and clamps vertical scroll bounds, updates the
 * state's cached max scroll, and renders visible text lines and a vertically stacked image section
 * (showing cached images, failure markers, or a progress bar with percentage). After image drawing,
 * advances image loading via the image cache tick. The bottom screen displays up to two wrapped
 * title lines, a "Line N / M" scroll indicator, and the appropriate control guide.
 *
 * Side effects:
 * - Updates state's cached line/feed/article/content tracking and state.cachedMaxScroll.
 * - May start/reset the image loader and attach/reset the image cache for the article.
 * - Calls state.imgCache.tick(...) with the set of visible image URLs.
 */
static void drawArticleView(const AppState& state) {
    const Article& art = state.viewingBookmark
                             ? state.bookmarkTempArticle
                             : state.feeds[state.selectedFeed].articles[state.selectedArticle];

    int cacheFeedKey = state.viewingBookmark ? -2 : state.selectedFeed;
    int cacheArticleKey = state.viewingBookmark ? -2 : state.selectedArticle;

    // キャッシュが無効なら再計算（フィード・記事・本文サイズのいずれかが変わった場合）
    if (state.cachedLineFeed != cacheFeedKey || state.cachedLineArticle != cacheArticleKey ||
        state.cachedLineContentSize != art.content.size()) {
        // stripHtml で HTML を除去しつつ \x01URL\x01 マーカーはそのまま通過させる
        std::string processed = stripHtml(art.content);
        state.articleLines = parseContentLines(processed, TOP_WRAP_PX);
        state.cachedLineFeed = cacheFeedKey;
        state.cachedLineArticle = cacheArticleKey;
        state.cachedLineContentSize = art.content.size();

        // ContentLines から inline 画像 URL を収集して imgCache を初期化
        std::vector<std::string> inlineUrls;
        for (const auto& cl : state.articleLines)
            if (cl.kind == LineKind::Image)
                inlineUrls.push_back(cl.imageUrl);

        state.imgLoader.start();
        state.imgCache.attach(&state.imgLoader);
        state.imgCache.resetForArticle(inlineUrls);
        state.cachedImagesFeed = cacheFeedKey;
        state.cachedImagesArticle = cacheArticleKey;
    }
    const std::vector<ContentLine>& lines = state.articleLines;

    // 上画面: 本文
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();

    // totalDisplayLines: 各 ContentLine の占有行数の合計
    int totalDisplayLines =
        std::accumulate(lines.begin(), lines.end(), 0, [](int acc, const ContentLine& cl) {
            return acc + ((cl.kind == LineKind::Image) ? IMG_INLINE_LINES : 1);
        });

    int maxScroll = totalDisplayLines > TOP_MAX_LINES ? totalDisplayLines - TOP_MAX_LINES : 0;
    state.cachedMaxScroll = maxScroll;
    int scroll = state.scrollY < maxScroll ? state.scrollY : maxScroll;

    // ContentLine を順に描画（テキスト行 / プログレスバー / インライン画像）
    std::unordered_set<std::string> visible;
    int displayY = 0;
    for (const auto& cl : lines) {
        int clLines = (cl.kind == LineKind::Image) ? IMG_INLINE_LINES : 1;

        if (displayY + clLines > scroll && displayY < scroll + TOP_MAX_LINES) {
            float screenY = TOP_CONTENT_Y + (float)(displayY - scroll) * LINE_HEIGHT;

            if (cl.kind == LineKind::Text) {
                drawText(cl.text.c_str(), TEXT_MARGIN_X, screenY, 0.5f, TEXT_SCALE, TEXT_SCALE,
                         CLR_TEXT);
            } else {
                visible.insert(cl.imageUrl);
                const CachedImage* c = state.imgCache.get(cl.imageUrl);

                if (c && c->state == ImgState::Ready) {
                    float maxW = TOP_W - TEXT_MARGIN_X * 2.0f;
                    float maxH = (float)IMG_INLINE_H;
                    float scaleX = (c->imgW > 0) ? (maxW / (float)c->imgW) : 1.0f;
                    float scaleY = (c->imgH > 0) ? (maxH / (float)c->imgH) : 1.0f;
                    float s = (scaleX < scaleY) ? scaleX : scaleY;
                    if (s > 1.0f)
                        s = 1.0f; // アップスケールしない
                    C2D_Image img{const_cast<C3D_Tex*>(&c->tex),
                                  const_cast<Tex3DS_SubTexture*>(&c->sub)};
                    C2D_DrawImageAt(img, TEXT_MARGIN_X, screenY, 0.5f, nullptr, s, s);
                } else if (c && c->state == ImgState::Failed) {
                    drawText("[image failed]", TEXT_MARGIN_X, screenY + LINE_HEIGHT, 0.5f,
                             TEXT_SCALE, TEXT_SCALE, CLR_ERROR);
                } else {
                    // プログレスバー（既存コードを流用）
                    constexpr float BAR_H = 10.0f;
                    constexpr float BAR_W = TOP_W - TEXT_MARGIN_X * 2.0f;
                    float barY = screenY + 6.0f;
                    C2D_DrawRectSolid(TEXT_MARGIN_X, barY, 0.5f, BAR_W, BAR_H,
                                      C2D_Color32(0x40, 0x40, 0x60, 0xFF));
                    float pct = state.imgCache.getProgress(cl.imageUrl);
                    if (pct > 0.0f)
                        C2D_DrawRectSolid(TEXT_MARGIN_X, barY, 0.5f, BAR_W * pct, BAR_H, CLR_TITLE);
                    char pctBuf[20];
                    snprintf(pctBuf, sizeof(pctBuf), "Loading... %d%%", (int)(pct * 100.0f));
                    drawText(pctBuf, TEXT_MARGIN_X, barY + BAR_H + 3.0f, 0.5f, TEXT_SCALE,
                             TEXT_SCALE, CLR_HINT);
                }
            }
        }

        displayY += clLines;
        if (displayY >= scroll + TOP_MAX_LINES)
            break;
    }
    state.imgCache.tick(visible);

    // 下画面: 記事タイトル + 操作ガイド
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    std::vector<std::string> titleLines =
        wrapText(art.title, BOT_WRAP_PX, TEXT_SCALE * HEADING_SCALE_FACTOR);
    for (int i = 0; i < (int)titleLines.size() && i < 2; ++i) {
        drawStyledText(titleLines[i].c_str(), TEXT_MARGIN_X, TEXT_MARGIN_Y + (float)i * LINE_HEIGHT,
                       0.5f, TextStyle::Heading);
    }

    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X, BOT_H - 30.0f, 0.5f, TEXT_SCALE,
                 TEXT_SCALE, CLR_HINT);
    } else {
        char scrollInfo[32];
        int displayScroll = scroll < totalDisplayLines ? scroll + 1 : totalDisplayLines;
        snprintf(scrollInfo, sizeof(scrollInfo), "Line %d / %d", displayScroll, totalDisplayLines);
        drawText(scrollInfo, TEXT_MARGIN_X, BOT_H - 30.0f, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }

    bool isBookmarked = state.bookmarkStore.isBookmarked(art.link, art.title);
    const char* bmMark = isBookmarked ? "\xe2\x98\x85" : "\xe2\x98\x86";
    bool hasReadyImg = false;
    for (const auto& cl : lines) {
        if (cl.kind != LineKind::Image)
            continue;
        const CachedImage* ci = state.imgCache.get(cl.imageUrl);
        if (ci && ci->state == ImgState::Ready) {
            hasReadyImg = true;
            break;
        }
    }
    char guide[64];
    if (!art.fullFetched && !art.link.empty())
        snprintf(guide, sizeof(guide), "Up/Down:scroll  A:full  SEL:%s  B:back", bmMark);
    else if (hasReadyImg)
        snprintf(guide, sizeof(guide), "Up/Down:scroll  A:img  SEL:%s  B:back", bmMark);
    else
        snprintf(guide, sizeof(guide), "Up/Down:scroll  SEL:%s  B:back", bmMark);
    drawText(guide, TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f, 0.42f, 0.42f, CLR_HINT);
}

static void drawImageView(const AppState& state) {
    C2D_TargetClear(topTarget, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(topTarget);

    // 高解像度が Ready なら優先表示、未完なら低解像度をフォールバック
    const CachedImage* hi = state.imgViewCache.get(state.imageViewUrl);
    const CachedImage* lo = state.imgCache.get(state.imageViewUrl);
    const CachedImage* c = (hi && hi->state == ImgState::Ready) ? hi : lo;
    bool hiLoading = !hi || hi->state == ImgState::Pending || hi->state == ImgState::None;

    if (c && c->state == ImgState::Ready) {
        float z = state.imageViewZoom;
        float w = (float)c->imgW * z;
        float h = (float)c->imgH * z;
        float x = (TOP_W - w) / 2.0f + state.imageViewOffX;
        float y = (TOP_H - h) / 2.0f + state.imageViewOffY;
        C2D_Image img{const_cast<C3D_Tex*>(&c->tex), const_cast<Tex3DS_SubTexture*>(&c->sub)};
        C2D_DrawImageAt(img, x, y, 0.5f, nullptr, z, z);
    }

    // 高解像度ローディング中はプログレスバーを右下に小さく表示
    if (hiLoading) {
        float pct = state.imgViewCache.getProgress(state.imageViewUrl);
        constexpr float BAR_W = 80.0f;
        constexpr float BAR_H = 6.0f;
        float barX = TOP_W - BAR_W - 4.0f;
        float barY = TOP_H - BAR_H - 4.0f;
        C2D_DrawRectSolid(barX, barY, 0.5f, BAR_W, BAR_H, C2D_Color32(0x40, 0x40, 0x60, 0xC0));
        if (pct > 0.0f)
            C2D_DrawRectSolid(barX, barY, 0.5f, BAR_W * pct, BAR_H, CLR_TITLE);
    }

    std::unordered_set<std::string> vis;
    if (!state.imageViewUrl.empty())
        vis.insert(state.imageViewUrl);
    state.imgViewCache.tick(vis);

    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    char zoomBuf[20];
    snprintf(zoomBuf, sizeof(zoomBuf), "Zoom: x%.1f", state.imageViewZoom);
    drawText(zoomBuf, TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TITLE);
    drawText("L/R:zoom  Stick/Dpad:scroll  B:back", TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f, 0.42f,
             0.42f, CLR_HINT);
}

static void drawBookmarkList(const AppState& state) {
    const auto& bms = state.bookmarkStore.getAll();

    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();
    drawStyledText("\xe2\x98\x85 Bookmarks", TEXT_MARGIN_X, TOP_CONTENT_Y, 0.5f,
                   TextStyle::Heading);

    if (!bms.empty() && state.selectedBookmark < (int)bms.size()) {
        const Bookmark& bm = bms[state.selectedBookmark];
        drawText(bm.feedTitle.c_str(), TEXT_MARGIN_X, 40.0f + STATUSBAR_H, 0.5f, TEXT_SCALE,
                 TEXT_SCALE, CLR_HINT);
        auto wrapped = wrapText(bm.title, TOP_W - (int)(TEXT_MARGIN_X * 2));
        float ty = 55.0f + STATUSBAR_H;
        for (const auto& line : wrapped) {
            drawText(line.c_str(), TEXT_MARGIN_X, ty, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
            ty += LINE_HEIGHT;
        }
    }

    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    int total = (int)bms.size();
    if (total == 0) {
        drawText("No bookmarks yet.", TEXT_MARGIN_X, TEXT_MARGIN_Y, 0.5f, TEXT_SCALE, TEXT_SCALE,
                 CLR_HINT);
    } else {
        int start = state.selectedBookmark - BOT_MAX_LINES / 2;
        if (start < 0)
            start = 0;
        if (start > total - BOT_MAX_LINES && total > BOT_MAX_LINES)
            start = total - BOT_MAX_LINES;

        for (int i = start; i < total && (i - start) < BOT_MAX_LINES; ++i) {
            float y = TEXT_MARGIN_Y + (float)(i - start) * LINE_HEIGHT;
            if (i == state.selectedBookmark)
                C2D_DrawRectSolid(0, y - 1.0f, 0.0f, BOT_W, LINE_HEIGHT + 1.0f, CLR_SEL_BG);
            auto lw = wrapText(bms[i].title, BOT_WRAP_PX);
            std::string label = lw.empty() ? "" : lw.front();
            drawText(label.c_str(), TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
        }
    }

    if (state.bookmarkConfirmRemove) {
        constexpr float DLG_X = 30.0f, DLG_Y = 80.0f, DLG_W = BOT_W - 60.0f, DLG_H = 54.0f;
        C2D_DrawRectSolid(DLG_X, DLG_Y, 0.1f, DLG_W, DLG_H, CLR_SEL_BG);
        drawText("Remove bookmark?", DLG_X + 8.0f, DLG_Y + 8.0f, 0.1f, TEXT_SCALE, TEXT_SCALE,
                 CLR_TEXT);
        drawText("A:Yes  B:Cancel", DLG_X + 8.0f, DLG_Y + 28.0f, 0.1f, TEXT_SCALE, TEXT_SCALE,
                 CLR_HINT);
    } else {
        drawText("A:open  SEL:remove  B:back", TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f, 0.42f, 0.42f,
                 CLR_HINT);
    }
}

static void drawSettings(const AppState& state) {
    // 上画面: タイトル
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();
    drawStyledText("Settings", TEXT_MARGIN_X, TOP_CONTENT_Y, 0.5f, TextStyle::Heading);

    // 下画面: 設定項目
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    struct Item {
        const char* label;
        int value;
        bool isAction;
    };
    const Item items[3] = {
        {"Scroll Delay", state.settings.scrollRepeatDelayMs, false},
        {"Scroll Interval", state.settings.scrollRepeatIntervalMs, false},
        {"Save", 0, true},
    };

    char buf[64];
    for (int i = 0; i < 3; ++i) {
        float y = TEXT_MARGIN_Y + (float)i * LINE_HEIGHT;
        if (i == state.settingsSelectedItem) {
            C2D_DrawRectSolid(0, y - 1.0f, 0.0f, BOT_W, LINE_HEIGHT + 1.0f, CLR_SEL_BG);
        }
        if (items[i].isAction)
            snprintf(buf, sizeof(buf), "%s", items[i].label);
        else
            snprintf(buf, sizeof(buf), "%-18s < %d ms >", items[i].label, items[i].value);
        drawText(buf, TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    drawText("Up/Down:select  L/R:change  A:save  B:back", TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f,
             0.42f, 0.42f, CLR_HINT);
}

// --- パブリック関数 ---

void uiDraw(const AppState& state) {
    C2D_TextBufClear(textBuf);

    switch (state.currentScreen) {
    case Screen::FeedList:
        drawFeedList(state);
        break;
    case Screen::Loading:
        drawLoadingScreen(state);
        break;
    case Screen::LoadingAll:
        drawLoadingScreen(state);
        break;
    case Screen::LoadingArticle:
        drawLoadingScreen(state);
        break;
    case Screen::ArticleList:
        drawArticleList(state);
        break;
    case Screen::ArticleView:
        drawArticleView(state);
        break;
    case Screen::ImageView:
        drawImageView(state);
        break;
    case Screen::Settings:
        drawSettings(state);
        break;
    case Screen::BookmarkList:
        drawBookmarkList(state);
        break;
    }
}

void uiHandleInput(AppState& state, u32 kDown, u32 kHeld, u32 kRepeat) {
    // D-pad は kRepeat を使用（libctru 組み込みの長押しリピート）。
    // A/B/START などの確定ボタンは引き続き kDown を使用する。

    switch (state.currentScreen) {
    case Screen::FeedList: {
        int feedCount = (int)state.feedConfigs.size();
        int total = feedCount + 2; // +1 for Bookmarks, +1 for Settings
        if ((kRepeat & KEY_DOWN) && state.selectedFeed < total - 1)
            ++state.selectedFeed;
        if ((kRepeat & KEY_UP) && state.selectedFeed > 0)
            --state.selectedFeed;
        if ((kDown & KEY_A) && total > 0) {
            if (state.selectedFeed == feedCount + 1) {
                // Settings エントリ
                state.currentScreen = Screen::Settings;
                state.settingsSelectedItem = 0;
            } else if (state.selectedFeed == feedCount) {
                // Bookmarks エントリ
                state.selectedBookmark = 0;
                state.currentScreen = Screen::BookmarkList;
            } else {
                // 実フィードを開く
                const FeedConfig& cfg = state.feedConfigs[state.selectedFeed];
                state.statusMsg = cfg.name.empty() ? cfg.url : cfg.name;
                state.selectedArticle = 0;
                state.feedJobSubmitted = false;
                state.currentScreen = Screen::Loading;
            }
            kDown &= ~KEY_A; // coding-patterns #6
        }
        if (kDown & KEY_Y) {
            for (size_t i = 0; i < state.feedLoaded.size(); ++i)
                state.feedLoaded[i] = false;
            state.refreshIdx = 0;
            state.statusMsg = "Refreshing...";
            state.currentScreen = Screen::LoadingAll;
            // cppcheck-suppress unreadVariable
            kDown &= ~KEY_Y;
        }
        break;
    }
    case Screen::Loading:
    case Screen::LoadingAll:
    case Screen::LoadingArticle:
        // main.cpp のループで処理するため入力は無視
        break;
    case Screen::ArticleList: {
        int idx = state.selectedFeed;
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
                const std::string& title = state.feeds[idx].articles[state.selectedArticle].title;
                float textW = measureStr(title.c_str(), TEXT_SCALE);
                float displayW = (float)BOT_WRAP_PX;
                int maxScroll = (textW > displayW) ? (int)(textW - displayW + 0.5f) : 0;
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
            kDown &= ~KEY_A; // coding-patterns #6
            Article& art = state.feeds[idx].articles[state.selectedArticle];
            state.readHistory.markRead(ReadHistory::keyFor(art.link, art.title));
            if ((int)art.content.size() < CONTENT_SHORT_THRESHOLD && !art.link.empty()) {
                state.pendingFetchFeed = idx;
                state.pendingFetchArticle = state.selectedArticle;
                state.pendingFetchFullArticle = false;
                state.pendingReturnScreen = Screen::ArticleList;
                state.statusMsg = "Downloading article...";
                state.articleLoader.submit(art.link);
                state.currentScreen = Screen::LoadingArticle;
            } else {
                state.currentScreen = Screen::ArticleView;
                state.scrollY = 0;
            }
        }
        if ((kDown & KEY_SELECT) && total > 0) {
            const Article& art = state.feeds[idx].articles[state.selectedArticle];
            const Feed& feed = state.feeds[idx];
            const std::string& feedTitle =
                feed.title.empty() ? state.feedConfigs[idx].name : feed.title;
            state.bookmarkStore.toggle(art.title, art.link, feedTitle);
            bool nowBm = state.bookmarkStore.isBookmarked(art.link, art.title);
            state.statusMsg = nowBm ? "Bookmarked!" : "Bookmark removed.";
            kDown &= ~KEY_SELECT;
        }
        if (kDown & KEY_Y) {
            state.feedLoaded[idx] = false;
            const FeedConfig& cfg = state.feedConfigs[idx];
            state.statusMsg = cfg.name.empty() ? cfg.url : cfg.name;
            state.feedJobSubmitted = false;
            state.currentScreen = Screen::Loading;
            kDown &= ~KEY_Y;
        }
        if (kDown & KEY_B) {
            state.currentScreen = Screen::FeedList;
            state.articleListScrollX = 0;
            state.statusMsg = "";
            // cppcheck-suppress unreadVariable
            kDown &= ~KEY_B;
        }
        break;
    }
    case Screen::Settings: {
        if ((kRepeat & KEY_UP) && state.settingsSelectedItem > 0)
            --state.settingsSelectedItem;
        if ((kRepeat & KEY_DOWN) && state.settingsSelectedItem < 2)
            ++state.settingsSelectedItem;

        // 値変更（Save 行では無効）
        if (state.settingsSelectedItem == 0) {
            int idx =
                findIndex(SCROLL_DELAY_OPTIONS, NUM_DELAY_OPTS, state.settings.scrollRepeatDelayMs);
            if ((kRepeat & KEY_RIGHT) && idx < NUM_DELAY_OPTS - 1)
                state.settings.scrollRepeatDelayMs = SCROLL_DELAY_OPTIONS[idx + 1];
            if ((kRepeat & KEY_LEFT) && idx > 0)
                state.settings.scrollRepeatDelayMs = SCROLL_DELAY_OPTIONS[idx - 1];
        } else if (state.settingsSelectedItem == 1) {
            int idx = findIndex(SCROLL_INTERVAL_OPTIONS, NUM_INTERVAL_OPTS,
                                state.settings.scrollRepeatIntervalMs);
            if ((kRepeat & KEY_RIGHT) && idx < NUM_INTERVAL_OPTS - 1)
                state.settings.scrollRepeatIntervalMs = SCROLL_INTERVAL_OPTIONS[idx + 1];
            if ((kRepeat & KEY_LEFT) && idx > 0)
                state.settings.scrollRepeatIntervalMs = SCROLL_INTERVAL_OPTIONS[idx - 1];
        }

        // A (Save 行) → 保存して戻る
        if ((kDown & KEY_A) && state.settingsSelectedItem == 2) {
            settingsSave(state.settings);
            state.currentScreen = Screen::FeedList;
            kDown &= ~KEY_A;
        }
        // B → 保存せずに戻る
        if (kDown & KEY_B) {
            state.currentScreen = Screen::FeedList;
            // cppcheck-suppress unreadVariable
            kDown &= ~KEY_B;
        }
        break;
    }
    case Screen::ArticleView: {
        if ((kRepeat & KEY_DOWN) && state.scrollY < state.cachedMaxScroll)
            ++state.scrollY;
        if ((kRepeat & KEY_UP) && state.scrollY > 0)
            --state.scrollY;
        if ((kDown & KEY_A)) {
            Article& art = state.viewingBookmark
                               ? state.bookmarkTempArticle
                               : state.feeds[state.selectedFeed].articles[state.selectedArticle];
            if (!art.fullFetched && !art.link.empty()) {
                state.pendingFetchFeed = state.viewingBookmark ? -2 : state.selectedFeed;
                state.pendingFetchArticle = state.viewingBookmark ? -2 : state.selectedArticle;
                state.pendingFetchFullArticle = true;
                state.pendingReturnScreen = Screen::ArticleView;
                state.statusMsg = "Downloading full article...";
                state.articleLoader.submit(art.link);
                state.currentScreen = Screen::LoadingArticle;
                kDown &= ~KEY_A; // coding-patterns #6
            } else {
                // 可視エリアの Image ContentLine から画面中央に最も近い Ready 画像を選ぶ
                std::string bestUrl;
                float bestDist = 1e9f;
                int dispY = 0;
                int sc =
                    state.scrollY < state.cachedMaxScroll ? state.scrollY : state.cachedMaxScroll;
                for (const auto& cl : state.articleLines) {
                    int clLines = (cl.kind == LineKind::Image) ? IMG_INLINE_LINES : 1;
                    if (cl.kind == LineKind::Image && dispY + clLines > sc &&
                        dispY < sc + TOP_MAX_LINES) {
                        const CachedImage* ci = state.imgCache.get(cl.imageUrl);
                        if (ci && ci->state == ImgState::Ready) {
                            float screenY = TOP_CONTENT_Y + (float)(dispY - sc) * LINE_HEIGHT;
                            float cy = screenY + (float)IMG_INLINE_H / 2.0f;
                            float d = cy - (float)TOP_H / 2.0f;
                            if (d < 0.0f)
                                d = -d;
                            if (d < bestDist) {
                                bestDist = d;
                                bestUrl = cl.imageUrl;
                            }
                        }
                    }
                    dispY += clLines;
                    if (dispY >= sc + TOP_MAX_LINES)
                        break;
                }
                if (!bestUrl.empty()) {
                    state.imageViewUrl = bestUrl;
                    state.imageViewZoom = 1.0f;
                    state.imageViewOffX = 0.0f;
                    state.imageViewOffY = 0.0f;
                    state.imgViewLoader.start();
                    state.imgViewCache.attach(&state.imgViewLoader);
                    state.imgViewCache.resetForArticle({state.imageViewUrl});
                    state.currentScreen = Screen::ImageView;
                }
            }
            kDown &= ~KEY_A;
        }
        if (kDown & KEY_SELECT) {
            std::string artTitle, artLink, feedTitle;
            if (state.viewingBookmark) {
                artTitle = state.bookmarkTempArticle.title;
                artLink = state.bookmarkTempArticle.link;
                feedTitle = state.bookmarkTempFeedTitle;
            } else {
                const Article& art =
                    state.feeds[state.selectedFeed].articles[state.selectedArticle];
                const Feed& feed = state.feeds[state.selectedFeed];
                artTitle = art.title;
                artLink = art.link;
                feedTitle =
                    feed.title.empty() ? state.feedConfigs[state.selectedFeed].name : feed.title;
            }
            state.bookmarkStore.toggle(artTitle, artLink, feedTitle);
            bool nowBm = state.bookmarkStore.isBookmarked(artLink, artTitle);
            if (state.viewingBookmark && !nowBm) {
                int bmTotal = (int)state.bookmarkStore.getAll().size();
                if (state.selectedBookmark >= bmTotal)
                    state.selectedBookmark = bmTotal > 0 ? bmTotal - 1 : 0;
            }
            state.statusMsg = nowBm ? "Bookmarked!" : "Bookmark removed.";
            kDown &= ~KEY_SELECT;
        }
        if (kDown & KEY_B) {
            state.currentScreen =
                state.viewingBookmark ? Screen::BookmarkList : Screen::ArticleList;
            state.viewingBookmark = false;
            state.statusMsg = "";
            // cppcheck-suppress unreadVariable
            kDown &= ~KEY_B;
        }
        break;
    }
    case Screen::BookmarkList: {
        int total = (int)state.bookmarkStore.getAll().size();
        if (state.bookmarkConfirmRemove) {
            if (kDown & KEY_A) {
                if (total > 0 && state.selectedBookmark < total) {
                    const Bookmark bm = state.bookmarkStore.getAll()[state.selectedBookmark];
                    state.bookmarkStore.toggle(bm.title, bm.link, bm.feedTitle);
                    int newTotal = (int)state.bookmarkStore.getAll().size();
                    if (state.selectedBookmark >= newTotal && newTotal > 0)
                        state.selectedBookmark = newTotal - 1;
                }
                state.bookmarkConfirmRemove = false;
                kDown &= ~KEY_A;
            }
            if (kDown & KEY_B) {
                state.bookmarkConfirmRemove = false;
                // cppcheck-suppress unreadVariable
                kDown &= ~KEY_B;
            }
            break;
        }
        if ((kRepeat & KEY_DOWN) && state.selectedBookmark < total - 1)
            ++state.selectedBookmark;
        if ((kRepeat & KEY_UP) && state.selectedBookmark > 0)
            --state.selectedBookmark;
        if ((kDown & KEY_A) && total > 0) {
            const Bookmark& bm = state.bookmarkStore.getAll()[state.selectedBookmark];
            state.bookmarkTempArticle = Article{};
            state.bookmarkTempArticle.title = bm.title;
            state.bookmarkTempArticle.link = bm.link;
            state.bookmarkTempFeedTitle = bm.feedTitle;
            state.pendingFetchFeed = -2;
            state.pendingFetchArticle = -2;
            state.pendingFetchFullArticle = true;
            state.pendingReturnScreen = Screen::BookmarkList;
            state.cachedLineContentSize = 0;
            state.cachedImagesArticle = -1;
            state.scrollY = 0;
            state.statusMsg = "Loading article...";
            state.articleLoader.submit(bm.link);
            state.currentScreen = Screen::LoadingArticle;
            kDown &= ~KEY_A;
        }
        if ((kDown & KEY_SELECT) && total > 0) {
            state.bookmarkConfirmRemove = true;
            kDown &= ~KEY_SELECT;
        }
        if (kDown & KEY_B) {
            state.currentScreen = Screen::FeedList;
            // cppcheck-suppress unreadVariable
            kDown &= ~KEY_B;
        }
        break;
    }
    case Screen::ImageView: {
        constexpr float ZOOM_STEP = 0.05f;
        constexpr float ZOOM_MIN = 0.5f;
        constexpr float ZOOM_MAX = 4.0f;
        // L/R でズーム (old 3DS 含む全機種対応)
        if (kHeld & KEY_R) {
            state.imageViewZoom += ZOOM_STEP;
            if (state.imageViewZoom > ZOOM_MAX)
                state.imageViewZoom = ZOOM_MAX;
        }
        if (kHeld & KEY_L) {
            state.imageViewZoom -= ZOOM_STEP;
            if (state.imageViewZoom < ZOOM_MIN)
                state.imageViewZoom = ZOOM_MIN;
        }

        // スティック + Dpad でパン
        constexpr float PAN_SPEED = 12.0f;
        constexpr float DPAD_SPEED = 10.0f;
        constexpr int DEAD_ZONE = 20;
        circlePosition pos;
        hidCircleRead(&pos);
        if (pos.dx > DEAD_ZONE || pos.dx < -DEAD_ZONE)
            state.imageViewOffX -= (float)pos.dx / 154.0f * PAN_SPEED;
        if (pos.dy > DEAD_ZONE || pos.dy < -DEAD_ZONE)
            state.imageViewOffY += (float)pos.dy / 154.0f * PAN_SPEED;
        if (kHeld & KEY_RIGHT)
            state.imageViewOffX -= DPAD_SPEED;
        if (kHeld & KEY_LEFT)
            state.imageViewOffX += DPAD_SPEED;
        if (kHeld & KEY_UP)
            state.imageViewOffY += DPAD_SPEED;
        if (kHeld & KEY_DOWN)
            state.imageViewOffY -= DPAD_SPEED;

        // クランプ: 高解像度版の寸法を優先して使用
        {
            const CachedImage* hi = state.imgViewCache.get(state.imageViewUrl);
            const CachedImage* lo = state.imgCache.get(state.imageViewUrl);
            const CachedImage* c = (hi && hi->state == ImgState::Ready) ? hi : lo;
            if (c && c->state == ImgState::Ready) {
                float hw = (float)c->imgW * state.imageViewZoom / 2.0f;
                float hh = (float)c->imgH * state.imageViewZoom / 2.0f;
                float maxX = hw > TOP_W / 2.0f ? hw - TOP_W / 2.0f : 0.0f;
                float maxY = hh > TOP_H / 2.0f ? hh - TOP_H / 2.0f : 0.0f;
                if (state.imageViewOffX > maxX)
                    state.imageViewOffX = maxX;
                if (state.imageViewOffX < -maxX)
                    state.imageViewOffX = -maxX;
                if (state.imageViewOffY > maxY)
                    state.imageViewOffY = maxY;
                if (state.imageViewOffY < -maxY)
                    state.imageViewOffY = -maxY;
            }
        }

        if (kDown & KEY_B) {
            state.currentScreen = Screen::ArticleView;
            // cppcheck-suppress unreadVariable
            // cppcheck-suppress uselessAssignmentArg
            kDown &= ~KEY_B;
        }
        break;
    }
    }
}
