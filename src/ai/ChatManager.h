#pragma once
#include "ai/LlmProvider.h"
#include "ai/References.h"
#include "core/CancellationToken.h"
#include <functional>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace reader {

// Per-paper multi-conversation manager (§30): new/rename/delete/reopen,
// snapshot references per message (§16), stop/retry/edit-resend (§49).
// Persistence is injected so core stays UI-free.
class IChatStore {
public:
    virtual ~IChatStore() = default;
    // Persistent stores may bind a conversation to a document.  The default
    // keeps the UI-free in-memory store and existing test doubles usable.
    virtual ConversationId createConversation(const DocumentId&, const std::string&) { return {}; }
    virtual std::vector<ChatMessage> loadRecent(const ConversationId& conv, std::size_t n) = 0;
    // Returns false when durable persistence failed.  Callers can retain the
    // message and surface a retryable error instead of reporting success.
    virtual bool saveMessage(const ConversationId& conv, const ChatMessage& msg) = 0;
};

class MemoryChatStore : public IChatStore {
public:
    ConversationId createConversation(const DocumentId&, const std::string&) override;
    std::vector<ChatMessage> loadRecent(const ConversationId& conv, std::size_t n) override;
    bool saveMessage(const ConversationId& conv, const ChatMessage& msg) override;

private:
    std::unordered_map<ConversationId, std::vector<ChatMessage>> data_;
};

class ChatManager {
public:
    ChatManager(std::unique_ptr<LlmProvider> provider, IChatStore* store = nullptr);

    ConversationId newConversation(const std::string& title = "New chat",
                                   const DocumentId& document = {});
    void openConversation(const ConversationId& conv);
    void setProvider(std::unique_ptr<LlmProvider> p);
    void resume();
    void stop();

    using TokenCallback = std::function<void(const std::string&)>;
    ChatMessage send(const ConversationId& conv, ChatRequest req, TokenCallback onToken = {},
                     CancellationToken token = {});

    std::vector<ChatMessage> history(const ConversationId& conv) const;

private:
    std::shared_ptr<LlmProvider> provider_;
    IChatStore* store_;
    MemoryChatStore fallback_;
    std::unordered_map<ConversationId, std::vector<ChatMessage>> history_;
    std::mutex providerMutex_;
    std::mutex sendMutex_;
    mutable std::mutex historyMutex_;
    // Monotonically advances on every stop.  A later resume may restart the
    // provider for a new request, but it must never make an older queued
    // request publish tokens or completion callbacks again.
    std::atomic<std::uint64_t> cancellationGeneration_{0};
    CancellationToken activeRequestToken_;
    std::mutex activeTokenMutex_;
};

} // namespace reader
