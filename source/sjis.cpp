#include "sjis.h"
#include "sjis_table.h"
#include <cstdint>

// UTF-8 コードポイントエンコーダ
static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// SJIS 先頭バイト → sjis_table 行インデックス (0..59)、範囲外は -1
static inline int sjisLeadBucket(uint8_t c) {
    if (c >= 0x81 && c <= 0x9F) return c - 0x81;         // 0..30
    if (c >= 0xE0 && c <= 0xFC) return c - 0xE0 + 31;    // 31..59
    return -1;
}

// SJIS 後続バイト → sjis_table 列インデックス (0..187)、範囲外は -1
static inline int sjisTrailBucket(uint8_t c) {
    if (c >= 0x40 && c <= 0x7E) return c - 0x40;         // 0..62
    if (c >= 0x80 && c <= 0xFC) return c - 0x80 + 63;    // 63..187
    return -1;
}

std::string sjisToUtf8(const std::string& sjis) {
    std::string out;
    out.reserve(sjis.size() * 3 / 2);  // 控えめに 1.5 倍で予約

    const uint8_t* p = reinterpret_cast<const uint8_t*>(sjis.data());
    const size_t len = sjis.size();
    size_t i = 0;

    while (i < len) {
        uint8_t c = p[i];

        // ASCII (JIS X 0201 Roman を含む)
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }

        // JIS X 0201 半角カタカナ: 0xA1-0xDF → U+FF61-FF9F
        if (c >= 0xA1 && c <= 0xDF) {
            appendUtf8(out, 0xFF61 + (c - 0xA1));
            ++i;
            continue;
        }

        // JIS X 0208 2 バイト
        int lead = sjisLeadBucket(c);
        if (lead >= 0 && i + 1 < len) {
            uint8_t c2 = p[i + 1];
            int trail = sjisTrailBucket(c2);
            if (trail >= 0) {
                uint16_t uni = sjis_table::TABLE[lead][trail];
                if (uni != 0) {
                    appendUtf8(out, uni);
                    i += 2;
                    continue;
                }
            }
        }

        // 未対応/破損バイト
        appendUtf8(out, 0xFFFD);
        ++i;
    }

    return out;
}
