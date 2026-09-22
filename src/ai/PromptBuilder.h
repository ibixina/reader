#pragma once
#include "ai/References.h"
#include <string>

namespace reader {

// Renders a ChatRequest into provider prompt text. Structured references
// keep stable IDs so answers can cite resolvable anchors (§21); grounding
// rules enforce paper-vs-interpretation separation (§50) and honest
// "not in the paper" fallback (§51).
class PromptBuilder {
public:
    struct Options {
        std::size_t charBudget = 12000;
    };
    struct Built {
        std::string system;
        std::string user;
    };
    struct Prepared {
        ChatRequest request;
        Built prompt;
        std::string error;

        explicit operator bool() const { return error.empty(); }
    };

    // Produces the exact bounded request that may be sent to a provider.
    // The question is never truncated. Evidence that does not fit is removed
    // from `request`, so citation resolution cannot see unsent sources.
    static Prepared prepare(const ChatRequest& req);
    static Prepared prepare(const ChatRequest& req, const Options& options);
    static Built build(const ChatRequest& req);
    static Built build(const ChatRequest& req, const Options& options);
};

} // namespace reader
