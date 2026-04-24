#include "readability.h"
#include "html_strip.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "lexbor/html/html.h"
#ifdef __cplusplus
}
#endif

#include <3ds.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

// =============================================================================
// 定数
// =============================================================================

static const size_t MAX_HTML_BYTES = 256 * 1024;
static const size_t MAX_IMAGE_URLS = 10;

// pre-clean: サブツリーごと即削除するタグ
static const char* UNLIKELY_TAGS[] = {"script", "style", "noscript", "nav",    "header",
                                      "footer", "aside", "form",     "iframe", nullptr};

// class/id キーワード: UNLIKELY_TAGS 以外のノイズ（pre-clean フェーズで適用）
static const char* NOISE_KW[] = {"share",      "sns",       "social",     "related",  "recommend",
                                 "endlink",    "comment",   "disqus",     "sidebar",  "widget",
                                 "sponsored",  "sponsor",   "promo",      "banner",   "outbrain",
                                 "newsletter", "subscribe", "cookie",     "gdpr",     "popup",
                                 "pagination", "pager",     "breadcrumb", "masthead", "menu",
                                 "byline",     "keywords",  "tags",       nullptr};

// post-clean: 本文サブツリー内の残存ノイズ
static const char* POST_NOISE_KW[] = {"share",  "social", "related", "recommend",
                                      "advert", "ads",    nullptr};

// タグごとの base score（Readability.js 準拠）
static const char* BLOCK_TAGS[] = {"p",  "div", "br", "li", "h1",         "h2",  "h3",
                                   "h4", "h5",  "h6", "tr", "blockquote", "pre", nullptr};
static const char* SKIP_SUBTREE[] = {nullptr};

// =============================================================================
// ユーティリティ
// =============================================================================

static bool tagEq(lxb_dom_node_t* n, const char* name) {
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return false;
    size_t len = 0;
    const lxb_char_t* tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    if (!tag)
        return false;
    for (size_t i = 0; i < len; ++i) {
        if (!name[i])
            return false;
        if (tag[i] != (lxb_char_t)name[i])
            return false;
    }
    return name[len] == '\0';
}

static bool tagIn(lxb_dom_node_t* n, const char** tags) {
    for (int i = 0; tags[i]; ++i)
        if (tagEq(n, tags[i]))
            return true;
    return false;
}

static const lxb_char_t* getAttr(lxb_dom_node_t* n, const char* name, size_t* len_out) {
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return nullptr;
    return lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                         reinterpret_cast<const lxb_char_t*>(name), strlen(name),
                                         len_out);
}

// class/id トークンが keywords のいずれかを部分一致で含むか
static bool attrContainsAny(const lxb_char_t* val, size_t len, const char** kw) {
    if (!val || len == 0)
        return false;
    size_t tok = 0;
    for (size_t i = 0; i <= len; ++i) {
        bool sep = (i == len || val[i] == ' ');
        if (!sep)
            continue;
        size_t tlen = i - tok;
        if (tlen > 0) {
            for (int ki = 0; kw[ki]; ++ki) {
                size_t klen = strlen(kw[ki]);
                if (klen > tlen) {
                    continue;
                }
                for (size_t j = 0; j + klen <= tlen; ++j) {
                    bool match = true;
                    for (size_t k = 0; k < klen && match; ++k)
                        match =
                            (tolower((unsigned char)val[tok + j + k]) == (unsigned char)kw[ki][k]);
                    if (match)
                        return true;
                }
            }
        }
        tok = i + 1;
    }
    return false;
}

// UTF-8 文字数（バイト数ではなく文字単位）
static size_t countCharsUtf8(const lxb_char_t* s, size_t byteLen) {
    size_t count = 0;
    for (size_t i = 0; i < byteLen;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80)
            i += 1;
        else if ((c & 0xE0) == 0xC0)
            i += 2;
        else if ((c & 0xF0) == 0xE0)
            i += 3;
        else if ((c & 0xF8) == 0xF0)
            i += 4;
        else
            i += 1;
        ++count;
    }
    return count;
}

// =============================================================================
// URL 解決（<img src> の相対URLを base URL 基準で絶対化）
// =============================================================================

// base URL から scheme://host[:port] の部分を取り出す。見つからなければ空。
static std::string urlOrigin(const std::string& base) {
    size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos)
        return {};
    size_t pathStart = base.find('/', schemeEnd + 3);
    if (pathStart == std::string::npos)
        return base;
    return base.substr(0, pathStart);
}

// base URL の「ディレクトリ部分」（末尾スラッシュ込）を取り出す。
// 例: "https://a.com/x/y.html" → "https://a.com/x/"
static std::string urlDir(const std::string& base) {
    size_t schemeEnd = base.find("://");
    size_t searchFrom = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
    size_t lastSlash = base.rfind('/');
    if (lastSlash == std::string::npos || lastSlash < searchFrom) {
        return base + "/";
    }
    return base.substr(0, lastSlash + 1);
}

// クエリ/フラグメントを除いた ref が空・無効な場合は空文字を返す。
static std::string resolveUrl(const std::string& base, const std::string& ref) {
    if (ref.empty())
        return {};
    if (ref[0] == '#')
        return {}; // フラグメントのみ
    if (ref.compare(0, 5, "data:") == 0)
        return {}; // data URL は除外

    // 既に絶対URL
    if (ref.compare(0, 7, "http://") == 0)
        return ref;
    if (ref.compare(0, 8, "https://") == 0)
        return ref;

    // protocol-relative: "//host/path"
    if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/') {
        size_t schemeEnd = base.find("://");
        std::string scheme =
            (schemeEnd == std::string::npos) ? std::string("https") : base.substr(0, schemeEnd);
        return scheme + ":" + ref;
    }

    // absolute path: "/path"
    if (!ref.empty() && ref[0] == '/') {
        std::string origin = urlOrigin(base);
        if (origin.empty())
            return {};
        return origin + ref;
    }

    // relative path
    std::string dir = urlDir(base);
    if (dir.empty())
        return {};
    return dir + ref;
}

// =============================================================================
// Phase A: pre-clean
// =============================================================================

struct CollectCtx {
    std::vector<lxb_dom_node_t*>* list;
    const char** kw;
};

static lexbor_action_t preClean_cb(lxb_dom_node_t* node, void* ctx) {
    auto* c = static_cast<CollectCtx*>(ctx);

    if (tagIn(node, UNLIKELY_TAGS)) {
        c->list->push_back(node);
        return LEXBOR_ACTION_NEXT; // サブツリーごと削除するので子をスキップ
    }

    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t len = 0;
        const lxb_char_t* cls = getAttr(node, "class", &len);
        if (attrContainsAny(cls, len, c->kw)) {
            c->list->push_back(node);
            return LEXBOR_ACTION_NEXT;
        }
        const lxb_char_t* id = getAttr(node, "id", &len);
        if (attrContainsAny(id, len, c->kw)) {
            c->list->push_back(node);
            return LEXBOR_ACTION_NEXT;
        }
    }
    return LEXBOR_ACTION_OK;
}

static void preClean(lxb_dom_node_t* root) {
    std::vector<lxb_dom_node_t*> to_remove;
    CollectCtx ctx = {&to_remove, NOISE_KW};
    lxb_dom_node_simple_walk(root, preClean_cb, &ctx);
    for (auto* n : to_remove)
        lxb_dom_node_remove(n);
}

// =============================================================================
// Phase B: Readability スコアリング
// =============================================================================

struct Candidate {
    lxb_dom_node_t* node;
    double score;
};

// ノード自身のテキスト文字数と <a> 内文字数を収集
struct TextCtx {
    size_t total;  // 全文字数
    size_t link;   // <a> 内文字数
    size_t commas; // コンマ等の句読点数
    bool in_link;
};

static lexbor_action_t textCount_cb(lxb_dom_node_t* node, void* ctx) {
    auto* tc = static_cast<TextCtx*>(ctx);
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        if (tagEq(node, "a")) {
            tc->in_link = true;
            return LEXBOR_ACTION_OK;
        }
        return LEXBOR_ACTION_OK;
    }
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t blen = 0;
        lxb_char_t* txt = lxb_dom_node_text_content(node, &blen);
        if (txt) {
            size_t chars = countCharsUtf8(txt, blen);
            tc->total += chars;
            if (tc->in_link)
                tc->link += chars;
            for (size_t i = 0; i < blen; ++i) {
                char ch = (char)txt[i];
                // 句読点カウント（日本語の読点・句点は UTF-8 3バイト: 0xE3開始）
                if (ch == ',' || ch == '.' || ch == ';' || ch == '!' || ch == '?')
                    tc->commas++;
            }
        }
    }
    return LEXBOR_ACTION_OK;
}

// ノードのテキスト文字数・リンク密度・句読点を取得
static void getTextInfo(lxb_dom_node_t* node, size_t& total, size_t& link, size_t& commas) {
    TextCtx tc = {0, 0, 0, false};
    lxb_dom_node_simple_walk(node, textCount_cb, &tc);
    total = tc.total;
    link = tc.link;
    commas = tc.commas;
}

// ノードの class/id ペナルティ（ポジティブなクラスはボーナス）
static double classBias(lxb_dom_node_t* node) {
    static const char* POSITIVE[] = {"article", "body", "content", "entry", "hentry", "main",
                                     "page",    "post", "text",    "blog",  "story",  nullptr};
    static const char* NEGATIVE[] = {"hidden", "hid",   "ad-",   "ads",  "agegate",
                                     "popup",  "extra", "print", nullptr};
    size_t len = 0;
    const lxb_char_t* cls = getAttr(node, "class", &len);
    if (attrContainsAny(cls, len, POSITIVE))
        return 25.0;
    if (attrContainsAny(cls, len, NEGATIVE))
        return -25.0;
    const lxb_char_t* id = getAttr(node, "id", &len);
    if (attrContainsAny(id, len, POSITIVE))
        return 25.0;
    if (attrContainsAny(id, len, NEGATIVE))
        return -25.0;
    return 0.0;
}

// タグ別 base score
static double tagBaseScore(lxb_dom_node_t* node) {
    if (tagEq(node, "div"))
        return 5.0;
    if (tagEq(node, "blockquote"))
        return 3.0;
    if (tagEq(node, "pre"))
        return 3.0;
    if (tagEq(node, "td"))
        return 3.0;
    if (tagEq(node, "article"))
        return 10.0;
    if (tagEq(node, "section"))
        return 5.0;
    if (tagEq(node, "li"))
        return -3.0;
    if (tagEq(node, "address"))
        return -3.0;
    return 0.0;
}

// Candidates 配列から node を検索し見つかればスコアを加算、なければ追加
static void addScore(std::vector<Candidate>& cands, lxb_dom_node_t* node, double score) {
    if (!node)
        return;
    for (auto& c : cands) {
        if (c.node == node) {
            c.score += score;
            return;
        }
    }
    cands.push_back({node, score});
}

struct ScoreCtx {
    std::vector<Candidate>* cands;
};

static lexbor_action_t score_cb(lxb_dom_node_t* node, void* ctx) {
    auto* sc = static_cast<ScoreCtx*>(ctx);

    // テキスト系ブロック要素でスコア計算
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return LEXBOR_ACTION_OK;

    static const char* SCORE_TAGS[] = {"p", "pre", "td", nullptr};
    if (!tagIn(node, SCORE_TAGS))
        return LEXBOR_ACTION_OK;

    size_t total = 0, link = 0, commas = 0;
    getTextInfo(node, total, link, commas);
    if (total < 25)
        return LEXBOR_ACTION_OK; // 短すぎるノードは無視

    double lk_density = (double)link / (double)total;
    if (lk_density > 0.5)
        return LEXBOR_ACTION_OK; // リンクが半分超は無視

    // スコア = (文字数/100 の min 3) + (コンマ数) + 1
    double score = 1.0 + (double)commas + std::min((double)(total / 100), 3.0);

    // 親・祖父にスコア伝播
    lxb_dom_node_t* parent = lxb_dom_node_parent(node);
    lxb_dom_node_t* grand = parent ? lxb_dom_node_parent(parent) : nullptr;
    lxb_dom_node_t* great = grand ? lxb_dom_node_parent(grand) : nullptr;

    if (parent)
        addScore(*sc->cands, parent, score + tagBaseScore(parent) + classBias(parent));
    if (grand)
        addScore(*sc->cands, grand, score / 2.0);
    if (great)
        addScore(*sc->cands, great, score / 3.0);

    return LEXBOR_ACTION_OK;
}

// リンク密度（候補ノード全体のリンク率）
static double linkDensity(lxb_dom_node_t* node) {
    size_t total = 0, link = 0, commas = 0;
    getTextInfo(node, total, link, commas);
    return (total > 0) ? (double)link / (double)total : 0.0;
}

// 最高スコア候補を返す
static lxb_dom_node_t* findTopCandidate(lxb_dom_node_t* body, std::vector<Candidate>& cands,
                                        double& topScore) {
    // まず article/main タグ、または id/class が "article"/"main" の要素を優先
    // 例: Gigazine は <div id="article"> を使う
    struct PriCtx {
        lxb_dom_node_t* found;
    };
    PriCtx pc = {nullptr};
    lxb_dom_node_simple_walk(
        body,
        [](lxb_dom_node_t* n, void* ctx) -> lexbor_action_t {
            auto* pctx = static_cast<PriCtx*>(ctx);
            static const char* PTAGS[] = {"article", "main", nullptr};
            if (tagIn(n, PTAGS)) {
                pctx->found = n;
                return LEXBOR_ACTION_STOP;
            }
            // div/section の id が "article" または "main" に完全一致
            if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                size_t len = 0;
                const lxb_char_t* id = getAttr(n, "id", &len);
                if (id && len > 0) {
                    static const char* PRI_ID[] = {"article", "main", nullptr};
                    for (int i = 0; PRI_ID[i]; ++i) {
                        size_t klen = strlen(PRI_ID[i]);
                        if (len == klen && strncmp((const char*)id, PRI_ID[i], klen) == 0) {
                            pctx->found = n;
                            return LEXBOR_ACTION_STOP;
                        }
                    }
                }
            }
            return LEXBOR_ACTION_OK;
        },
        &pc);
    if (pc.found) {
        topScore = 0.0;
        return pc.found;
    }

    // スコアリング
    ScoreCtx sc = {&cands};
    lxb_dom_node_simple_walk(body, score_cb, &sc);

    // 候補を順次評価してリンク密度ペナルティを掛ける
    lxb_dom_node_t* top = nullptr;
    topScore = 0.0;
    for (auto& c : cands) {
        double ld = linkDensity(c.node);
        double adj = c.score * (1.0 - ld);
        if (adj > topScore) {
            topScore = adj;
            top = c.node;
        }
    }
    return top;
}

// 兄弟ノードのマージ（類似スコアの兄弟を本文候補に含める）
// topScore の 20% 以上か "article" 類似クラスを持つ兄弟を収集して
// 結果テキストとして結合できる vector に格納
static std::vector<lxb_dom_node_t*>
collectSiblings(lxb_dom_node_t* top, const std::vector<Candidate>& cands, double topScore) {
    std::vector<lxb_dom_node_t*> result;
    if (!top)
        return result;

    lxb_dom_node_t* parent = lxb_dom_node_parent(top);
    if (!parent) {
        result.push_back(top);
        return result;
    }

    double threshold = topScore * 0.2;

    lxb_dom_node_t* sibling = lxb_dom_node_first_child(parent);
    while (sibling) {
        if (sibling == top) {
            result.push_back(sibling);
        } else if (sibling->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            // スコアが閾値以上なら採用
            for (const auto& c : cands) {
                if (c.node == sibling && c.score >= threshold) {
                    result.push_back(sibling);
                    break;
                }
            }
            // <p> かつある程度テキストがあれば採用
            if (tagEq(sibling, "p")) {
                size_t total = 0, link = 0, commas = 0;
                getTextInfo(sibling, total, link, commas);
                double ld = (total > 0) ? (double)link / (double)total : 0.0;
                if (total >= 80 && ld < 0.25) {
                    bool already = false;
                    for (auto* r : result)
                        if (r == sibling) {
                            already = true;
                            break;
                        }
                    if (!already)
                        result.push_back(sibling);
                }
            }
        }
        sibling = lxb_dom_node_next(sibling);
    }
    if (result.empty())
        result.push_back(top);
    return result;
}

// =============================================================================
// Phase C: post-clean
// =============================================================================

static lexbor_action_t postClean_cb(lxb_dom_node_t* node, void* ctx) {
    auto* c = static_cast<CollectCtx*>(ctx);
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return LEXBOR_ACTION_OK;

    size_t len = 0;
    const lxb_char_t* cls = getAttr(node, "class", &len);
    if (attrContainsAny(cls, len, c->kw)) {
        c->list->push_back(node);
        return LEXBOR_ACTION_NEXT;
    }
    const lxb_char_t* id = getAttr(node, "id", &len);
    if (attrContainsAny(id, len, c->kw)) {
        c->list->push_back(node);
        return LEXBOR_ACTION_NEXT;
    }
    return LEXBOR_ACTION_OK;
}

static void postClean(lxb_dom_node_t* subtree) {
    std::vector<lxb_dom_node_t*> to_remove;
    CollectCtx ctx = {&to_remove, POST_NOISE_KW};
    lxb_dom_node_simple_walk(subtree, postClean_cb, &ctx);
    for (auto* n : to_remove)
        lxb_dom_node_remove(n);
}

// =============================================================================
// テキスト化
// =============================================================================

struct SerializeCtx {
    std::string* out;
    std::vector<std::string>* imageUrls; // nullable: 画像URL収集先
    const std::string* baseUrl;          // nullable: 相対URL解決用
};

static lexbor_action_t serialize_cb(lxb_dom_node_t* node, void* ctx) {
    auto* c = static_cast<SerializeCtx*>(ctx);
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        if (tagIn(node, SKIP_SUBTREE))
            return LEXBOR_ACTION_NEXT;
        // <img> は本文テキストを持たないが、画像URL収集の対象
        if (tagEq(node, "img")) {
            size_t alen = 0;
            const lxb_char_t* src = getAttr(node, "src", &alen);
            if (src && alen > 0) {
                std::string ref((const char*)src, alen);
                std::string abs = c->baseUrl ? resolveUrl(*c->baseUrl, ref)
                                             : (ref.compare(0, 7, "http://") == 0 ||
                                                        ref.compare(0, 8, "https://") == 0
                                                    ? ref
                                                    : std::string());
                if (!abs.empty()) {
                    bool dup = false;
                    if (c->imageUrls) {
                        for (const auto& u : *c->imageUrls)
                            if (u == abs) {
                                dup = true;
                                break;
                            }
                    }
                    if (!dup) {
                        bool withinCap = !c->imageUrls || c->imageUrls->size() < MAX_IMAGE_URLS;
                        if (withinCap) {
                            if (c->imageUrls)
                                c->imageUrls->push_back(abs);
                            // インライン位置にマーカーを埋め込む
                            *c->out += '\x01';
                            *c->out += abs;
                            *c->out += '\x01';
                        }
                    }
                }
            }
            return LEXBOR_ACTION_NEXT; // <img> に子テキストは無い
        }
        if (tagIn(node, BLOCK_TAGS)) {
            if (!c->out->empty() && c->out->back() != '\n')
                *c->out += '\n';
        }
        return LEXBOR_ACTION_OK;
    }
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t len = 0;
        lxb_char_t* txt = lxb_dom_node_text_content(node, &len);
        if (txt)
            c->out->append((char*)txt, len);
    }
    return LEXBOR_ACTION_OK;
}

static std::string serializeText(const std::vector<lxb_dom_node_t*>& nodes,
                                 const std::string* baseUrl,
                                 std::vector<std::string>* outImageUrls) {
    std::string result;
    result.reserve(8192);
    SerializeCtx c{&result, outImageUrls, baseUrl};
    for (auto* n : nodes) {
        lxb_dom_node_simple_walk(n, serialize_cb, &c);
        if (!result.empty() && result.back() != '\n')
            result += '\n';
    }
    return result;
}

// 連続改行を最大 2 に正規化
static std::string normalizeNewlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    int nl = 0;
    for (char c : s) {
        if (c == '\n') {
            if (++nl <= 2)
                out += c;
        } else {
            nl = 0;
            out += c;
        }
    }
    return out;
}

// =============================================================================
// ログ
// =============================================================================

static void logEntry(const char* url, const char* status, const char* winner, double score,
                     size_t len, u64 elapsed_ms) {
    if (!url || !*url)
        return;
    FILE* f = fopen("sdmc:/3ds/rssreader/extract_log.txt", "a");
    if (!f)
        return;

    time_t now = time(nullptr);
    const struct tm* ti = localtime(&now);
    char ts[32];
    if (!ti || strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", ti) == 0)
        snprintf(ts, sizeof(ts), "t=%ld", (long)now);

    fprintf(f, "[%s] %s\n", ts, url);
    if (winner && *winner && score > 0.0)
        fprintf(f, "  status:%s  winner:%s  score:%.1f  len:%lu  elapsed:%llums\n\n", status,
                winner, score, (unsigned long)len, (unsigned long long)elapsed_ms);
    else if (winner && *winner)
        fprintf(f, "  status:%s  winner:%s  len:%lu  elapsed:%llums\n\n", status, winner,
                (unsigned long)len, (unsigned long long)elapsed_ms);
    else
        fprintf(f, "  status:%s  len:%lu  elapsed:%llums\n\n", status, (unsigned long)len,
                (unsigned long long)elapsed_ms);
    fclose(f);
}

// =============================================================================
// 進捗通知ヘルパー
// =============================================================================

static inline void notify(ReadabilityProgressCb cb, void* user, int percent,
                          ReadabilityStage stage) {
    if (cb)
        cb(percent, stage, user);
}

// =============================================================================
// 公開関数
// =============================================================================

std::string extractContent(const std::string& html, const std::string& url,
                           ReadabilityProgressCb cb, void* cb_user,
                           std::vector<std::string>* outImageUrls) {
    u64 t_start = osGetTime();
    const char* logStatus = "OK";
    const char* logWinner = "body";
    double logScore = 0.0;

    // 1. 入力サイズ制限
    size_t parse_len = html.size() > MAX_HTML_BYTES ? MAX_HTML_BYTES : html.size();

    notify(cb, cb_user, 5, ReadabilityStage::Parse);

    lxb_html_document_t* doc = lxb_html_document_create();
    if (!doc) {
        std::string fb = stripHtml(html);
        logEntry(url.c_str(), "FALLBACK/doc_create", "", 0.0, fb.size(), osGetTime() - t_start);
        return fb;
    }

    lxb_status_t st = lxb_html_document_parse(doc, (const lxb_char_t*)html.c_str(), parse_len);
    if (st != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc);
        std::string fb = stripHtml(html);
        logEntry(url.c_str(), "FALLBACK/parse_err", "", 0.0, fb.size(), osGetTime() - t_start);
        return fb;
    }

    lxb_html_body_element_t* bodyEl = lxb_html_document_body_element(doc);
    if (!bodyEl) {
        lxb_html_document_destroy(doc);
        std::string fb = stripHtml(html);
        logEntry(url.c_str(), "FALLBACK/no_body", "", 0.0, fb.size(), osGetTime() - t_start);
        return fb;
    }
    lxb_dom_node_t* body = lxb_dom_interface_node(bodyEl);

    notify(cb, cb_user, 10, ReadabilityStage::Parse);

    // 2. Phase A: pre-clean
    preClean(body);
    notify(cb, cb_user, 30, ReadabilityStage::PreClean);

    // 3. Phase B: Readability スコアリング
    std::vector<Candidate> cands;
    cands.reserve(32);
    double topScore = 0.0;
    lxb_dom_node_t* top = findTopCandidate(body, cands, topScore);

    if (!top) {
        top = body;
        logWinner = "body";
    } else {
        // winner タグ名をログ用に記録
        if (tagEq(top, "article"))
            logWinner = "article";
        else if (tagEq(top, "main"))
            logWinner = "main";
        else if (tagEq(top, "div"))
            logWinner = "div";
        else if (tagEq(top, "section"))
            logWinner = "section";
        else
            logWinner = "other";
        logScore = topScore;
    }
    notify(cb, cb_user, 60, ReadabilityStage::Score);

    // 兄弟マージ（topScore が 0 の優先タグ選択時はスキップ）
    std::vector<lxb_dom_node_t*> selected;
    if (topScore > 0.0)
        selected = collectSiblings(top, cands, topScore);
    else
        selected.push_back(top);

    // 4. Phase C: post-clean（各採用ノードに適用）
    for (auto* n : selected)
        postClean(n);
    notify(cb, cb_user, 80, ReadabilityStage::PostClean);

    // 5. テキスト化（同時に画像URL収集）
    const std::string* basePtr = url.empty() ? nullptr : &url;
    std::string result = normalizeNewlines(serializeText(selected, basePtr, outImageUrls));

    lxb_html_document_destroy(doc);

    if (result.empty() || result.size() < 50) {
        std::string fb = stripHtml(html);
        logEntry(url.c_str(), "FALLBACK/empty", "", 0.0, fb.size(), osGetTime() - t_start);
        notify(cb, cb_user, 100, ReadabilityStage::Done);
        return fb;
    }

    logEntry(url.c_str(), logStatus, logWinner, logScore, result.size(), osGetTime() - t_start);
    notify(cb, cb_user, 100, ReadabilityStage::Done);
    return result;
}
