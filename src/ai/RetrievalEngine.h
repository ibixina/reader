#pragma once
#include "ai/References.h"
#include "analysis/PaperAnalysis.h"
#include "document/DocumentModel.h"
#include "search/TextIndex.h"
#include "search/VectorIndex.h"
#include <cstddef>
#include <optional>
#include <vector>

namespace reader {

// Builds the context hierarchy for a question (§45):
// pinned > selection > paragraph > section > semantic retrieval > metadata.
// Only as much context as needed, under a token-ish char budget.
struct RetrievalOptions {
    std::size_t charBudget = 12000;
    std::size_t maxPassages = 5;
    bool allowFullUpload = false; // privacy default off (§59)
    // The caller may request only explicitly pinned/selected context.  This
    // keeps the privacy setting separate from the retrieval ranking itself.
    bool includeImplicitContext = true;
};

class RetrievalEngine {
public:
    using Options = RetrievalOptions;
    void index(const DocumentModel& model);
    void setSemanticSnapshot(SemanticSearchSnapshot snapshot);
    void clearSemanticSnapshot();

    std::vector<RetrievedPassage> retrieve(const DocumentModel& model,
                                           const std::string& question,
                                           const ReaderState& state,
                                           const PaperAnalysis* analysis,
                                           const Options& opts = Options{},
                                           const CancellationToken& token = {}) const;

private:
    TextIndex textIndex_;
    VectorIndex vectorIndex_;
    DocumentId indexedDocument_;
    std::string indexedFileHash_;
    std::vector<BlockId> indexedBlocks_;
    std::optional<SemanticSearchSnapshot> semantic_;
};

} // namespace reader
