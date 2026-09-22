#pragma once
#include "search/VectorIndex.h"

namespace reader {

// Explicit OpenAI-compatible embeddings adapter. It is UI/provider-layer code
// and is never constructed automatically by the core or on document open.
struct EmbeddingProviderQtConfig {
    std::string baseUrl;
    std::string apiKey;
    std::string model;
    std::size_t dimension = 0;
    int timeoutMs = 120000;
};

class EmbeddingProviderQt final : public EmbeddingProvider {
public:
    explicit EmbeddingProviderQt(EmbeddingProviderQtConfig config);

    std::size_t dimension() const override { return config_.dimension; }
    std::string modelId() const override;
    bool embed(const std::string& text, std::vector<float>& output,
               const CancellationToken& token, std::string& error) const override;
    bool embedBatch(const std::vector<std::string>& texts,
                    std::vector<std::vector<float>>& outputs,
                    const CancellationToken& token, std::string& error) const override;

private:
    EmbeddingProviderQtConfig config_;
};

} // namespace reader
