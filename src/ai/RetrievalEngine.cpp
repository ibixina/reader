#include "ai/RetrievalEngine.h"
#include <algorithm>

namespace reader {

void RetrievalEngine::index(const DocumentModel& model) {
    textIndex_.build(model);
    vectorIndex_.build(model);
    indexedDocument_ = model.document.id;
    indexedFileHash_ = model.document.fileHash;
    indexedBlocks_.clear();
    indexedBlocks_.reserve(model.blocks.size());
    for (const auto& block : model.blocks) indexedBlocks_.push_back(block.id);
}

void RetrievalEngine::setSemanticSnapshot(SemanticSearchSnapshot snapshot) {
    semantic_ = snapshot ? std::optional<SemanticSearchSnapshot>(std::move(snapshot))
                         : std::nullopt;
}

void RetrievalEngine::clearSemanticSnapshot() {
    semantic_.reset();
}

std::vector<RetrievedPassage> RetrievalEngine::retrieve(const DocumentModel& model,
                                                        const std::string& question,
                                                        const ReaderState& state,
                                                        const PaperAnalysis* analysis,
                                                        const Options& opts,
                                                        const CancellationToken& token) const {
    // Analysis summaries are not retrieval evidence until their schema carries
    // the exact source IDs used to derive each statement.
    (void)analysis;
    std::vector<RetrievedPassage> out;
    std::size_t used = 0;
    auto fits = [&](const std::string& t) {
        return t.size() <= opts.charBudget - std::min(used, opts.charBudget) &&
               out.size() < opts.maxPassages;
    };
    auto duplicateBlock = [&](const std::optional<BlockId>& block) {
        if (!block) return false;
        for (const auto& passage : out)
            if (passage.anchor.block == block) return true;
        return false;
    };

    // Current paragraph under viewport center (§17 implicit context).
    if (opts.includeImplicitContext && state.paragraphBlock) {
        if (const TextBlock* b = model.findBlock(*state.paragraphBlock)) {
            if (fits(b->text)) {
                RetrievedPassage p;
                p.anchor = anchorForBlock(model, *b);
                p.text = b->text;
                p.score = 2.0;
                used += p.text.size();
                out.push_back(std::move(p));
            }
        }
    }
    // Current section.
    if (opts.includeImplicitContext && state.section) {
        if (const Section* s = model.findSection(*state.section)) {
            std::string t = model.sectionText(s->id);
            if (t.size() > 1500) t.resize(1500);
            if (!t.empty() && fits(t)) {
                RetrievedPassage p;
                p.anchor = anchorForSection(model, *s);
                p.text = t;
                p.score = 1.5;
                used += p.text.size();
                out.push_back(std::move(p));
            }
        }
    }
    const bool indexMatches = indexedDocument_ == model.document.id &&
                              indexedFileHash_ == model.document.fileHash &&
                              indexedBlocks_.size() == model.blocks.size() &&
                              std::equal(indexedBlocks_.begin(), indexedBlocks_.end(),
                                         model.blocks.begin(),
                                         [](const BlockId& id, const TextBlock& block) {
                                             return id == block.id;
                                         });
    // Literal retrieval is deterministic and useful for exact paper terms;
    // hashed semantic retrieval follows it under the same context budget.
    if (indexMatches) {
        for (const auto& h : textIndex_.search(model, question, opts.maxPassages)) {
            if (duplicateBlock(h.anchor.block) || !fits(h.snippet)) continue;
            RetrievedPassage p;
            p.anchor = h.anchor;
            p.text = h.snippet;
            p.score = 1.0;
            used += p.text.size();
            out.push_back(std::move(p));
        }
    }
    // Explicitly configured semantic retrieval runs only when the caller has
    // installed an immutable snapshot. Provider errors and stale snapshots
    // fall through to the deterministic lexical index.
    if (indexMatches && semantic_ && *semantic_ && !token.cancelled()) {
        std::string semanticError;
        for (const auto& h : semantic_->index.querySemantic(
                 model, question, *semantic_->provider, opts.maxPassages, token,
                 &semanticError)) {
            if (duplicateBlock(h.anchor.block) || !fits(h.snippet)) continue;
            RetrievedPassage p;
            p.anchor = h.anchor;
            p.text = h.snippet;
            p.score = h.score + 1.0;
            used += p.text.size();
            out.push_back(std::move(p));
        }
    }
    // Deterministic hashed retrieval is the offline fallback.
    if (indexMatches) for (const auto& h : vectorIndex_.query(model, question, opts.maxPassages)) {
        if (duplicateBlock(h.anchor.block) || !fits(h.snippet)) continue;
        RetrievedPassage p;
        p.anchor = h.anchor;
        p.text = h.snippet;
        p.score = h.score;
        used += p.text.size();
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace reader
