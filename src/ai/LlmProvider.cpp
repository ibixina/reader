#include "ai/LlmProvider.h"
#include <sstream>

namespace reader {

void EchoProvider::streamChat(const ChatRequest& request, StreamCallbacks cb) {
    std::ostringstream os;
    os << "Based on the current context";
    if (!request.explicitReferences.empty()) {
        os << " (";
        bool first = true;
        for (const auto& r : request.explicitReferences) {
            if (!first) os << ", ";
            first = false;
            os << r.displayName << " [" << contextReferenceId(r) << "]";
        }
        os << ")";
    }
    os << ":\n\n";
    if (!request.explicitReferences.empty()) {
        os << "The selected passage states: \"" << request.explicitReferences.front().extractedText.substr(0, 300)
           << "\"\n\n";
    } else if (!request.retrievedPassages.empty()) {
        os << "The most relevant passage states: \"" << request.retrievedPassages.front().text.substr(0, 300)
           << "\" [" << retrievedPassageId(request.retrievedPassages.front()) << "]\n\n";
    } else {
        os << "I don't see this addressed explicitly in the paper.\n";
    }
    os << "Interpretation: this relates to the surrounding section argument; "
          "verify against the cited sources before relying on it.";
    std::string full = os.str();
    // Stream word-by-word to exercise the incremental UI path (§49).
    std::size_t offset = 0;
    while (offset < full.size()) {
        const std::size_t boundary = full.find(' ', offset);
        const std::size_t end = boundary == std::string::npos ? full.size() : boundary + 1;
        if (cb.onToken) cb.onToken(full.substr(offset, end - offset));
        offset = end;
    }
    // The fallback names the first context item in its answer, so expose only
    // that anchor as cited evidence. Retrieved context that was not mentioned
    // remains available to the composer but is not presented as a citation.
    if (cb.onSource) {
        if (!request.explicitReferences.empty()) cb.onSource(request.explicitReferences.front().anchor);
        else if (!request.retrievedPassages.empty()) cb.onSource(request.retrievedPassages.front().anchor);
    }
    if (cb.onDone) cb.onDone(full);
}

std::unique_ptr<LlmProvider> makeProvider(const ProviderConfig& config) {
    if (config.kind == "offline" || config.kind == "local" || config.apiKey.empty())
        return std::make_unique<EchoProvider>();
    // Network providers live in OpenAIProvider.cpp (QtNetwork, UI build).
    // Core builds fall back to echo so offline behavior always works (§58).
    return std::make_unique<EchoProvider>();
}

} // namespace reader
