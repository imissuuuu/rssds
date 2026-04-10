#include "net.h"
#include <3ds.h>
#include <curl/curl.h>
#include <cstring>
#include <cstdio>
#include <malloc.h>

static constexpr u32 SOC_BUFFER_SIZE = 0x100000; // 1MB
static u32* socBuffer = nullptr;
static Result lastSocResult = 0;

Result netLastError() { return lastSocResult; }

static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

bool netInit() {
    // socInit は 0x1000 境界アライメントが必要
    socBuffer = static_cast<u32*>(memalign(0x1000, SOC_BUFFER_SIZE));
    if (!socBuffer) return false;

    Result ret = socInit(socBuffer, SOC_BUFFER_SIZE);
    lastSocResult = ret;
    if (R_FAILED(ret)) {
        free(socBuffer);
        socBuffer = nullptr;
        return false;
    }

    curl_global_init(CURL_GLOBAL_ALL);
    return true;
}

void netExit() {
    curl_global_cleanup();
    socExit();
    free(socBuffer);
    socBuffer = nullptr;
}

std::string httpGet(const std::string& url, std::string& errMsg) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        errMsg = "curl_easy_init failed";
        return {};
    }

    std::string body;
    body.reserve(64 * 1024);

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO,         "sdmc:/3ds/rssreader/cacert.pem");
    // ユーザーエージェント: 一部サーバーが空UAをブロックするため設定
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "3DS-RSSReader/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        errMsg = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return {};
    }

    curl_easy_cleanup(curl);
    return body;
}
