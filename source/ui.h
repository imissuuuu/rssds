#pragma once
#include "rss.h"
#include "feed_config.h"
#include "settings.h"
#include "image_loader.h"
#include "image_cache.h"
#include "article_loader.h"
#include "feed_loader.h"
#include <3ds.h>
#include <vector>
#include <string>

enum class Screen { FeedList, Loading, LoadingAll, ArticleList, ArticleView, ImageView, Settings, LoadingArticle };

enum class LineKind : uint8_t { Text, Image };

struct ContentLine {
    LineKind    kind     = LineKind::Text;
    std::string text;      // LineKind::Text のとき使用
    std::string imageUrl;  // LineKind::Image のとき使用
};

struct AppState {
    Screen currentScreen = Screen::FeedList;

    // 起動時にロード（ネットワーク不要）
    std::vector<FeedConfig> feedConfigs;

    // オンデマンドでロード。feedConfigs と同じインデックス
    std::vector<Feed> feeds;
    std::vector<bool> feedLoaded;

    int selectedFeed        = 0;
    int selectedArticle     = 0;
    int scrollY             = 0;
    int articleListScrollX  = 0;
    int refreshIdx          = 0;  // 記事一覧: 選択タイトルの水平スクロール量(px)
    std::string statusMsg;

    AppSettings settings;
    int settingsSelectedItem = 0;

    // ArticleView 描画キャッシュ（毎フレームの parseContentLines 再計算を防ぐ）
    mutable std::vector<ContentLine> articleLines;
    mutable int cachedLineFeed    = -1;
    mutable int cachedLineArticle = -1;
    mutable size_t cachedLineContentSize = 0;
    mutable int cachedMaxScroll   = 0;

    // Phase 7 Stage 2: 画像ロード/キャッシュ (ArticleView 表示中のみ有効)
    mutable ImageLoader imgLoader;
    mutable ImageCache  imgCache;
    mutable int cachedImagesFeed    = -1;
    mutable int cachedImagesArticle = -1;

    // Phase 7.6: 画像拡大ビュー
    std::string imageViewUrl;
    float       imageViewZoom = 1.0f;
    float       imageViewOffX = 0.0f;
    float       imageViewOffY = 0.0f;
    // 高解像度ローダー (1024px, ImageView 専用)
    mutable ImageLoader imgViewLoader;
    mutable ImageCache  imgViewCache;

    // 記事本文非同期ロード
    mutable ArticleLoader articleLoader;
    int  pendingFetchFeed        = -1;
    int  pendingFetchArticle     = -1;
    bool pendingFetchFullArticle = false;
    Screen pendingReturnScreen   = Screen::ArticleList;

    // フィード非同期ロード
    mutable FeedLoader feedLoader;
};

void uiInit();
void uiExit();
void uiDraw(const AppState& state);
void uiHandleInput(AppState& state, u32 kDown, u32 kHeld, u32 kRepeat);
