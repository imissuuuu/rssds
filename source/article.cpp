#include "article.h"
#include "net.h"
#include "readability.h"

FetchedArticle fetchArticleBody2(const std::string& url, std::string& errMsg) {
    FetchedArticle out;
    std::string html = httpGet(url, errMsg);
    if (html.empty()) return out;
    out.body = extractContent(html, url, nullptr, nullptr, &out.imageUrls);
    return out;
}

std::string fetchArticleBody(const std::string& url, std::string& errMsg) {
    return fetchArticleBody2(url, errMsg).body;
}
