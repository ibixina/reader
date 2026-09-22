#include "search/VectorIndex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_map>

namespace reader {

bool EmbeddingProvider::embedBatch(const std::vector<std::string>& texts,
                                   std::vector<std::vector<float>>& outputs,
                                   const CancellationToken& token, std::string& error) const {
    outputs.clear();
    outputs.reserve(texts.size());
    for (const auto& text : texts) {
        if (token.cancelled()) {
            error = "embedding cancelled";
            return false;
        }
        std::vector<float> output;
        if (!embed(text, output, token, error)) return false;
        outputs.push_back(std::move(output));
    }
    return true;
}

std::vector<std::string> VectorIndex::tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!cur.empty()) {
            if (cur.size() > 2) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() > 2) out.push_back(cur);
    return out;
}

void VectorIndex::build(const DocumentModel& model) {
    semanticVectors_.clear();
    semanticReady_ = false;
    semanticFileHash_.clear();
    semanticDocumentId_.clear();
    semanticModelId_.clear();
    semanticDimension_ = 0;
    semanticBlockHashes_.clear();
    vectors_.clear();
    idf_.assign(kDim, 0);
    std::vector<std::unordered_map<std::size_t, int>> docTerms(model.blocks.size());
    std::vector<int> df(kDim, 0);
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        for (auto& t : tokens(model.blocks[i].text)) {
            std::size_t h = std::hash<std::string>{}(t) % kDim;
            docTerms[i][h]++;
        }
        for (auto& [h, _] : docTerms[i]) df[h]++;
    }
    double n = static_cast<double>(std::max<std::size_t>(1, model.blocks.size()));
    for (std::size_t h = 0; h < kDim; ++h) idf_[h] = std::log((n + 1) / (df[h] + 1)) + 1.0;
    vectors_.reserve(model.blocks.size());
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        std::vector<float> v(kDim, 0);
        for (auto& [h, c] : docTerms[i]) v[h] = static_cast<float>(c) * static_cast<float>(idf_[h]);
        double norm = 0;
        for (float x : v) norm += x * x;
        norm = std::sqrt(norm);
        if (norm > 0)
            for (float& x : v) x = static_cast<float>(x / norm);
        vectors_.push_back(std::move(v));
    }
}

bool VectorIndex::validVector(const std::vector<float>& vector) {
    if (vector.empty()) return false;
    double norm = 0;
    for (const float value : vector) {
        if (!std::isfinite(value)) return false;
        norm += static_cast<double>(value) * value;
    }
    return std::isfinite(norm) && norm > std::numeric_limits<double>::epsilon();
}

std::string VectorIndex::cachePath(const DocumentModel& model, const std::string& fileHash,
                                   const EmbeddingProvider& provider) {
    const auto key = sha256Hex(model.document.id + "\n" + fileHash + "\n" +
                               provider.modelId() + "\n" +
                               std::to_string(provider.dimension()));
    const char* home = std::getenv("HOME");
    const std::filesystem::path root = home && *home ? home : ".";
    return (root / ".local" / "share" / "paper-reader" / "embeddings" / (key + ".bin"))
        .string();
}

template <typename T>
bool readPod(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(input);
}

bool readString(std::istream& input, std::string& value) {
    std::uint64_t size = 0;
    if (!readPod(input, size) || size > 64 * 1024 * 1024) return false;
    value.resize(static_cast<std::size_t>(size));
    input.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(input);
}

template <typename T>
bool writePod(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(output);
}

bool writeString(std::ostream& output, const std::string& value) {
    const auto size = static_cast<std::uint64_t>(value.size());
    return writePod(output, size) &&
           static_cast<bool>(output.write(value.data(), static_cast<std::streamsize>(size)));
}

bool VectorIndex::saveSemanticCache(const DocumentModel& model, const std::string& fileHash,
                                    const EmbeddingProvider& provider, std::string* error) const {
    const std::filesystem::path path = cachePath(model, fileHash, provider);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create embedding cache directory: " + ec.message();
        return false;
    }
    const auto temporary = path.string() + ".tmp." + sha256Hex(std::to_string(nowMs())).substr(0, 16);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const std::uint32_t magic = 0x52454931; // REI1
    const std::uint32_t version = 1;
    const auto dimension = static_cast<std::uint64_t>(provider.dimension());
    const auto count = static_cast<std::uint64_t>(semanticVectors_.size());
    bool ok = output && writePod(output, magic) && writePod(output, version) &&
              writePod(output, dimension) && writePod(output, count) &&
              writeString(output, fileHash) && writeString(output, provider.modelId());
    for (std::size_t i = 0; ok && i < semanticVectors_.size(); ++i) {
        ok = writeString(output, model.blocks[i].id) &&
             writeString(output, sha256Hex(model.blocks[i].text));
        if (ok)
            output.write(reinterpret_cast<const char*>(semanticVectors_[i].data()),
                         static_cast<std::streamsize>(semanticVectors_[i].size() * sizeof(float)));
        ok = ok && static_cast<bool>(output);
    }
    output.close();
    if (!ok || std::rename(temporary.c_str(), path.string().c_str()) != 0) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = "could not write embedding cache";
        return false;
    }
    return true;
}

bool VectorIndex::loadSemanticCache(const DocumentModel& model, const std::string& fileHash,
                                    const EmbeddingProvider& provider, std::string* error) {
    semanticVectors_.clear();
    semanticReady_ = false;
    const std::filesystem::path path = cachePath(model, fileHash, provider);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "embedding cache miss";
        return false;
    }
    std::uint32_t magic = 0, version = 0;
    std::uint64_t dimension = 0, count = 0;
    std::string storedHash, storedModel;
    if (!readPod(input, magic) || !readPod(input, version) || !readPod(input, dimension) ||
        !readPod(input, count) || !readString(input, storedHash) || !readString(input, storedModel) ||
        magic != 0x52454931 || version != 1 || dimension != provider.dimension() ||
        count != model.blocks.size() || storedHash != fileHash || storedModel != provider.modelId() ||
        dimension == 0 || dimension > 1'000'000) {
        if (error) *error = "embedding cache metadata mismatch";
        return false;
    }
    semanticVectors_.reserve(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        std::string blockId, textHash;
        if (!readString(input, blockId) || !readString(input, textHash) ||
            blockId != model.blocks[i].id || textHash != sha256Hex(model.blocks[i].text)) {
            semanticVectors_.clear();
            if (error) *error = "embedding cache document mismatch";
            return false;
        }
        std::vector<float> vector(static_cast<std::size_t>(dimension));
        input.read(reinterpret_cast<char*>(vector.data()),
                   static_cast<std::streamsize>(vector.size() * sizeof(float)));
        if (!input || !validVector(vector)) {
            semanticVectors_.clear();
            if (error) *error = "embedding cache contains invalid vector";
            return false;
        }
        semanticVectors_.push_back(std::move(vector));
    }
    semanticFileHash_ = fileHash;
    semanticDocumentId_ = model.document.id;
    semanticModelId_ = provider.modelId();
    semanticDimension_ = static_cast<std::size_t>(dimension);
    semanticBlockHashes_.clear();
    for (const auto& block : model.blocks) semanticBlockHashes_.push_back(sha256Hex(block.text));
    semanticReady_ = true;
    return true;
}

bool VectorIndex::buildSemantic(const DocumentModel& model, const EmbeddingProvider& provider,
                                const CancellationToken& token, std::size_t maxBlocks,
                                std::string* error, bool persist) {
    auto previousVectors = std::move(semanticVectors_);
    const auto previousHash = semanticFileHash_;
    const auto previousDocument = semanticDocumentId_;
    const auto previousModel = semanticModelId_;
    const auto previousDimension = semanticDimension_;
    const bool previousReady = semanticReady_;
    auto previousBlockHashes = std::move(semanticBlockHashes_);
    const auto restorePrevious = [&] {
        semanticVectors_ = std::move(previousVectors);
        semanticFileHash_ = previousHash;
        semanticDocumentId_ = previousDocument;
        semanticModelId_ = previousModel;
        semanticDimension_ = previousDimension;
        semanticReady_ = previousReady;
        semanticBlockHashes_ = std::move(previousBlockHashes);
    };
    semanticVectors_.clear();
    semanticReady_ = false;
    if (provider.dimension() == 0 || provider.dimension() > 1'000'000) {
        if (error) *error = "embedding provider dimension is invalid";
        restorePrevious();
        return false;
    }
    if (maxBlocks != 0 && model.blocks.size() > maxBlocks) {
        if (error) *error = "embedding budget exceeded";
        restorePrevious();
        return false;
    }
    if (token.cancelled()) {
        if (error) *error = "embedding cancelled";
        restorePrevious();
        return false;
    }
    if (persist && !model.document.fileHash.empty() &&
        loadSemanticCache(model, model.document.fileHash, provider, nullptr))
        return true;
    std::vector<std::vector<float>> outputs;
    std::string providerError;
    constexpr std::size_t kBatchItems = 32;
    constexpr std::size_t kBatchCharacters = 120000;
    for (std::size_t start = 0; start < model.blocks.size();) {
        if (token.cancelled()) {
            restorePrevious();
            if (error) *error = "embedding cancelled";
            return false;
        }
        std::vector<std::string> texts;
        std::size_t characters = 0;
        std::size_t end = start;
        while (end < model.blocks.size() && end - start < kBatchItems &&
               (end == start || characters + model.blocks[end].text.size() <= kBatchCharacters)) {
            texts.push_back(model.blocks[end].text);
            characters += model.blocks[end].text.size();
            ++end;
        }
        std::vector<std::vector<float>> batch;
        if (!provider.embedBatch(texts, batch, token, providerError) ||
            batch.size() != texts.size()) {
            restorePrevious();
            if (error) *error = providerError.empty()
                                      ? "embedding provider returned the wrong batch size"
                                      : providerError;
            return false;
        }
        outputs.insert(outputs.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        start = end;
    }
    const std::size_t dimension = provider.dimension();
    semanticVectors_.reserve(outputs.size());
    for (auto vector : outputs) {
        if (vector.size() != dimension || !validVector(vector)) {
            restorePrevious();
            if (error) *error = "embedding provider returned invalid vector";
            return false;
        }
        double norm = 0;
        for (const float value : vector) norm += static_cast<double>(value) * value;
        norm = std::sqrt(norm);
        for (float& value : vector) value = static_cast<float>(value / norm);
        semanticVectors_.push_back(std::move(vector));
    }
    semanticFileHash_ = model.document.fileHash;
    semanticDocumentId_ = model.document.id;
    semanticModelId_ = provider.modelId();
    semanticDimension_ = dimension;
    semanticBlockHashes_.clear();
    for (const auto& block : model.blocks) semanticBlockHashes_.push_back(sha256Hex(block.text));
    semanticReady_ = true;
    if (token.cancelled()) {
        restorePrevious();
        if (error) *error = "embedding cancelled";
        return false;
    }
    if (persist && !model.document.fileHash.empty() &&
        !saveSemanticCache(model, model.document.fileHash, provider, error)) {
        restorePrevious();
        return false;
    }
    return true;
}

std::vector<float> VectorIndex::embed(const std::string& text) const {
    std::vector<float> v(kDim, 0);
    std::unordered_map<std::size_t, int> counts;
    for (auto& t : tokens(text)) counts[std::hash<std::string>{}(t) % kDim]++;
    for (auto& [h, c] : counts) v[h] = static_cast<float>(c) * static_cast<float>(idf_[h]);
    double norm = 0;
    for (float x : v) norm += x * x;
    norm = std::sqrt(norm);
    if (norm > 0)
        for (float& x : v) x = static_cast<float>(x / norm);
    return v;
}

std::vector<VectorIndex::Hit> VectorIndex::query(const DocumentModel& model, const std::string& text,
                                                 std::size_t k) const {
    if (vectors_.empty() || trim(text).empty()) return {};
    return queryVectors(model, embed(text), k, false);
}

std::vector<VectorIndex::Hit> VectorIndex::querySemantic(
    const DocumentModel& model, const std::string& text, const EmbeddingProvider& provider,
    std::size_t k, const CancellationToken& token, std::string* error) const {
    if (!semanticReady_ || semanticVectors_.size() != model.blocks.size() ||
        semanticBlockHashes_.size() != model.blocks.size() ||
        semanticDocumentId_ != model.document.id ||
        semanticFileHash_ != model.document.fileHash ||
        semanticDimension_ != provider.dimension() || semanticModelId_ != provider.modelId()) {
        if (error) *error = "semantic index is not ready for this provider";
        return {};
    }
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        if (semanticBlockHashes_[i] != sha256Hex(model.blocks[i].text)) {
            if (error) *error = "semantic index is stale for this document";
            return {};
        }
    }
    std::vector<float> q;
    std::string providerError;
    if (token.cancelled()) {
        if (error) *error = "embedding cancelled";
        return {};
    }
    if (!provider.embed(text, q, token, providerError) ||
        q.size() != provider.dimension() || !validVector(q)) {
        if (error) *error = providerError.empty() ? "query embedding is invalid" : providerError;
        return {};
    }
    double norm = 0;
    for (const float value : q) norm += static_cast<double>(value) * value;
    norm = std::sqrt(norm);
    for (float& value : q) value = static_cast<float>(value / norm);
    return queryVectors(model, q, k, true);
}

std::vector<VectorIndex::Hit> VectorIndex::queryVectors(const DocumentModel& model,
                                                         const std::vector<float>& q,
                                                         std::size_t k, bool semantic) const {
    std::vector<Hit> out;
    const auto& vectors = semantic ? semanticVectors_ : vectors_;
    std::vector<std::pair<std::size_t, double>> scored;
    scored.reserve(vectors.size());
    for (std::size_t i = 0; i < vectors.size() && i < model.blocks.size(); ++i) {
        double dot = 0;
        const std::size_t dimension = std::min(q.size(), vectors[i].size());
        for (std::size_t h = 0; h < dimension; ++h) dot += q[h] * vectors[i][h];
        if (dot > 1e-6) scored.emplace_back(i, dot);
    }
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < std::min(k, scored.size()); ++i) {
        const auto& b = model.blocks[scored[i].first];
        Hit h;
        h.anchor = anchorForBlock(model, b);
        h.score = scored[i].second;
        h.snippet = b.text.substr(0, 200);
        out.push_back(std::move(h));
    }
    return out;
}

} // namespace reader
