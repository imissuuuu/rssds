#pragma once
#include "article_loader.h"
#include "bookmark.h"
#include "feed_config.h"
#include "feed_loader.h"
#include "image_cache.h"
#include "image_loader.h"
#include "read_history.h"
#include "refresh_all_loader.h"
#include "rss.h"
#include "settings.h"
#include <3ds.h>
#include <string>
#include <vector>

enum class Screen {
    FeedList,
    Loading,
    LoadingAll,
    ArticleList,
    ArticleView,
    ImageView,
    Settings,
    LoadingArticle,
    BookmarkList,
    ManageFeeds,
};

enum class FeedListPopup { None, Menu, Move, DeleteConfirm };

enum class ManageFeedsPopup { None, Menu, Move, DeleteConfirm, DiscardConfirm };

enum class LineKind : uint8_t { Text, Image };

struct ContentLine {
    LineKind kind = LineKind::Text;
    std::string text;     // LineKind::Text のとき使用
    std::string imageUrl; // LineKind::Image のとき使用
};

struct AppState {
    Screen currentScreen = Screen::FeedList;

    // 起動時にロード（ネットワーク不要）
    std::vector<FeedConfig> feedConfigs;

    // オンデマンドでロード。feedConfigs と同じインデックス
    std::vector<Feed> feeds;
    std::vector<bool> feedLoaded;

    int selectedFeed = 0;
    int selectedArticle = 0;
    int scrollY = 0;
    int articleListScrollX = 0;
    std::string statusMsg;

    AppSettings settings;
    int settingsSelectedItem = 0;

    // ArticleView 描画キャッシュ（毎フレームの parseContentLines 再計算を防ぐ）
    mutable std::vector<ContentLine> articleLines;
    mutable int cachedLineFeed = -1;
    mutable int cachedLineArticle = -1;
    mutable size_t cachedLineContentSize = 0;
    mutable int cachedMaxScroll = 0;

    // Phase 7 Stage 2: 画像ロード/キャッシュ (ArticleView 表示中のみ有効)
    mutable ImageLoader imgLoader;
    mutable ImageCache imgCache;
    mutable int cachedImagesFeed = -1;
    mutable int cachedImagesArticle = -1;

    // Phase 7.6: 画像拡大ビュー
    std::string imageViewUrl;
    float imageViewZoom = 1.0f;
    float imageViewOffX = 0.0f;
    float imageViewOffY = 0.0f;
    // 高解像度ローダー (1024px, ImageView 専用)
    mutable ImageLoader imgViewLoader;
    mutable ImageCache imgViewCache;

    // 記事本文非同期ロード
    mutable ArticleLoader articleLoader;
    int pendingFetchFeed = -1;
    int pendingFetchArticle = -1;
    bool pendingFetchFullArticle = false;
    Screen pendingReturnScreen = Screen::ArticleList;

    // フィード非同期ロード（単一）
    mutable FeedLoader feedLoader;
    bool feedJobSubmitted = false;

    // 全フィード並列リフレッシュ
    mutable RefreshAllLoader refreshAllLoader;

    // 既読管理
    ReadHistory readHistory;

    // ブックマーク
    BookmarkStore bookmarkStore;
    int selectedBookmark = 0;

    // ブックマーク一覧から開く記事の一時保持
    Article bookmarkTempArticle;
    std::string bookmarkTempFeedTitle;
    bool viewingBookmark = false;
    bool bookmarkConfirmRemove = false;

    // FeedList ポップアップ
    FeedListPopup feedListPopup = FeedListPopup::None;
    int feedListPopupMenuSel = 0; // 0=Move, 1=Delete
    int feedListPopupTarget = -1;
    int feedListMoveInsertPos = 0; // 0..feedConfigs.size()

    // ManageFeeds 画面
    std::vector<FeedConfig> manageFeedsEditing;
    int manageFeedsSelected = 0;
    bool manageFeedsDirty = false;
    ManageFeedsPopup manageFeedsPopup = ManageFeedsPopup::None;
    int manageFeedsPopupMenuSel = 0; // 0=Move, 1=Delete
    int manageFeedsPopupTarget = -1;
    int manageFeedsMoveInsertPos = 0; // 0..manageFeedsEditing.size()
};

void uiInit();
void uiExit();
void uiDraw(const AppState& state);
void uiHandleInput(AppState& state, u32 kDown, u32 kHeld, u32 kRepeat);
