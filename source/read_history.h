#pragma once
#include <string>
#include <unordered_set>

class ReadHistory {
  public:
    void load();
    void save() const;

    void markRead(const std::string& key);
    bool isRead(const std::string& key) const;

    static std::string keyFor(const std::string& link, const std::string& title);

  private:
    std::unordered_set<std::string> read_;
};
