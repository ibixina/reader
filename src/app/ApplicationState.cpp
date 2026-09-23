#include "app/ApplicationState.h"

namespace reader {

void NavigationHistory::visit(NavEntry entry) {
    back_.push_back(std::move(entry));
    forward_.clear();
    if (back_.size() > 200) back_.erase(back_.begin());
}

NavEntry NavigationHistory::back() {
    if (back_.size() > 1) {
        forward_.push_back(back_.back());
        back_.pop_back();
    }
    return current();
}

NavEntry NavigationHistory::forward() {
    if (!forward_.empty()) {
        back_.push_back(forward_.back());
        forward_.pop_back();
    }
    return current();
}

} // namespace reader
