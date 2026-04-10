#include "html_strip.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

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

// =============================================================================
// extractContent: Lexbor DOM + Readability ヒューリスティクス
// スタックオーバーフロー対策:
//   1. HTML 入力を 256KB に切り詰めて Lexbor の内部再帰を抑制
//   2. 全 DOM トラバーサルを lxb_dom_node_simple_walk（内部イテレーティブ）で実装
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif
#include "lexbor/html/html.h"
#ifdef __cplusplus
}
#endif

#include <vector>

static const size_t EC_MAX_HTML_BYTES = 256 * 1024; // 256KB

static const char* NOISE_TAGS[]    = {
    "script","style","noscript","nav","header","footer","aside","form", nullptr
};
static const char* PRIORITY_TAGS[] = { "article", "main", nullptr };
static const char* CANDIDATE_TAGS[] = { "div", "section", nullptr };
static const char* TEXT_TAGS[]        = { "p", "li", nullptr };
static const char* SKIP_SUBTREE_TAGS[] = { "pre", nullptr };
static const char* BLOCK_TAGS[]    = {
    "p","div","br","li","h1","h2","h3","h4","h5","h6",
    "tr","blockquote", nullptr
};

// node のタグ名が name と一致するか（Lexbor は小文字で保持）
static bool ec_tagEq(lxb_dom_node_t* node, const char* name) {
    if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    size_t len = 0;
    const lxb_char_t* tag = lxb_dom_element_local_name(
        lxb_dom_interface_element(node), &len);
    if (!tag || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!name[i]) return false;
        if (tag[i] != (lxb_char_t)name[i]) return false; // Lexbor は既に小文字
    }
    return name[len] == '\0';
}

static bool ec_tagIn(lxb_dom_node_t* node, const char** tags) {
    for (int i = 0; tags[i]; ++i)
        if (ec_tagEq(node, tags[i])) return true;
    return false;
}

// ---- ノイズ除去（simple_walk + 後処理削除） ----

static lexbor_action_t ec_noise_collect_cb(lxb_dom_node_t* node, void* ctx) {
    auto* list = static_cast<std::vector<lxb_dom_node_t*>*>(ctx);
    if (ec_tagIn(node, NOISE_TAGS)) {
        list->push_back(node);
        return LEXBOR_ACTION_NEXT; // サブツリーごと削除するので子はスキップ
    }
    return LEXBOR_ACTION_OK;
}

static void ec_removeNoise(lxb_dom_node_t* root) {
    std::vector<lxb_dom_node_t*> to_remove;
    lxb_dom_node_simple_walk(root, ec_noise_collect_cb, &to_remove);
    for (auto* n : to_remove) lxb_dom_node_remove(n);
}

// ---- article/main を探す（simple_walk） ----

static lexbor_action_t ec_priority_find_cb(lxb_dom_node_t* node, void* ctx) {
    auto* found = static_cast<lxb_dom_node_t**>(ctx);
    if (ec_tagIn(node, PRIORITY_TAGS)) {
        *found = node;
        return LEXBOR_ACTION_STOP;
    }
    return LEXBOR_ACTION_OK;
}

static lxb_dom_node_t* ec_findPriority(lxb_dom_node_t* root) {
    lxb_dom_node_t* found = nullptr;
    lxb_dom_node_simple_walk(root, ec_priority_find_cb, &found);
    return found;
}

// ---- スコアリング（simple_walk） ----

struct ScoreCtx {
    double text_score;  // p/pre/li の文字数 + 句読点
    double link_text;   // <a> 内の文字数
};

static lexbor_action_t ec_score_cb(lxb_dom_node_t* node, void* ctx) {
    auto* sc = static_cast<ScoreCtx*>(ctx);
    if (ec_tagIn(node, TEXT_TAGS)) {
        size_t len = 0;
        lxb_char_t* txt = lxb_dom_node_text_content(node, &len);
        // txt は document destroy 時に一括解放。lexbor_free() は不可（mraw 管理下）
        if (txt) {
            sc->text_score += (double)len;
            for (size_t i = 0; i < len; ++i) {
                char c = (char)txt[i];
                if (c=='.'||c==','||c==';'||c==':'||c=='!'||c=='?')
                    sc->text_score += 1.0;
            }
        }
        return LEXBOR_ACTION_NEXT; // 子を二重カウントしない
    }
    if (ec_tagEq(node, "a")) {
        size_t len = 0;
        lxb_char_t* txt = lxb_dom_node_text_content(node, &len);
        if (txt) { sc->link_text += (double)len; }
        return LEXBOR_ACTION_NEXT;
    }
    return LEXBOR_ACTION_OK;
}

static double ec_scoreCandidate(lxb_dom_node_t* node) {
    size_t totalLen = 0;
    lxb_char_t* txt = lxb_dom_node_text_content(node, &totalLen);
    if (!txt || totalLen == 0) return 0.0;

    ScoreCtx sc = {0.0, 0.0};
    lxb_dom_node_simple_walk(node, ec_score_cb, &sc);

    double lk = sc.link_text / (double)totalLen;
    return sc.text_score * (1.0 - lk);
}

// ---- テキスト化（simple_walk） ----

static lexbor_action_t ec_text_cb(lxb_dom_node_t* node, void* ctx) {
    auto* out = static_cast<std::string*>(ctx);
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && ec_tagIn(node, SKIP_SUBTREE_TAGS))
        return LEXBOR_ACTION_NEXT;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t len = 0;
        lxb_char_t* txt = lxb_dom_node_text_content(node, &len);
        // txt は document destroy 時に一括解放。lexbor_free() は不可（mraw 管理下）
        if (txt) { out->append((char*)txt, len); }
        return LEXBOR_ACTION_OK;
    }
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && ec_tagIn(node, BLOCK_TAGS)) {
        if (!out->empty() && out->back() != '\n')
            *out += '\n';
    }
    return LEXBOR_ACTION_OK;
}

// ---- 公開関数 ----

std::string extractContent(const std::string& html) {
    // 1. 入力サイズ制限（Lexbor 内部の DOM 構築による再帰深さを抑制）
    size_t parse_len = html.size() > EC_MAX_HTML_BYTES ? EC_MAX_HTML_BYTES : html.size();

    lxb_html_document_t* doc = lxb_html_document_create();
    if (!doc) return stripHtml(html);

    lxb_status_t st = lxb_html_document_parse(
        doc, (const lxb_char_t*)html.c_str(), parse_len);
    if (st != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc);
        return stripHtml(html);
    }

    lxb_html_body_element_t* bodyEl = lxb_html_document_body_element(doc);
    if (!bodyEl) {
        lxb_html_document_destroy(doc);
        return stripHtml(html);
    }
    lxb_dom_node_t* body = lxb_dom_interface_node(bodyEl);

    // 2. ノイズ除去
    ec_removeNoise(body);

    // 3. article/main があれば即採用
    lxb_dom_node_t* winner = ec_findPriority(body);

    // 4. なければ body 直下の div/section をスコアリング
    if (!winner) {
        double bestScore = 0.0;
        lxb_dom_node_t* node = lxb_dom_node_first_child(body);
        while (node) {
            if (ec_tagIn(node, CANDIDATE_TAGS)) {
                double s = ec_scoreCandidate(node);
                if (s > bestScore) { bestScore = s; winner = node; }
            }
            node = lxb_dom_node_next(node);
        }
    }

    // 5. 勝者なしは body 全体
    if (!winner) winner = body;

    // 6. テキスト化
    std::string result;
    result.reserve(4096);
    lxb_dom_node_simple_walk(winner, ec_text_cb, &result);

    lxb_html_document_destroy(doc);

    if (result.empty()) return stripHtml(html);

    // 連続空行を正規化
    std::string normalized;
    normalized.reserve(result.size());
    int nlCount = 0;
    for (char c : result) {
        if (c == '\n') { if (++nlCount <= 2) normalized += c; }
        else           { nlCount = 0; normalized += c; }
    }
    return normalized;
}
