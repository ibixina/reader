#pragma once
#include "analysis/PaperAnalysis.h"
#include "core/CancellationToken.h"
#include "document/DocumentModel.h"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace reader {

// Explicit whole-paper AI analysis (§5). Opening a paper never triggers
// network; only Ingest does. States drive the toolbar button (§5.2).
enum class IngestState {
    NotIngested,
    Preparing,
    Uploading,
    Analyzing,
    Applying,
    Ingested,
    Failed
};

struct IngestProgress {
    IngestState state = IngestState::NotIngested;
    float progress = 0;
    std::string statusText;
};

inline std::string ingestStateLabel(IngestState s) {
    switch (s) {
        case IngestState::NotIngested: return "Ingest";
        case IngestState::Preparing: return "Preparing document…";
        case IngestState::Uploading: return "Sending paper…";
        case IngestState::Analyzing: return "Analyzing paper…";
        case IngestState::Applying: return "Applying annotations…";
        case IngestState::Ingested: return "✓ Ingested";
        case IngestState::Failed: return "Retry Ingest";
    }
    return "Ingest";
}

// Cache root: ~/.local/share/paper-reader/papers/<sha256>/ (§5.10, §57).
std::string cacheDirFor(const std::string& fileHash);

class PaperIngestor {
public:
    using ProgressCallback = std::function<void(const IngestProgress&)>;
    using FetchAnalysis = std::function<std::string(const std::string& prompt)>;

    // Offline deterministic analysis: extractive summaries, keyword
    // concepts, importance scoring. No network, always available.
    PaperAnalysis analyzeLocal(const DocumentModel& model, CancellationToken token = {});

    // Full flow: local prep -> optional remote LLM JSON -> validate ->
    // cache -> return. Remote step is skipped when no fetcher configured
    // (privacy default: no complete-document upload, §59).
    std::optional<PaperAnalysis> ingest(const DocumentModel& model,
                                        FetchAnalysis remote,
                                        ProgressCallback progress,
                                        CancellationToken token = {});

    std::optional<PaperAnalysis> cachedAnalysis(const DocumentId& document,
                                                const std::string& fileHash) const;
    void clearAnalysis(const std::string& fileHash) const;
    bool saveAnalysis(const std::string& fileHash, const PaperAnalysis& analysis) const;
    // Drops dangling and ungrounded model output before any UI/cache publish.
    bool normalizeAnalysis(PaperAnalysis& analysis, const DocumentModel& model,
                           std::string* error = nullptr) const;
    // Raw chat-model response text (what the Ingest Raw tab shows).
    bool saveRawResponse(const std::string& fileHash, const std::string& text) const;
    std::string loadRawResponse(const std::string& fileHash) const;
    std::string buildWholePaperPrompt(const DocumentModel& model) const;
    // Compact companion for the attached-PDF flow: the model reads the paper
    // content from the file, this message only carries the stable ID map it
    // must cite (blocks/sections/figures/tables/equations/citations). Short
    // per-block locators let it map PDF passages back to block IDs for
    // annotation offsets; anything not citing these IDs is dropped by
    // normalizeAnalysis, so a bare file upload without this map is useless.
    std::string buildGroundedSkeletonPrompt(const DocumentModel& model) const;

private:
    bool saveCache(const std::string& fileHash, const PaperAnalysis& analysis) const;
    void clearRawResponse(const std::string& fileHash) const;
};

} // namespace reader
