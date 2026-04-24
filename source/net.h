#pragma once
#include <3ds/types.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

bool netInit();
void netExit();
Result netLastError(); // netInit失敗時のsocInitエラーコード

// ダウンロード進捗コールバック: (ud, dltotal, dlnow) → 0 で継続。dltotal=0 は不明。
using XferInfoFn = int (*)(void* ud, int64_t dltotal, int64_t dlnow);

// URLのコンテンツをHTTPS GETで取得する。
// 失敗時は空文字列を返し、errMsgにエラー内容をセットする。
// progressFn/progressUd は省略可能。
std::string httpGet(const std::string& url, std::string& errMsg, XferInfoFn progressFn = nullptr,
                    void* progressUd = nullptr);

// HTTPS GET でバイナリを取得。受信が maxBytes を超えた時点で中断し
// 空 vector を返す。画像など大きめペイロード用。
// progressFn/progressUd は省略可能。
std::vector<uint8_t> httpGetBinary(const std::string& url, size_t maxBytes, std::string& errMsg,
                                   XferInfoFn progressFn = nullptr, void* progressUd = nullptr);
