#pragma once
#include "ai/LlmProvider.h"
#include <atomic>
#include <cstdint>
#include <QJsonObject>
#include <QObject>

namespace reader {

// OpenAI-compatible streaming provider (works for OpenAI, Gemini
// OpenAI-endpoint, and any OpenAICompatible server, §48). SSE tokens are
// forwarded as they arrive; first token ASAP (§49).
class OpenAIProvider : public QObject, public LlmProvider {
    Q_OBJECT
public:
    explicit OpenAIProvider(ProviderConfig config, QObject* parent = nullptr);
    std::string name() const override { return "openai-compatible:" + config_.model; }
    void streamChat(const ChatRequest& request, StreamCallbacks callbacks) override;
    void streamChat(const ChatRequest& request, StreamCallbacks callbacks,
                    CancellationToken token) override;
    void resume() override;
    void cancel() override;

    // Exposed for deterministic transport tests. Images are encoded only at
    // this provider boundary; core references retain their immutable bytes.
    static QJsonObject requestBody(const ProviderConfig& config,
                                   const ChatRequest& request);

    // Blocking single completion used by the Ingest remote step (§5.1).
    std::string complete(const std::string& systemPrompt, const std::string& userPrompt);

private:
    ProviderConfig config_;
    // Requests are created in streamChat/complete, which run on the caller's
    // worker thread. Cancellation crosses that boundary without touching Qt
    // objects from the UI thread.
    // Cancellation is an epoch rather than a reusable boolean.  A resume
    // for a new request cannot accidentally uncancel an older network reply.
    std::atomic<std::uint64_t> cancellationEpoch_{0};
};

} // namespace reader
