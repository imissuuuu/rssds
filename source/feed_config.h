#pragma once
#include <string>
#include <vector>

// SDカード上の feeds.txt からフィードURLを読み込む。
// 空行・'#'始まりの行はスキップ。
// ファイルが存在しない場合は空のvectorを返す。
std::vector<std::string> loadFeedUrls(const std::string& path);
