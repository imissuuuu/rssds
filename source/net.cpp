#include "net.h"
#include "sjis.h"
#include <3ds.h>
#include <curl/curl.h>
#include <cctype>
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

// Content-Type ヘッダを捕まえて charset 文字列を取り出す。
// ヘッダは "Content-Type: text/html; charset=Shift_JIS\r\n" のような形式。
static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* charset = static_cast<std::string*>(userdata);
    size_t total = size * nitems;

    // "content-type:" で始まるヘッダだけ対象
    static const char key[] = "content-type:";
    const size_t keyLen = sizeof(key) - 1;
    if (total < keyLen) return total;
    for (size_t i = 0; i < keyLen; ++i) {
        if (tolower((unsigned char)buffer[i]) != key[i]) return total;
    }

    // charset=... を探す
    for (size_t i = keyLen; i + 8 <= total; ++i) {
        if (strncasecmp(buffer + i, "charset=", 8) == 0) {
            size_t start = i + 8;
            // 値は ; CR LF SP で終わる。" も剥がす。
            if (start < total && (buffer[start] == '"' || buffer[start] == '\'')) ++start;
            size_t end = start;
            while (end < total) {
                char c = buffer[end];
                if (c == ';' || c == '\r' || c == '\n' || c == ' '
                    || c == '"' || c == '\'') break;
                ++end;
            }
            charset->assign(buffer + start, end - start);
            break;
        }
    }
    return total;
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

// charset 名を小文字化して比較し、Shift_JIS 系統なら true を返す。
static bool isShiftJis(const std::string& cs) {
    if (cs.empty()) return false;
    std::string lo;
    lo.reserve(cs.size());
    for (char c : cs) {
        if (c == '_' || c == '-') continue;  // shift-jis / shift_jis 吸収
        lo += static_cast<char>(tolower((unsigned char)c));
    }
    return lo == "shiftjis"
        || lo == "sjis"
        || lo == "xsjis"
        || lo == "mskanji"
        || lo == "windows31j"
        || lo == "cp932";
}

// body 先頭を軽くスキャンして charset を自己申告から拾う。
// <?xml encoding="..."?> / <meta charset="..."> / <meta http-equiv=... content="...charset=...">
static std::string sniffCharsetFromBody(const std::string& body) {
    // 巨大な本文でも先頭 2KB までで十分（HTML の <head> はその範囲に入るのが通例）
    size_t scan = body.size() < 2048 ? body.size() : 2048;
    const char* p = body.c_str();

    // XML prolog
    if (scan >= 6 && strncmp(p, "<?xml", 5) == 0) {
        const char* end = static_cast<const char*>(memchr(p, '>', scan));
        if (end) {
            const char* enc = strstr(p, "encoding=");
            if (enc && enc < end) {
                enc += 9;
                char quote = (*enc == '"' || *enc == '\'') ? *enc : 0;
                if (quote) ++enc;
                const char* e = enc;
                while (e < end && *e != quote && *e != ' ' && *e != '?' && *e != '>') ++e;
                return std::string(enc, e - enc);
            }
        }
    }

    // HTML <meta> — 大文字小文字を区別せず "charset=" を探す。
    // シンプルに: scan 範囲内で "charset=" を線形検索
    for (size_t i = 0; i + 8 < scan; ++i) {
        if (strncasecmp(p + i, "charset=", 8) != 0) continue;
        size_t start = i + 8;
        if (start < scan && (p[start] == '"' || p[start] == '\'')) ++start;
        size_t end = start;
        while (end < scan) {
            char c = p[end];
            if (c == '"' || c == '\'' || c == ' ' || c == '>' || c == ';'
                || c == '/' || c == '\r' || c == '\n') break;
            ++end;
        }
        if (end > start) return std::string(p + start, end - start);
    }

    return {};
}

namespace {
struct BinaryCtx {
    std::vector<uint8_t>* buf;
    size_t                maxBytes;
    bool                  aborted;
};

struct XferCtxWrap {
    XferInfoFn fn;
    void*      ud;
};
}

static int curlXferCb(void* ud, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t, curl_off_t) {
    auto* w = static_cast<XferCtxWrap*>(ud);
    return w->fn(w->ud, (int64_t)dltotal, (int64_t)dlnow);
}


std::string httpGet(const std::string& url, std::string& errMsg,
                    XferInfoFn progressFn, void* progressUd) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        errMsg = "curl_easy_init failed";
        return {};
    }

    std::string body;
    std::string headerCharset;
    body.reserve(64 * 1024);

    curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,  headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,      &headerCharset);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO,          "sdmc:/3ds/rssreader/cacert.pem");
    // ユーザーエージェント: 一部サーバーが空UAをブロックするため設定
    curl_easy_setopt(curl, CURLOPT_USERAGENT,       "3DS-RSSReader/1.0");

    XferCtxWrap xferWrap { progressFn, progressUd };
    if (progressFn) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &xferWrap);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        errMsg = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return {};
    }

    curl_easy_cleanup(curl);

    // charset 決定: ヘッダ優先、無ければ body 自己申告、最後は UTF-8 扱いでパススルー。
    // Shift_JIS と判定された場合だけ UTF-8 へ変換する。
    // ITmedia 等、HTML を Shift_JIS で返す和サイト対策。
    std::string charset = headerCharset;
    if (charset.empty()) charset = sniffCharsetFromBody(body);
    if (isShiftJis(charset)) {
        return sjisToUtf8(body);
    }
    return body;
}

/**
 * @brief Appends a received binary chunk into the provided output buffer while enforcing a size limit.
 *
 * Appends up to `sz * nm` bytes from `ptr` into the `BinaryCtx::buf`. If the context is already marked
 * aborted or adding the incoming chunk would exceed `BinaryCtx::maxBytes`, the function sets the
 * aborted flag (when applicable) and returns 0 to signal interruption to libcurl.
 *
 * @param ptr Pointer to the incoming data.
 * @param sz Size of each incoming element in bytes.
 * @param nm Number of incoming elements.
 * @param ud Pointer to a `BinaryCtx` that holds the destination buffer, the maximum allowed bytes,
 *           and the aborted flag.
 * @return size_t Number of bytes written (`sz * nm`) on success, `0` if the transfer was aborted or
 *         would exceed the size limit.
 */
static size_t writeBinaryCallback(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* ctx = static_cast<BinaryCtx*>(ud);
    size_t n = sz * nm;
    if (ctx->aborted) return 0;
    if (ctx->buf->size() + n > ctx->maxBytes) {
        ctx->aborted = true;
        return 0;  // libcurl に中断を伝える
    }
    ctx->buf->insert(ctx->buf->end(),
                     reinterpret_cast<uint8_t*>(ptr),
                     reinterpret_cast<uint8_t*>(ptr) + n);
    return n;
}

/**
 * @brief Downloads binary data from a URL with a maximum size and optional progress reporting.
 *
 * Attempts to fetch the resource at `url` into a byte vector. If the transfer exceeds
 * `maxBytes` the download is aborted and the function reports an error.
 *
 * @param url The URL to download.
 * @param maxBytes Maximum number of bytes to accept; exceeding this aborts the transfer.
 * @param errMsg Output parameter that receives an error message on failure.
 *               Possible values include "curl_easy_init failed", "size limit exceeded",
 *               or a libcurl error string from `curl_easy_strerror`.
 * @param progressFn Optional progress callback. If non-null, it will be invoked with
 *                   `(progressUd, dltotal, dlnow)` where `dltotal` and `dlnow` are
 *                   the total and downloaded byte counts as `int64_t`.
 * @param progressUd User data pointer passed to `progressFn`.
 * @return std::vector<uint8_t> The downloaded bytes on success, or an empty vector on failure.
 */
std::vector<uint8_t> httpGetBinary(const std::string& url,
                                    size_t maxBytes,
                                    std::string& errMsg,
                                    XferInfoFn progressFn,
                                    void*       progressUd) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        errMsg = "curl_easy_init failed";
        return {};
    }

    std::vector<uint8_t> buf;
    BinaryCtx ctx { &buf, maxBytes, false };

    curl_easy_setopt(curl, CURLOPT_URL,               url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,     writeBinaryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,         &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,    1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,           30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,    1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,    2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO,            "sdmc:/3ds/rssreader/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_USERAGENT,         "3DS-RSSReader/1.0");
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)maxBytes);

    XferCtxWrap xferWrap { progressFn, progressUd };
    if (progressFn) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS,        0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,  curlXferCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,      &xferWrap);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (ctx.aborted) {
        errMsg = "size limit exceeded";
        return {};
    }
    if (res != CURLE_OK) {
        errMsg = curl_easy_strerror(res);
        return {};
    }
    return buf;
}
