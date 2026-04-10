#pragma once
#include <string>

// HTMLタグを除去してプレーンテキストに変換する。
// <p><div><br><li><h1-h6> → 改行
// &amp; &lt; &gt; &quot; &#数値; → デコード
// その他タグ → 除去（テキストは保持）
std::string stripHtml(const std::string& html);
