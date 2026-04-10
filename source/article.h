#pragma once
#include <string>

// リンク先URLからHTMLを取得し、本文をプレーンテキストで返す。
// <article> → <main> の順で本文要素を探し、なければページ全体を変換する。
// 失敗時は空文字列を返し errMsg にエラー内容をセット。
std::string fetchArticleBody(const std::string& url, std::string& errMsg);
