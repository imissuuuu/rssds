#pragma once
#include <string>

// HTMLタグを除去してプレーンテキストに変換する（RSS description 整形用）。
// <p><div><br><li><h1-h6> → 改行
// &amp; &lt; &gt; &quot; &#数値; → デコード
// その他タグ → 除去（テキストは保持）
std::string stripHtml(const std::string& html);

// Lexbor DOM + Readabilityヒューリスティクスで本文ブロックを抽出する。
// 失敗時（Lexborエラー・本文ゼロ）は stripHtml(html) のフォールバックを返す。
// url を与えると sdmc:/3ds/rssreader/extract_log.txt に抽出結果をログ記録する
// （Phase 4.7 のフィルタ改善の根拠データ）。空文字列の場合はログしない。
std::string extractContent(const std::string& html, const std::string& url = "");
