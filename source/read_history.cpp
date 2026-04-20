#include "read_history.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static const char* PATH = "sdmc:/3ds/rssreader/read_history.json";

std::string ReadHistory::keyFor(const std::string& link, const std::string& title) {
    return link.empty() ? title : link;
}

void ReadHistory::markRead(const std::string& key) {
    if (key.empty()) return;
    read_.insert(key);
    save();
}

bool ReadHistory::isRead(const std::string& key) const {
    return !key.empty() && read_.count(key) > 0;
}

// JSON エスケープ（" と \ のみ）
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void ReadHistory::load() {
    read_.clear();
    FILE* f = fopen(PATH, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return; }

    std::string buf(sz, '\0');
    fread(&buf[0], 1, sz, f);
    fclose(f);

    // "read":["url1","url2",...] を手動パース
    const char* p = strstr(buf.c_str(), "\"read\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    ++p;

    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == ',') ++p;
        if (*p == ']' || *p == '\0') break;
        if (*p != '"') { ++p; continue; }
        ++p;
        std::string key;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p + 1)) { ++p; }
            key += *p++;
        }
        if (*p == '"') ++p;
        if (!key.empty()) read_.insert(key);
    }
}

void ReadHistory::save() const {
    mkdir("sdmc:/3ds",           0777);
    mkdir("sdmc:/3ds/rssreader", 0777);

    FILE* f = fopen(PATH, "w");
    if (!f) return;

    fprintf(f, "{\"read\":[");
    bool first = true;
    for (const auto& k : read_) {
        if (!first) fprintf(f, ",");
        fprintf(f, "\"%s\"", jsonEscape(k).c_str());
        first = false;
    }
    fprintf(f, "]}\n");
    fclose(f);
}
