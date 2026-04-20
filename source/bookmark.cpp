#include "bookmark.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>

static const char* PATH = "sdmc:/3ds/rssreader/bookmarks.json";

std::string BookmarkStore::keyFor(const std::string& link, const std::string& title) {
    return link.empty() ? title : link;
}

bool BookmarkStore::isBookmarked(const std::string& link, const std::string& title) const {
    std::string key = keyFor(link, title);
    for (const auto& b : bookmarks_)
        if (keyFor(b.link, b.title) == key) return true;
    return false;
}

void BookmarkStore::toggle(const std::string& title, const std::string& link,
                           const std::string& feedTitle) {
    std::string key = keyFor(link, title);
    auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(),
        [&](const Bookmark& b){ return keyFor(b.link, b.title) == key; });
    if (it != bookmarks_.end())
        bookmarks_.erase(it);
    else
        bookmarks_.push_back({ title, link, feedTitle });
    save();
}

static std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    char buf[8];
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// "key":"value" を文字列から取り出す
static std::string extractStr(const char* p, const char* key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(p, search);
    if (!pos) return {};
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return {};
    ++pos;
    while (*pos == ' ') ++pos;
    if (*pos != '"') return {};
    ++pos;
    std::string val;
    while (*pos && *pos != '"') {
        if (*pos == '\\' && *(pos + 1)) ++pos;
        val += *pos++;
    }
    return val;
}

void BookmarkStore::load() {
    bookmarks_.clear();
    FILE* f = fopen(PATH, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return; }

    std::string buf(sz, '\0');
    size_t n = fread(&buf[0], 1, sz, f);
    fclose(f);
    if (n < (size_t)sz) buf.resize(n);

    // "bookmarks":[{...},{...}] を手動パース
    const char* arr = strstr(buf.c_str(), "\"bookmarks\"");
    if (!arr) return;
    arr = strchr(arr, '[');
    if (!arr) return;
    ++arr;

    while (*arr) {
        while (*arr == ' ' || *arr == '\n' || *arr == '\r' || *arr == ',') ++arr;
        if (*arr == ']' || *arr == '\0') break;
        if (*arr != '{') { ++arr; continue; }
        const char* end = strchr(arr, '}');
        if (!end) break;
        std::string obj(arr, end - arr + 1);
        Bookmark b;
        b.title     = extractStr(obj.c_str(), "title");
        b.link      = extractStr(obj.c_str(), "link");
        b.feedTitle = extractStr(obj.c_str(), "feed");
        if (!b.title.empty() || !b.link.empty())
            bookmarks_.push_back(std::move(b));
        arr = end + 1;
    }
}

void BookmarkStore::save() const {
    mkdir("sdmc:/3ds",           0777);
    mkdir("sdmc:/3ds/rssreader", 0777);

    FILE* f = fopen(PATH, "w");
    if (!f) return;

    fprintf(f, "{\"bookmarks\":[\n");
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
        const auto& b = bookmarks_[i];
        fprintf(f, "  {\"title\":\"%s\",\"link\":\"%s\",\"feed\":\"%s\"}%s\n",
                esc(b.title).c_str(), esc(b.link).c_str(), esc(b.feedTitle).c_str(),
                (i + 1 < bookmarks_.size()) ? "," : "");
    }
    fprintf(f, "]}\n");
    fclose(f);
}
