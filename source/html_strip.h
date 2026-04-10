#pragma once
#include <string>

// HTMLタグを除去してプレーンテキストに変換する（RSS description 整形用）。
// <p><div><br><li><h1-h6> → 改行
// &amp; &lt; &gt; &quot; &#数値; → デコード
// その他タグ → 除去（テキストは保持）
std::string stripHtml(const std::string& html);

// Lexbor DOM + Readabilityヒューリスティクスで本文ブロックを抽出する。
// 失敗時（Lexborエラー・本文ゼロ）は stripHtml(html) のフォールバックを返す。
std::string extractContent(const std::string& html);
