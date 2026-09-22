#pragma once
#include "ai/References.h"
#include "core/CancellationToken.h"
#include "document/DocumentAnchor.h"
#include <functional>
#include <memory>
#include <string>

namespace reader {

struct StreamCallbacks {
    std::function<void(const std::string& token)> onToken;
    std::function<void(const DocumentAnchor& source)> onSource;
    std::function<void(const std::string& fullText)> onDone;
    std::function<void(const std::string& error)> onError;
};

// Provider abstraction (§48): the UI depends only on this interface.
class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual std::string name() const = 0;
    virtual void streamChat(const ChatRequest& request, StreamCallbacks callbacks) = 0;
    virtual void streamChat(const ChatRequest& request, StreamCallbacks callbacks,
                            CancellationToken token) {
        streamChat(request, std::move(callbacks));
        (void)token;
    }
    virtual void resume() {}
    virtual void cancel() {}
};

// Deterministic offline provider: cites the actual context anchors so the
// select -> ask -> verify loop works with no network (§66 prototype).
class EchoProvider : public LlmProvider {
public:
    std::string name() const override { return "offline-echo"; }
    void streamChat(const ChatRequest& request, StreamCallbacks cb) override;
};

struct ProviderConfig {
    std::string kind = "offline"; // openai|compatible|local|offline
    std::string apiKey;
    std::string baseUrl = "https://api.openai.com/v1";
    std::string model = "gpt-4o-mini";
};

std::unique_ptr<LlmProvider> makeProvider(const ProviderConfig& config);

} // namespace reader
