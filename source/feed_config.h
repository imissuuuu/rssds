#pragma once
#include <string>
#include <vector>

struct FeedConfig {
    std::string url;
    std::string name;           // 省略時は url をそのまま使用
    bool fetch_full_text = false;
};

// sdmc:/3ds/rssreader/feeds.json を読む。
// ファイルが存在しない / パース失敗時は空 vector を返す。
std::vector<FeedConfig> loadFeedConfig(const std::string& path);
