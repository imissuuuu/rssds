#pragma once
#include "rss.h"
#include <3ds.h>
#include <vector>
#include <string>

enum class Screen { FeedList, ArticleList, ArticleView };

struct AppState {
    Screen currentScreen = Screen::FeedList;
    std::vector<Feed> feeds;
    int selectedFeed    = 0;
    int selectedArticle = 0;
    int scrollY         = 0;  // ArticleView のスクロール行数
    std::string statusMsg;
};

void uiInit();
void uiExit();
void uiDraw(const AppState& state);
void uiHandleInput(AppState& state, u32 kDown, u32 kHeld);
