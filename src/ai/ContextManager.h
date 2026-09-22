#pragma once
#include "ai/References.h"
#include "document/DocumentModel.h"
#include <string>

namespace reader {

// Deterministic composer context (§47). No LLM decides what the selection
// means; priority order follows §17 exactly.
class ContextManager {
public:
    void setCurrentSelection(ContextReference ref);
    void clearCurrentSelection();

    void pinReference(const ReferenceId& id);
    void unpinReference(const ReferenceId& id);
    void removeReference(const ReferenceId& id);

    ComposerContext currentContext() const { return context_; }

    ChatRequest buildRequest(const std::string& question, const ReaderState& readerState,
                             const PaperMetadata& paper,
                             std::vector<RetrievedPassage> retrieved,
                             std::vector<ChatMessage> recent) const;

    // §20 shorthands: @selection @page @section @eq4 @fig2 @table1
    // @methods @results. Resolves against the live model + context.
    std::optional<ContextReference> resolveShorthand(const std::string& token,
                                                    const DocumentModel& model,
                                                    const ReaderState& state) const;

private:
    ReferenceId nextReferenceId() const;
    ComposerContext context_;
    mutable int counter_ = 0;
};

} // namespace reader
