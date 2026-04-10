#include "rss.h"
#include "tinyxml2.h"
#include <cstring>

using namespace tinyxml2;

static std::string elemText(XMLElement* parent, const char* name) {
    XMLElement* el = parent->FirstChildElement(name);
    if (!el) return {};
    const char* text = el->GetText();
    return text ? text : "";
}

// Atom の <link href="..."> を取得
static std::string atomLinkHref(XMLElement* entry) {
    for (XMLElement* el = entry->FirstChildElement("link"); el;
         el = el->NextSiblingElement("link")) {
        const char* rel  = el->Attribute("rel");
        const char* href = el->Attribute("href");
        if (!href) continue;
        // rel がない or "alternate" → 記事URL
        if (!rel || strcmp(rel, "alternate") == 0) return href;
    }
    return {};
}

static Feed parseRss2(XMLElement* root) {
    Feed feed;
    XMLElement* channel = root->FirstChildElement("channel");
    if (!channel) return feed;

    feed.title = elemText(channel, "title");

    for (XMLElement* item = channel->FirstChildElement("item"); item;
         item = item->NextSiblingElement("item")) {
        Article art;
        art.title   = elemText(item, "title");
        art.link    = elemText(item, "link");
        art.pubDate = elemText(item, "pubDate");

        // content:encoded があれば優先、なければ description
        XMLElement* ce = item->FirstChildElement("content:encoded");
        if (ce && ce->GetText()) {
            art.content = ce->GetText();
        } else {
            art.content = elemText(item, "description");
        }

        feed.articles.push_back(std::move(art));
    }
    return feed;
}

static Feed parseAtom(XMLElement* root) {
    Feed feed;
    feed.title = elemText(root, "title");

    for (XMLElement* entry = root->FirstChildElement("entry"); entry;
         entry = entry->NextSiblingElement("entry")) {
        Article art;
        art.title   = elemText(entry, "title");
        art.link    = atomLinkHref(entry);
        art.pubDate = elemText(entry, "published");
        if (art.pubDate.empty()) art.pubDate = elemText(entry, "updated");

        XMLElement* content = entry->FirstChildElement("content");
        if (content && content->GetText()) {
            art.content = content->GetText();
        } else {
            art.content = elemText(entry, "summary");
        }

        feed.articles.push_back(std::move(art));
    }
    return feed;
}

// RSS 1.0 (RDF): <rdf:RDF> の下に <channel> と複数の <item> が並ぶ
static Feed parseRss1(XMLElement* root) {
    Feed feed;

    // チャンネルタイトル
    XMLElement* channel = root->FirstChildElement("channel");
    if (channel) feed.title = elemText(channel, "title");

    // item は rdf:RDF の直下に並ぶ
    for (XMLElement* item = root->FirstChildElement("item"); item;
         item = item->NextSiblingElement("item")) {
        Article art;
        art.title   = elemText(item, "title");
        art.link    = elemText(item, "link");
        art.content = elemText(item, "description");
        feed.articles.push_back(std::move(art));
    }
    return feed;
}

Feed parseFeed(const std::string& xml, std::string& errMsg) {
    XMLDocument doc;
    XMLError err = doc.Parse(xml.c_str(), xml.size());
    if (err != XML_SUCCESS) {
        errMsg = doc.ErrorStr();
        return {};
    }

    XMLElement* root = doc.RootElement();
    if (!root) {
        errMsg = "No root element";
        return {};
    }

    const char* name = root->Name();
    if (strcmp(name, "rss") == 0)       return parseRss2(root);
    if (strcmp(name, "feed") == 0)      return parseAtom(root);
    // RSS 1.0: ルート要素名は "rdf:RDF" だがtinyxml2は "RDF" と読む場合もある
    if (strstr(name, "RDF") != nullptr) return parseRss1(root);

    errMsg = std::string("Unknown root element: ") + name;
    return {};
}
