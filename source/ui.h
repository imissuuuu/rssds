#pragma once
#include "rss.h"
#include "feed_config.h"
#include "settings.h"
#include <3ds.h>
#include <vector>
#include <string>

enum class Screen { FeedList, Loading, ArticleList, ArticleView, Settings };

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
    int articleListScrollX  = 0;  // 記事一覧: 選択タイトルの水平スクロール量(px)
    std::string statusMsg;

    AppSettings settings;
    int settingsSelectedItem = 0;
};

void uiInit();
void uiExit();
void uiDraw(const AppState& state);
void uiHandleInput(AppState& state, u32 kDown, u32 kHeld, u32 kRepeat);
