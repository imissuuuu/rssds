#pragma once
#include <string>
#include <vector>

// 抽出ステージ（進捗通知用）
enum class ReadabilityStage {
    Parse,     // lexbor パース開始
    PreClean,  // pre-clean 完了
    Score,     // スコアリング完了
    PostClean, // post-clean 完了
    Done       // テキスト化完了
};

// 進捗コールバック。percent は 0〜100 の概算値。
using ReadabilityProgressCb = void(*)(int percent,
                                      ReadabilityStage stage,
                                      void* user);

// 本文抽出。失敗時は stripHtml(html) を返す。
// url が空でなければ sdmc:/3ds/rssreader/extract_log.txt にログ記録する。
// cb が非 nullptr なら各ステージ完了時に呼び出す。
// outImageUrls が非 nullptr なら本文領域内の <img src> を最大10件まで追記する
// （相対URLは url を基準に絶対URL化）。
std::string extractContent(const std::string& html,
                           const std::string& url = "",
                           ReadabilityProgressCb cb = nullptr,
                           void* cb_user = nullptr,
                           std::vector<std::string>* outImageUrls = nullptr);
