#include "ui.h"
#include "html_strip.h"
#include "article.h"
#include "settings.h"
#include <citro2d.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unordered_set>

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

// ステータスバー
static constexpr float STATUSBAR_H    = 14.0f;
static constexpr float TOP_CONTENT_Y  = STATUSBAR_H + TEXT_MARGIN_Y;  // 20.0f


// テキスト折り返し用ピクセル幅
// wrapText の高速推定用。citro2d による実幅検証で最終的に正確にトリムされる
static constexpr int   TOP_WRAP_PX         = 388;
static constexpr int   BOT_WRAP_PX         = 308;
static constexpr float HEADING_SCALE_FACTOR = 1.3f;

// 記事一覧タイトルの水平スクロール刻み幅(px)
// 将来: AppConfig::title_scroll_step に置き換え可能なよう1箇所に集約
static constexpr int   TITLE_SCROLL_STEP_PX = 50;

static constexpr int TOP_MAX_LINES =
    (int)((TOP_H - STATUSBAR_H - TEXT_MARGIN_Y * 2) / LINE_HEIGHT);
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

static C3D_RenderTarget* topTarget    = nullptr;
static C3D_RenderTarget* botTarget    = nullptr;
static C2D_TextBuf       textBuf      = nullptr;
static C2D_TextBuf       measureBuf   = nullptr;  // 幅測定専用バッファ
static C2D_Font          fallbackFont = nullptr;

// ステータスバー用キャッシュ（5秒ごとに更新）
static u8  s_battLevel    = 3;
static u8  s_battCharging = 0;
static u8  s_wifiStatus   = 0;  // 0-3: osGetWifiStrength()
static u64 s_statusLastMs = 0;


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
    topTarget    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    botTarget    = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf      = C2D_TextBufNew(4096);
    measureBuf   = C2D_TextBufNew(512);
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
    if (c < 0x80)            { outBytes = 1; return c; }
    if ((c & 0xE0) == 0xC0)  { outBytes = 2; return ((c & 0x1F) << 6)  | ((unsigned char)s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0)  { outBytes = 3; return ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0)  { outBytes = 4; return ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) | (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F); }
    outBytes = 1; return 0xFFFD;
}

// システムフォントにグリフがあるか（alterCharIndex と一致しなければあり）
static bool systemHasGlyph(uint32_t cp) {
    CFNT_s* sys   = fontGetSystemFont();
    int     alter = sys->finf.alterCharIndex;
    return fontGlyphIndexFromCodePoint(sys, cp) != alter;
}

static void drawText(const char* str, float x, float y, float z,
                     float sx, float sy, u32 color) {
    if (!fallbackFont) {
        C2D_Text text;
        C2D_TextParse(&text, textBuf, str);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, x, y, z, sx, sy, color);
        return;
    }
    float  cursorX = x;
    size_t n = strlen(str), i = 0;
    while (i < n) {
        int      bytes;
        uint32_t cp     = utf8Decode(str + i, bytes);
        bool     useSys = systemHasGlyph(cp);
        size_t   j      = i + bytes;
        while (j < n) {
            int nb; uint32_t ncp = utf8Decode(str + j, nb);
            if (systemHasGlyph(ncp) != useSys) break;
            j += nb;
        }
        std::string chunk(str + i, j - i);
        C2D_Text text;
        if (useSys) C2D_TextParse    (&text, textBuf, chunk.c_str());
        else        C2D_TextFontParse(&text, fallbackFont, textBuf, chunk.c_str());
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, cursorX, y, z, sx, sy, color);
        cursorX += text.width * sx;
        i = j;
    }
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
    if (!fallbackFont) {
        C2D_Text t;
        C2D_TextParse(&t, measureBuf, str);
        return t.width * scale;
    }
    float  total = 0.0f;
    size_t n = strlen(str), i = 0;
    while (i < n) {
        int      bytes;
        uint32_t cp     = utf8Decode(str + i, bytes);
        bool     useSys = systemHasGlyph(cp);
        size_t   j      = i + bytes;
        while (j < n) {
            int nb; uint32_t ncp = utf8Decode(str + j, nb);
            if (systemHasGlyph(ncp) != useSys) break;
            j += nb;
        }
        std::string chunk(str + i, j - i);
        C2D_Text t;
        if (useSys) C2D_TextParse    (&t, measureBuf, chunk.c_str());
        else        C2D_TextFontParse(&t, fallbackFont, measureBuf, chunk.c_str());
        total += t.width;
        i = j;
    }
    return total * scale;
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
        bool softWrap = (split != hard);
        if (!softWrap) {
            line = src.substr(pos, hard - pos);
            pos  = hard + (nl != std::string::npos ? 1 : 0);
        } else {
            line = src.substr(pos, split - pos);
            pos  = split;
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

// --- 設定画面用定数 ---

static const int SCROLL_DELAY_OPTIONS[]    = { 200, 300, 400, 500 };
static const int SCROLL_INTERVAL_OPTIONS[] = { 50, 80, 120, 160 };
static const int NUM_DELAY_OPTS            = 4;
static const int NUM_INTERVAL_OPTS         = 4;

// arr[0..n-1] 内で val のインデックスを返す。見つからなければ 0。
static int findIndex(const int* arr, int n, int val) {
    for (int i = 0; i < n; ++i) if (arr[i] == val) return i;
    return 0;
}

// --- ステータスバー ---

static void drawStatusBar() {
    // バッテリー・WiFi: 5秒ごとに更新
    u64 now = osGetTime();
    if (now - s_statusLastMs > 5000) {
        PTMU_GetBatteryLevel(&s_battLevel);
        PTMU_GetBatteryChargeState(&s_battCharging);
        s_wifiStatus   = osGetWifiStrength();
        s_statusLastMs = now;
    }

    // 現在時刻
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char timeBuf[8];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d",
             tm_info->tm_hour, tm_info->tm_min);

    // WiFi バー (■=U+25A0, □=U+25A1 — ホワイトリスト済み)
    static const char* WIFI_BARS[4] = {
        "\xe2\x96\xa1\xe2\x96\xa1\xe2\x96\xa1",  // □□□
        "\xe2\x96\xa0\xe2\x96\xa1\xe2\x96\xa1",  // ■□□
        "\xe2\x96\xa0\xe2\x96\xa0\xe2\x96\xa1",  // ■■□
        "\xe2\x96\xa0\xe2\x96\xa0\xe2\x96\xa0",  // ■■■
    };
    u8 wifiIdx = s_wifiStatus < 4 ? s_wifiStatus : 3;

    // バッテリー文字列（0-5段階 → 20%単位）
    char battBuf[12];
    int pct = (int)s_battLevel * 20;
    if (s_battCharging)
        snprintf(battBuf, sizeof(battBuf), "%d%%+", pct);
    else
        snprintf(battBuf, sizeof(battBuf), "%d%%",  pct);

    // 右端表示: WiFi バー + スペース + バッテリー
    char rightBuf[32];
    snprintf(rightBuf, sizeof(rightBuf), "%s %s", WIFI_BARS[wifiIdx], battBuf);

    // 背景帯
    C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, STATUSBAR_H, CLR_PANEL);

    // 時刻（左端）
    drawText(timeBuf, TEXT_MARGIN_X, 1.0f, 0.5f, 0.42f, 0.42f, CLR_HINT);

    // WiFi + バッテリー（右端）
    float rightW = measureStr(rightBuf, 0.42f);
    drawText(rightBuf, TOP_W - rightW - TEXT_MARGIN_X, 1.0f, 0.5f,
             0.42f, 0.42f, CLR_HINT);
}

// --- 各画面の描画 ---

static void drawLoadingScreen(const AppState& state) {
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();
    drawText("Loading...", TEXT_MARGIN_X, TOP_H / 2.0f - 14.0f + STATUSBAR_H / 2.0f,
             0.5f, TEXT_SCALE * 1.2f, TEXT_SCALE * 1.2f, CLR_TITLE);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X,
                 TOP_H / 2.0f + 4.0f + STATUSBAR_H / 2.0f, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    }
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);
}

static void drawFeedList(const AppState& state) {
    // 上画面: アプリタイトル
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();

    drawStyledText("3DS RSS Reader", TEXT_MARGIN_X, TOP_CONTENT_Y, 0.5f,
                   TextStyle::Heading);
    drawText("Select a feed on the bottom screen.", TEXT_MARGIN_X,
             40.0f + STATUSBAR_H, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    if (!state.statusMsg.empty()) {
        drawText(state.statusMsg.c_str(), TEXT_MARGIN_X, 70.0f + STATUSBAR_H, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_ERROR);
    }

    // 下画面: フィード一覧 + Settings エントリ（feedConfigs から名前を取得）
    C2D_TargetClear(botTarget, CLR_PANEL);
    C2D_SceneBegin(botTarget);

    int feedCount = (int)state.feedConfigs.size();
    int total     = feedCount + 1;  // +1 for Settings entry
    int start = state.selectedFeed - BOT_MAX_LINES / 2;
    if (start < 0) start = 0;
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
        else
            snprintf(label, sizeof(label), "[Settings]");
        drawText(label, TEXT_MARGIN_X, y, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    if (feedCount == 0) {
        drawText("No feeds. Add feeds.json to SD card.", TEXT_MARGIN_X,
                 TEXT_MARGIN_Y + LINE_HEIGHT, 0.5f, TEXT_SCALE, TEXT_SCALE, CLR_HINT);
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
    const std::string& title = feed.title.empty()
        ? state.feedConfigs[idx].name
        : feed.title;
    drawStyledText(title.c_str(), TEXT_MARGIN_X, TOP_CONTENT_Y, 0.5f,
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

    drawText("Up/Down:move  A:read  Y:refresh  B:back", TEXT_MARGIN_X, BOT_H - 16.0f,
             0.5f, 0.42f, 0.42f, CLR_HINT);
}

static void drawArticleView(const AppState& state) {
    const Article& art =
        state.feeds[state.selectedFeed].articles[state.selectedArticle];

    // キャッシュが無効なら再計算（フィード・記事・本文サイズのいずれかが変わった場合）
    if (state.cachedLineFeed    != state.selectedFeed
     || state.cachedLineArticle != state.selectedArticle
     || state.cachedLineContentSize != art.content.size()) {
        std::string plain = stripHtml(art.content);
        state.articleLines          = wrapText(plain, TOP_WRAP_PX);
        state.cachedLineFeed        = state.selectedFeed;
        state.cachedLineArticle     = state.selectedArticle;
        state.cachedLineContentSize = art.content.size();

        // 画像キャッシュも同タイミングで再構築 (Stage 2)
        state.imgLoader.start();
        state.imgCache.attach(&state.imgLoader);
        state.imgCache.resetForArticle(art.imageUrls);
        state.cachedImagesFeed    = state.selectedFeed;
        state.cachedImagesArticle = state.selectedArticle;
    }
    const std::vector<std::string>& lines = state.articleLines;

    // 上画面: 本文
    C2D_TargetClear(topTarget, CLR_BG);
    C2D_SceneBegin(topTarget);
    drawStatusBar();

    constexpr int IMG_GAP_PX  = 6;
    constexpr int IMG_BLOCK_H = 256;  // 画像表示枠 (実画像が小さくても予約)

    int totalLines = (int)lines.size();
    int imgCount   = (int)art.imageUrls.size();
    // 最後の画像にはギャップ不要なので除く
    int imgPixels  = imgCount > 0
        ? imgCount * IMG_BLOCK_H + (imgCount - 1) * IMG_GAP_PX
        : 0;
    int extraLines = imgCount > 0
        ? (imgPixels + (int)LINE_HEIGHT - 1) / (int)LINE_HEIGHT
        : 0;
    int totalDisplayLines = totalLines + extraLines;
    int maxScroll  = totalDisplayLines > TOP_MAX_LINES
        ? totalDisplayLines - TOP_MAX_LINES : 0;
    state.cachedMaxScroll = maxScroll;
    int scroll     = state.scrollY < maxScroll ? state.scrollY : maxScroll;

    for (int i = 0; i < TOP_MAX_LINES && (scroll + i) < totalLines; ++i) {
        float y = TOP_CONTENT_Y + (float)i * LINE_HEIGHT;
        drawText(lines[scroll + i].c_str(), TEXT_MARGIN_X, y, 0.5f,
                 TEXT_SCALE, TEXT_SCALE, CLR_TEXT);
    }

    // 画像セクション (本文末尾から縦に配置)
    std::unordered_set<std::string> visible;
    int textBottomYPx = (int)TOP_CONTENT_Y
        + (int)((float)(totalLines - scroll) * LINE_HEIGHT);
    for (int i = 0; i < imgCount; ++i) {
        int yPx = textBottomYPx + i * (IMG_BLOCK_H + IMG_GAP_PX);
        // 可視判定: 画面範囲 ± IMG_BLOCK_H (隣接 1 枚先読み)
        if (yPx + IMG_BLOCK_H > -IMG_BLOCK_H && yPx < TOP_H + IMG_BLOCK_H) {
            visible.insert(art.imageUrls[i]);
        }
        // 描画は画面内のみ
        if (yPx + IMG_BLOCK_H < (int)TOP_CONTENT_Y || yPx > TOP_H) continue;
        const CachedImage* c = state.imgCache.get(art.imageUrls[i]);
        if (c && c->state == ImgState::Ready) {
            C2D_Image img{ const_cast<C3D_Tex*>(&c->tex),
                           const_cast<Tex3DS_SubTexture*>(&c->sub) };
            C2D_DrawImageAt(img, TEXT_MARGIN_X, (float)yPx, 0.5f,
                            nullptr, 1.0f, 1.0f);
        } else if (c && c->state == ImgState::Failed) {
            drawText("[image failed]", TEXT_MARGIN_X, (float)yPx, 0.5f,
                     TEXT_SCALE, TEXT_SCALE, CLR_ERROR);
        } else {
            // プログレスバー
            constexpr float BAR_H = 10.0f;
            constexpr float BAR_W = TOP_W - TEXT_MARGIN_X * 2.0f;
            float barY = (float)yPx + 6.0f;
            C2D_DrawRectSolid(TEXT_MARGIN_X, barY, 0.5f, BAR_W, BAR_H,
                               C2D_Color32(0x40, 0x40, 0x60, 0xFF));
            float pct = state.imgCache.getProgress(art.imageUrls[i]);
            if (pct > 0.0f)
                C2D_DrawRectSolid(TEXT_MARGIN_X, barY, 0.5f, BAR_W * pct, BAR_H,
                                   CLR_TITLE);
            char pctBuf[20];
            snprintf(pctBuf, sizeof(pctBuf), "Loading... %d%%", (int)(pct * 100.0f));
            drawText(pctBuf, TEXT_MARGIN_X, barY + BAR_H + 3.0f, 0.5f,
                     TEXT_SCALE, TEXT_SCALE, CLR_HINT);
        }
    }
    state.imgCache.tick(visible);

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
    int displayScroll = scroll < totalDisplayLines ? scroll + 1 : totalDisplayLines;
    snprintf(scrollInfo, sizeof(scrollInfo), "Line %d / %d",
             displayScroll, totalDisplayLines);
    drawText(scrollInfo, TEXT_MARGIN_X, BOT_H - 30.0f, 0.5f,
             TEXT_SCALE, TEXT_SCALE, CLR_HINT);
    const char* guide = (!art.fullFetched && !art.link.empty())
        ? "Up/Down:scroll  A:full article  B:back"
        : "Up/Down:scroll  B:back";
    drawText(guide, TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f, 0.42f, 0.42f, CLR_HINT);
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

    struct Item { const char* label; int value; bool isAction; };
    const Item items[3] = {
        { "Scroll Delay",    state.settings.scrollRepeatDelayMs,    false },
        { "Scroll Interval", state.settings.scrollRepeatIntervalMs, false },
        { "Save",            0,                                      true  },
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

    drawText("Up/Down:select  L/R:change  A:save  B:back",
             TEXT_MARGIN_X, BOT_H - 16.0f, 0.5f, 0.42f, 0.42f, CLR_HINT);
}

// --- フェッチヘルパー ---

// art.link からHTMLを取得してart.contentを更新する。成功時trueを返す。
static bool doFetchArticle(Article& art, AppState& state, const char* loadingMsg) {
    state.statusMsg = loadingMsg;
    std::string errMsg;
    FetchedArticle fetched = fetchArticleBody2(art.link, errMsg);
    if (!fetched.body.empty()) {
        art.content     = std::move(fetched.body);
        art.imageUrls   = std::move(fetched.imageUrls);
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
        case Screen::FeedList:    drawFeedList(state);      break;
        case Screen::Loading:     drawLoadingScreen(state); break;
        case Screen::LoadingAll:  drawLoadingScreen(state); break;
        case Screen::ArticleList: drawArticleList(state);   break;
        case Screen::ArticleView: drawArticleView(state);   break;
        case Screen::Settings:    drawSettings(state);      break;
    }
}

void uiHandleInput(AppState& state, u32 kDown, u32 kHeld, u32 kRepeat) {
    (void)kHeld;
    // D-pad は kRepeat を使用（libctru 組み込みの長押しリピート）。
    // A/B/START などの確定ボタンは引き続き kDown を使用する。

    switch (state.currentScreen) {
        case Screen::FeedList: {
            int feedCount = (int)state.feedConfigs.size();
            int total     = feedCount + 1;  // +1 for Settings entry
            if ((kRepeat & KEY_DOWN) && state.selectedFeed < total - 1)
                ++state.selectedFeed;
            if ((kRepeat & KEY_UP) && state.selectedFeed > 0)
                --state.selectedFeed;
            if ((kDown & KEY_A) && total > 0) {
                if (state.selectedFeed == feedCount) {
                    // Settings エントリ
                    state.currentScreen       = Screen::Settings;
                    state.settingsSelectedItem = 0;
                } else {
                    // 実フィードを開く
                    const FeedConfig& cfg = state.feedConfigs[state.selectedFeed];
                    state.statusMsg       = cfg.name.empty() ? cfg.url : cfg.name;
                    state.currentScreen   = Screen::Loading;
                    state.selectedArticle = 0;
                }
                kDown &= ~KEY_A;  // coding-patterns #6
            }
            if (kDown & KEY_Y) {
                for (size_t i = 0; i < state.feedLoaded.size(); ++i)
                    state.feedLoaded[i] = false;
                state.refreshIdx    = 0;
                state.statusMsg     = "Refreshing...";
                state.currentScreen = Screen::LoadingAll;
                kDown &= ~KEY_Y;
            }
            break;
        }
        case Screen::Loading:
        case Screen::LoadingAll:
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
            if (kDown & KEY_Y) {
                state.feedLoaded[idx] = false;
                const FeedConfig& cfg = state.feedConfigs[idx];
                state.statusMsg = cfg.name.empty() ? cfg.url : cfg.name;
                state.currentScreen = Screen::Loading;
                kDown &= ~KEY_Y;
            }
            if (kDown & KEY_B) {
                state.currentScreen      = Screen::FeedList;
                state.articleListScrollX = 0;
                kDown &= ~KEY_B;
            }
            break;
        }
        case Screen::Settings: {
            if ((kRepeat & KEY_UP)   && state.settingsSelectedItem > 0)
                --state.settingsSelectedItem;
            if ((kRepeat & KEY_DOWN) && state.settingsSelectedItem < 2)
                ++state.settingsSelectedItem;

            // 値変更（Save 行では無効）
            if (state.settingsSelectedItem == 0) {
                int idx = findIndex(SCROLL_DELAY_OPTIONS, NUM_DELAY_OPTS,
                                    state.settings.scrollRepeatDelayMs);
                if ((kRepeat & KEY_RIGHT) && idx < NUM_DELAY_OPTS - 1)
                    state.settings.scrollRepeatDelayMs = SCROLL_DELAY_OPTIONS[idx + 1];
                if ((kRepeat & KEY_LEFT)  && idx > 0)
                    state.settings.scrollRepeatDelayMs = SCROLL_DELAY_OPTIONS[idx - 1];
            } else if (state.settingsSelectedItem == 1) {
                int idx = findIndex(SCROLL_INTERVAL_OPTIONS, NUM_INTERVAL_OPTS,
                                    state.settings.scrollRepeatIntervalMs);
                if ((kRepeat & KEY_RIGHT) && idx < NUM_INTERVAL_OPTS - 1)
                    state.settings.scrollRepeatIntervalMs = SCROLL_INTERVAL_OPTIONS[idx + 1];
                if ((kRepeat & KEY_LEFT)  && idx > 0)
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
                kDown &= ~KEY_B;
            }
            break;
        }
        case Screen::ArticleView: {
            if ((kRepeat & KEY_DOWN) && state.scrollY < state.cachedMaxScroll) ++state.scrollY;
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
