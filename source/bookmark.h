#pragma once
#include <string>
#include <vector>

struct Bookmark {
    std::string title;
    std::string link;
    std::string feedTitle;
};

class BookmarkStore {
  public:
    void load();
    void save() const;

    void toggle(const std::string& title, const std::string& link, const std::string& feedTitle);
    bool isBookmarked(const std::string& link, const std::string& title) const;

    const std::vector<Bookmark>& getAll() const { return bookmarks_; }

    static std::string keyFor(const std::string& link, const std::string& title);

  private:
    std::vector<Bookmark> bookmarks_;
};
