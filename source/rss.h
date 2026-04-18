#pragma once
#include <string>
#include <vector>

struct Article {
    std::string title;
    std::string link;
    std::string content;  // HTML or plaintext (フィード由来)
    std::string pubDate;
    bool fullFetched = false;  // true: リンク先から全文取得済み
    std::vector<std::string> imageUrls;  // 本文から抽出した画像URL（最大10件）
};

struct Feed {
    std::string title;
    std::vector<Article> articles;
};

// XML文字列をパースしてFeedを返す。RSS 2.0 / Atom を自動判別。
// 失敗時はerrMsgにエラー内容をセットして空のFeedを返す。
Feed parseFeed(const std::string& xml, std::string& errMsg);
