#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

static const char* SETTINGS_PATH = "sdmc:/3ds/rssreader/settings.json";

// "key": 整数値 を JSON 文字列から取り出す。見つからなければ false を返す。
static bool parseJsonInt(const char* json, const char* key, int& out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return false;
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return false;
    ++pos;
    while (*pos == ' ' || *pos == '\t') ++pos;
    if (*pos == '\0') return false;
    char* end;
    long val = strtol(pos, &end, 10);
    if (end == pos) return false;
    out = (int)val;
    return true;
}

bool settingsLoad(AppSettings& out) {
    out = AppSettings{};

    FILE* f = fopen(SETTINGS_PATH, "r");
    if (!f) return false;

    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    int val;
    if (parseJsonInt(buf, "scroll_repeat_delay_ms", val))
        out.scrollRepeatDelayMs = val;
    if (parseJsonInt(buf, "scroll_repeat_interval_ms", val))
        out.scrollRepeatIntervalMs = val;

    // バリデーション: 想定外の値はデフォルト値に戻す
    static const int VALID_DELAYS[]    = { 200, 300, 400, 500 };
    static const int VALID_INTERVALS[] = { 50, 80, 120, 160 };
    auto inList = [](int v, const int* arr, int n) -> bool {
        for (int i = 0; i < n; ++i) if (arr[i] == v) return true;
        return false;
    };
    if (!inList(out.scrollRepeatDelayMs,    VALID_DELAYS,    4))
        out.scrollRepeatDelayMs = AppSettings{}.scrollRepeatDelayMs;
    if (!inList(out.scrollRepeatIntervalMs, VALID_INTERVALS, 4))
        out.scrollRepeatIntervalMs = AppSettings{}.scrollRepeatIntervalMs;

    return true;
}

bool settingsSave(const AppSettings& cfg) {
    mkdir("sdmc:/3ds",           0777);
    mkdir("sdmc:/3ds/rssreader", 0777);

    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"scroll_repeat_delay_ms\": %d,\n",   cfg.scrollRepeatDelayMs);
    fprintf(f, "  \"scroll_repeat_interval_ms\": %d\n", cfg.scrollRepeatIntervalMs);
    fprintf(f, "}\n");

    fclose(f);
    return true;
}
