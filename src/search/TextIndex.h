#pragma once
#include "core/Types.h"
#include "document/DocumentAnchor.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace reader {

// Fast local literal index (§40.1): token postings + substring fallback.
class TextIndex {
public:
    void build(const DocumentModel& model);
    struct Hit {
        DocumentAnchor anchor;
        std::string snippet;
    };
    std::vector<Hit> search(const DocumentModel& model, const std::string& query,
                            std::size_t limit = 20) const;

private:
    struct Posting {
        std::size_t blockIdx;
        int count = 0;
    };
    std::vector<std::string> blockLower_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;

    static std::vector<std::string> tokens(const std::string& s);
};

} // namespace reader
