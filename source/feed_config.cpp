#include "feed_config.h"
#include <cstdio>

std::vector<std::string> loadFeedUrls(const std::string& path) {
    std::vector<std::string> urls;

    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return urls;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        // 末尾の改行・空白を除去
        int len = 0;
        while (line[len] != '\0') len++;
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = '\0';

        if (len == 0) continue;        // 空行
        if (line[0] == '#') continue;  // コメント行

        urls.emplace_back(line);
    }

    fclose(fp);
    return urls;
}
