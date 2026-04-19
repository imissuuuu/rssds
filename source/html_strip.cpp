#include "html_strip.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// 中身ごとスキップするタグ
static bool isSkipContentTag(const char* tag, int len) {
    static const char* SKIP[] = {
        "script", "style", "noscript", nullptr
    };
    for (int i = 0; SKIP[i]; ++i) {
        int sl = (int)strlen(SKIP[i]);
        if (sl == len && strncasecmp(tag, SKIP[i], len) == 0) return true;
    }
    return false;
}

// 改行を挿入するブロックタグ（小文字で比較）
static bool isBlockTag(const char* tag, int len) {
    static const char* BLOCK[] = {
        "p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6",
        "tr", "blockquote", "pre", nullptr
    };
    for (int i = 0; BLOCK[i]; ++i) {
        int bl = (int)strlen(BLOCK[i]);
        if (bl == len && strncasecmp(tag, BLOCK[i], len) == 0) return true;
    }
    return false;
}

// HTMLエンティティを1文字にデコードして result に追記する。
// 不明なエンティティはそのまま追記する。
// src は '&' の次の文字から始まり、';' で終わる想定。
// consumed は '&' を含む消費バイト数を返す。
static void decodeEntity(const char* src, int srcLen, std::string& result) {
    // src[0..srcLen-1] は '&' の次 ～ ';' の前（セミコロン不含）
    if (srcLen <= 0) { result += '&'; return; }

    if (src[0] == '#') {
        // 数値文字参照
        long codepoint = 0;
        if (srcLen > 1 && (src[1] == 'x' || src[1] == 'X')) {
            codepoint = strtol(src + 2, nullptr, 16);
        } else {
            codepoint = strtol(src + 1, nullptr, 10);
        }
        // ASCII範囲のみデコード（3DSフォントはASCII外記号が化けるが、
        // 日本語はシステムフォントでサポートされる）
        if (codepoint > 0 && codepoint < 0x110000) {
            if (codepoint < 0x80) {
                result += static_cast<char>(codepoint);
            } else if (codepoint < 0x800) {
                result += static_cast<char>(0xC0 | (codepoint >> 6));
                result += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else if (codepoint < 0x10000) {
                result += static_cast<char>(0xE0 | (codepoint >> 12));
                result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            // 4バイトUTF-8（サロゲートペア相当）は省略
        }
        return;
    }

    // 名前付きエンティティ
    struct { const char* name; char ch; } ENTITIES[] = {
        {"amp",  '&'}, {"lt",   '<'}, {"gt",   '>'},
        {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
        {nullptr, 0}
    };
    for (int i = 0; ENTITIES[i].name; ++i) {
        int nl = (int)strlen(ENTITIES[i].name);
        if (nl == srcLen && strncmp(src, ENTITIES[i].name, nl) == 0) {
            result += ENTITIES[i].ch;
            return;
        }
    }

    // 不明なエンティティ: そのまま出力
    result += '&';
    result.append(src, srcLen);
    result += ';';
}

std::string stripHtml(const std::string& html) {
    std::string result;
    result.reserve(html.size());

    const char* p   = html.c_str();
    const char* end = p + html.size();

    while (p < end) {
        if (*p == '<') {
            // タグ開始
            const char* tagStart = p + 1;
            // 閉じタグかどうか
            bool isClose = (tagStart < end && *tagStart == '/');
            if (isClose) tagStart++;

            // タグ名の終端（空白または '>'）を探す
            const char* nameEnd = tagStart;
            while (nameEnd < end && *nameEnd != '>' && !isspace((unsigned char)*nameEnd))
                nameEnd++;

            int nameLen = (int)(nameEnd - tagStart);

            // img タグ: src= を抽出して \x01URL\x01 マーカーを emit（絶対 HTTP(S) URL のみ）
            if (!isClose && nameLen == 3 && strncasecmp(tagStart, "img", 3) == 0) {
                const char* tagEnd = p;
                while (tagEnd < end && *tagEnd != '>') tagEnd++;
                const char* q = nameEnd;
                while (q + 3 < tagEnd) {
                    if (strncasecmp(q, "src=", 4) == 0) {
                        q += 4;
                        char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
                        const char* valStart = q;
                        if (quote) { while (q < tagEnd && *q != quote) q++; }
                        else        { while (q < tagEnd && !isspace((unsigned char)*q) && *q != '>') q++; }
                        std::string src(valStart, q - valStart);
                        if ((src.compare(0, 7, "http://")  == 0 ||
                             src.compare(0, 8, "https://") == 0) &&
                            src.compare(0, 5, "data:") != 0) {
                            result += '\x01';
                            result += src;
                            result += '\x01';
                        }
                        break;
                    }
                    q++;
                }
                p = tagEnd;
                if (p < end) p++;
                continue;
            }

            // script / style は中身ごとスキップ
            if (!isClose && isSkipContentTag(tagStart, nameLen)) {
                // '>' まで読み飛ばして開始タグを閉じる
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                // 対応する閉じタグを探してスキップ
                char closeTag[16] = "</";
                int copyLen = nameLen < 13 ? nameLen : 13;
                for (int i = 0; i < copyLen; ++i)
                    closeTag[2 + i] = (char)tolower((unsigned char)tagStart[i]);
                closeTag[2 + copyLen] = '\0';
                const char* found = end;
                for (const char* q = p; q + copyLen + 2 < end; ++q) {
                    bool match = true;
                    for (int i = 0; closeTag[i] && match; ++i)
                        if (tolower((unsigned char)q[i]) != closeTag[i]) match = false;
                    if (match) { found = q; break; }
                }
                // 閉じタグの '>' まで読み飛ばす
                p = found;
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            // ブロックタグなら改行を追加
            if (isBlockTag(tagStart, nameLen)) {
                if (!result.empty() && result.back() != '\n')
                    result += '\n';
            }

            // '>' まで読み飛ばす
            while (p < end && *p != '>') p++;
            if (p < end) p++; // '>' を消費
            continue;
        }

        if (*p == '&') {
            // エンティティ参照
            const char* entStart = p + 1;
            const char* entEnd   = entStart;
            while (entEnd < end && *entEnd != ';' && *entEnd != '<' && (entEnd - entStart) < 16)
                entEnd++;

            if (entEnd < end && *entEnd == ';') {
                decodeEntity(entStart, (int)(entEnd - entStart), result);
                p = entEnd + 1;
            } else {
                // ';' が見つからない → そのまま出力
                result += *p++;
            }
            continue;
        }

        result += *p++;
    }

    // 連続する空行を1つに正規化
    std::string normalized;
    normalized.reserve(result.size());
    int consecutiveNewlines = 0;
    for (char c : result) {
        if (c == '\n') {
            ++consecutiveNewlines;
            if (consecutiveNewlines <= 2) normalized += c;
        } else {
            consecutiveNewlines = 0;
            normalized += c;
        }
    }

    return normalized;
}

