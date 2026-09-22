#pragma once
#include "core/CancellationToken.h"
#include "core/Types.h"
#include "document/DocumentAnchor.h"
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace reader {

// A provider boundary for genuine semantic embeddings. Providers may be local
// or explicitly configured compatible endpoints; this core type performs no
// network access itself.
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    virtual std::size_t dimension() const = 0;
    virtual std::string modelId() const = 0;
    virtual bool embed(const std::string& text, std::vector<float>& output,
                       const CancellationToken& token, std::string& error) const = 0;
    virtual bool embedBatch(const std::vector<std::string>& texts,
                            std::vector<std::vector<float>>& outputs,
                            const CancellationToken& token, std::string& error) const;
};

// Search index with a deterministic lexical fallback and an injectable,
// persistable semantic path. `build` remains the offline lexical path.
class VectorIndex {
public:
    static constexpr std::size_t kDim = 512;

    void build(const DocumentModel& model);
    bool buildSemantic(const DocumentModel& model, const EmbeddingProvider& provider,
                       const CancellationToken& token = {}, std::size_t maxBlocks = 0,
                       std::string* error = nullptr, bool persist = true);
    bool loadSemanticCache(const DocumentModel& model, const std::string& fileHash,
                           const EmbeddingProvider& provider, std::string* error = nullptr);
    bool semanticReady() const { return semanticReady_; }
    struct Hit {
        DocumentAnchor anchor;
        double score = 0;
        std::string snippet;
    };
    std::vector<Hit> query(const DocumentModel& model, const std::string& text,
                           std::size_t k = 5) const;
    std::vector<Hit> querySemantic(const DocumentModel& model, const std::string& text,
                                   const EmbeddingProvider& provider, std::size_t k = 5,
                                   const CancellationToken& token = {},
                                   std::string* error = nullptr) const;

private:
    std::vector<std::vector<float>> vectors_;
    std::vector<double> idf_;
    std::vector<std::vector<float>> semanticVectors_;
    std::string semanticFileHash_;
    std::string semanticDocumentId_;
    std::string semanticModelId_;
    std::size_t semanticDimension_ = 0;
    bool semanticReady_ = false;
    std::vector<std::string> semanticBlockHashes_;

    static std::vector<std::string> tokens(const std::string& s);
    std::vector<float> embed(const std::string& text) const;
    static bool validVector(const std::vector<float>& vector);
    static std::string cachePath(const DocumentModel& model, const std::string& fileHash,
                                 const EmbeddingProvider& provider);
    bool saveSemanticCache(const DocumentModel& model, const std::string& fileHash,
                           const EmbeddingProvider& provider, std::string* error) const;
    std::vector<Hit> queryVectors(const DocumentModel& model,
                                  const std::vector<float>& query, std::size_t k,
                                  bool semantic) const;
};

// Immutable value snapshot handed from an explicitly configured UI index to
// a background retrieval job. Copying it never starts provider or disk work.
struct SemanticSearchSnapshot {
    VectorIndex index;
    std::shared_ptr<const EmbeddingProvider> provider;

    explicit operator bool() const {
        return provider && index.semanticReady();
    }
};

} // namespace reader
