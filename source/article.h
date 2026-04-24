#pragma once
#include "net.h"
#include <string>
#include <vector>

// 記事取得結果: 本文 + 画像URL列
struct FetchedArticle {
    std::string body;                   // 本文プレーンテキスト
    std::vector<std::string> imageUrls; // 本文領域の画像URL（最大10件）
};

// リンク先URLからHTMLを取得し、本文と画像URL群を返す。
// <article> → <main> の順で本文要素を探し、なければページ全体を変換する。
// 失敗時は body が空の FetchedArticle を返し errMsg にエラー内容をセット。
// progressFn/progressUd は省略可能（ダウンロード進捗コールバック）。
FetchedArticle fetchArticleBody2(const std::string& url, std::string& errMsg,
                                 XferInfoFn progressFn = nullptr, void* progressUd = nullptr);

// 既存互換: 本文テキストのみ返す薄いラッパ。
std::string fetchArticleBody(const std::string& url, std::string& errMsg);
