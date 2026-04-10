#pragma once
#include "rss.h"
#include "feed_config.h"
#include <3ds.h>
#include <vector>
#include <string>

enum class Screen { FeedList, Loading, ArticleList, ArticleView };

struct AppState {
    Screen currentScreen = Screen::FeedList;

    // 起動時にロード（ネットワーク不要）
    std::vector<FeedConfig> feedConfigs;

    // オンデマンドでロード。feedConfigs と同じインデックス
    std::vector<Feed> feeds;
    std::vector<bool> feedLoaded;

    int selectedFeed    = 0;
    int selectedArticle = 0;
    int scrollY         = 0;
    std::string statusMsg;
};

void uiInit();
void uiExit();
void uiDraw(const AppState& state);
void uiHandleInput(AppState& state, u32 kDown, u32 kHeld);
