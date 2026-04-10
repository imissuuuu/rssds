#pragma once
#include <string>
#include <3ds/types.h>

bool netInit();
void netExit();
Result netLastError(); // netInit失敗時のsocInitエラーコード

// URLのコンテンツをHTTPS GETで取得する。
// 失敗時は空文字列を返し、errMsgにエラー内容をセットする。
std::string httpGet(const std::string& url, std::string& errMsg);
