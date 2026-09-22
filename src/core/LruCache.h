#pragma once
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace reader {

// Thread-safe LRU used for rendered page tiles (§6: RenderKey-indexed cache).
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second.second);
        return it->second.first;
    }

    void put(const Key& key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.first = std::move(value);
            order_.splice(order_.begin(), order_, it->second.second);
            return;
        }
        if (map_.size() >= capacity_ && capacity_ > 0) {
            map_.erase(order_.back());
            order_.pop_back();
        }
        order_.push_front(key);
        map_.emplace(key, std::make_pair(std::move(value), order_.begin()));
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        order_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::list<Key> order_;
    std::unordered_map<Key, std::pair<Value, typename std::list<Key>::iterator>, Hash> map_;
};

} // namespace reader
