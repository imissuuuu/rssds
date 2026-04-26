#define JSON_NOEXCEPTION
#include "feed_config.h"
#include "json.hpp"
#include <cstdio>
#include <sys/stat.h>

std::vector<FeedConfig> loadFeedConfig(const std::string& path) {
    std::vector<FeedConfig> configs;

    FILE* fp = fopen(path.c_str(), "r");
    if (!fp)
        return configs;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    if (size <= 0) {
        fclose(fp);
        return configs;
    }

    std::string content(static_cast<size_t>(size), '\0');
    fread(&content[0], 1, static_cast<size_t>(size), fp);
    fclose(fp);

    // allow_exceptions=false: パース失敗時は discarded value を返す
    auto j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded())
        return configs;
    if (!j.contains("feeds") || !j["feeds"].is_array())
        return configs;

    for (const auto& item : j["feeds"]) {
        if (!item.contains("url") || !item["url"].is_string())
            continue;
        FeedConfig cfg;
        cfg.url = item["url"].get<std::string>();
        cfg.name = item.value("name", cfg.url);
        cfg.fetch_full_text = item.value("fetch_full_text", false);
        configs.push_back(std::move(cfg));
    }
    return configs;
}

bool saveFeedConfig(const std::string& path, const std::vector<FeedConfig>& configs) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/rssreader", 0777);

    FILE* fp = fopen(path.c_str(), "w");
    if (!fp)
        return false;

    fprintf(fp, "{\n  \"feeds\": [\n");
    for (int i = 0; i < (int)configs.size(); ++i) {
        const auto& c = configs[i];
        fprintf(fp, "    {\"url\": \"%s\", \"name\": \"%s\", \"fetch_full_text\": %s}%s\n",
                c.url.c_str(), c.name.c_str(), c.fetch_full_text ? "true" : "false",
                i + 1 < (int)configs.size() ? "," : "");
    }
    fprintf(fp, "  ]\n}\n");
    fclose(fp);
    return true;
}
