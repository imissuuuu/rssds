#pragma once
#include <string>

// Shift_JIS (CP932 の主要部分) を UTF-8 に変換する。
//
// 対応範囲:
//   - ASCII (0x00-0x7F)                    そのまま
//   - JIS X 0201 半角カタカナ (0xA1-0xDF)  U+FF61-FF9F に展開
//   - JIS X 0208 2 バイト文字              sjis_table.h で変換
//
// 非対応/破損バイトは U+FFFD (置換文字) を出力する。
// EUC-JP や JIS X 0212 は対象外。
//
// 入力が既に UTF-8 / ASCII の場合も「Shift_JIS として解釈」するため、
// 呼び出し側が charset 判定済みのときだけ呼ぶこと。
std::string sjisToUtf8(const std::string& sjis);
