#include "search/TextIndex.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace reader {

std::vector<std::string> TextIndex::tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void TextIndex::build(const DocumentModel& model) {
    blockLower_.clear();
    postings_.clear();
    blockLower_.reserve(model.blocks.size());
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        blockLower_.push_back(toLower(model.blocks[i].text));
        std::unordered_map<std::string, int> counts;
        for (auto& t : tokens(model.blocks[i].text)) counts[t]++;
        for (auto& [tok, n] : counts) postings_[tok].push_back({i, n});
    }
}

std::vector<TextIndex::Hit> TextIndex::search(const DocumentModel& model, const std::string& query,
                                              std::size_t limit) const {
    std::vector<Hit> out;
    std::string q = toLower(trim(query));
    if (q.empty()) return out;
    auto qtoks = tokens(q);

    std::unordered_map<std::size_t, int> scores;
    for (auto& t : qtoks) {
        auto it = postings_.find(t);
        if (it == postings_.end()) continue;
        for (auto& p : it->second) scores[p.blockIdx] += p.count;
    }
    // Substring fallback: rare/compound terms missing from postings.
    if (scores.empty()) {
        for (std::size_t i = 0; i < blockLower_.size(); ++i)
            if (blockLower_[i].find(q) != std::string::npos) scores[i] = 1;
    }
    std::vector<std::pair<std::size_t, int>> ranked(scores.begin(), scores.end());
    std::sort(ranked.begin(), ranked.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < std::min(limit, ranked.size()); ++i) {
        const auto& b = model.blocks[ranked[i].first];
        Hit h;
        h.anchor = anchorForBlock(model, b);
        std::size_t pos = blockLower_[ranked[i].first].find(qtoks.empty() ? q : qtoks.front());
        std::size_t from = pos == std::string::npos ? 0 : (pos > 40 ? pos - 40 : 0);
        h.snippet = b.text.substr(0, 0) + "..." + b.text.substr(from, 160) + "...";
        out.push_back(std::move(h));
    }
    return out;
}

} // namespace reader
