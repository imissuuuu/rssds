#include "article.h"
#include "net.h"
#include "html_strip.h"

std::string fetchArticleBody(const std::string& url, std::string& errMsg) {
    std::string html = httpGet(url, errMsg);
    if (html.empty()) return {};
    return extractContent(html);
}
