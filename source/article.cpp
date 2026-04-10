#include "article.h"
#include "net.h"
#include "html_strip.h"
#include <cstring>
#include <cctype>

// html[pos] から始まるタグ名が tagName と一致するか（大文字小文字無視）
// タグ名の直後が '>' / ' ' / '\t' / '\n' / '/' であることも確認する
static bool matchTag(const std::string& html, size_t pos, const char* tagName) {
    size_t len = strlen(tagName);
    if (pos + len >= html.size()) return false;
    for (size_t i = 0; i < len; ++i) {
        if (tolower((unsigned char)html[pos + i]) != tagName[i]) return false;
    }
    char next = html[pos + len];
    return next == '>' || next == ' ' || next == '\t' || next == '\n' || next == '/';
}

// html から <tagName>...</tagName> の内容を返す。
// 最初に見つかった要素の内容のみ（ネスト非対応）。
static std::string extractTag(const std::string& html, const char* tagName) {
    size_t tagLen = strlen(tagName);

    // 開始タグを探す
    size_t start = std::string::npos;
    for (size_t i = 0; i + tagLen + 1 < html.size(); ++i) {
        if (html[i] == '<' && matchTag(html, i + 1, tagName)) {
            start = i;
            break;
        }
    }
    if (start == std::string::npos) return {};

    // 開始タグの '>' を探す
    size_t tagEnd = html.find('>', start);
    if (tagEnd == std::string::npos) return {};
    size_t contentStart = tagEnd + 1;

    // 終了タグを探す（最初に見つかったもの）
    for (size_t i = contentStart; i + tagLen + 2 < html.size(); ++i) {
        if (html[i] == '<' && html[i + 1] == '/' && matchTag(html, i + 2, tagName)) {
            return html.substr(contentStart, i - contentStart);
        }
    }

    return {};
}

std::string fetchArticleBody(const std::string& url, std::string& errMsg) {
    std::string html = httpGet(url, errMsg);
    if (html.empty()) return {};

    // <article> → <main> の順で本文候補を探す
    std::string content = extractTag(html, "article");
    if (content.empty()) content = extractTag(html, "main");
    if (content.empty()) content = html;  // フォールバック: ページ全体

    return stripHtml(content);
}
