#include "ai/ChatManager.h"
#include "ai/PromptBuilder.h"
#include "core/Types.h"
#include <atomic>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace reader {

namespace {

std::vector<ChatSource> citedSources(const ChatRequest& request, const std::string& text) {
    std::unordered_map<std::string, DocumentAnchor> known;
    for (const auto& ref : request.explicitReferences) {
        const auto id = contextReferenceId(ref); if (!id.empty()) { auto anchor = ref.anchor; anchor.anchorText = ref.extractedText; known.emplace(id, anchor); }
        for (auto related : ref.relatedSources) {
            const auto relatedId = anchorReferenceId(related);
            if (!relatedId.empty()) known.emplace(relatedId, std::move(related));
        }
    }
    for (const auto& passage : request.retrievedPassages) {
        const auto id = retrievedPassageId(passage); if (!id.empty()) { auto anchor = passage.anchor; anchor.anchorText = passage.text; known.emplace(id, anchor); }
    }
    std::vector<ChatSource> out;
    std::unordered_set<std::string> seen;
    for (std::size_t pos = 0; (pos = text.find('[', pos)) != std::string::npos; ++pos) {
        const auto end = text.find(']', pos + 1);
        if (end == std::string::npos) break;
        const auto id = text.substr(pos + 1, end - pos - 1);
        const auto it = known.find(id);
        if (it != known.end() && seen.insert(id).second) out.push_back({id, it->second});
        pos = end;
    }
    return out;
}

std::string uniqueId(const char* prefix) {
    static std::atomic<unsigned long long> sequence{0};
    static std::random_device random;
    static std::mutex randomMutex;
    const auto n = sequence.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(randomMutex);
    return std::string(prefix) + sha256Hex(std::to_string(nowMs()) + ":" +
                                           std::to_string(random()) + ":" +
                                           std::to_string(n));
}

} // namespace

std::vector<ChatMessage> MemoryChatStore::loadRecent(const ConversationId& conv, std::size_t n) {
    auto it = data_.find(conv);
    if (it == data_.end()) return {};
    const auto& v = it->second;
    if (v.size() <= n) return v;
    return std::vector<ChatMessage>(v.end() - n, v.end());
}

bool MemoryChatStore::saveMessage(const ConversationId& conv, const ChatMessage& msg) {
    data_[conv].push_back(msg);
    return true;
}

ConversationId MemoryChatStore::createConversation(const DocumentId&, const std::string&) {
    return uniqueId("conv_");
}

ChatManager::ChatManager(std::unique_ptr<LlmProvider> provider, IChatStore* store)
    : provider_(std::shared_ptr<LlmProvider>(std::move(provider))),
      store_(store ? store : &fallback_) {}

void ChatManager::setProvider(std::unique_ptr<LlmProvider> p) {
    std::shared_ptr<LlmProvider> next(std::move(p));
    std::lock_guard<std::mutex> lock(providerMutex_);
    provider_ = std::move(next);
}

void ChatManager::stop() {
    cancellationGeneration_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(activeTokenMutex_);
        activeRequestToken_.cancel();
    }
    std::shared_ptr<LlmProvider> provider;
    {
        std::lock_guard<std::mutex> lock(providerMutex_);
        provider = provider_;
    }
    if (provider) provider->cancel();
}

void ChatManager::resume() {
    std::shared_ptr<LlmProvider> provider;
    {
        std::lock_guard<std::mutex> lock(providerMutex_);
        provider = provider_;
    }
    if (provider) provider->resume();
}

ConversationId ChatManager::newConversation(const std::string& title, const DocumentId& document) {
    ConversationId id;
    if (!document.empty()) id = store_->createConversation(document, title);
    if (id.empty()) id = uniqueId("conv_");
    std::lock_guard<std::mutex> lock(historyMutex_);
    history_[id] = {};
    return id;
}

void ChatManager::openConversation(const ConversationId& conv) {
    auto loaded = store_->loadRecent(conv, 1000);
    std::lock_guard<std::mutex> lock(historyMutex_);
    history_[conv] = std::move(loaded);
}

std::vector<ChatMessage> ChatManager::history(const ConversationId& conv) const {
    std::lock_guard<std::mutex> lock(historyMutex_);
    const auto it = history_.find(conv);
    return it == history_.end() ? std::vector<ChatMessage>{} : it->second;
}

ChatMessage ChatManager::send(const ConversationId& conv, ChatRequest req, TokenCallback onToken,
                              CancellationToken token) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    if (token.cancelled()) {
        ChatMessage cancelled;
        cancelled.id = uniqueId("msg_");
        cancelled.role = "assistant";
        cancelled.incomplete = true;
        cancelled.createdAt = nowMs();
        return cancelled;
    }
    CancellationToken requestToken;
    {
        std::lock_guard<std::mutex> lock(activeTokenMutex_);
        activeRequestToken_ = CancellationToken{};
        requestToken = activeRequestToken_;
    }
    const auto requestGeneration = cancellationGeneration_.load(std::memory_order_acquire);
    std::shared_ptr<LlmProvider> provider;
    {
        std::lock_guard<std::mutex> lock(providerMutex_);
        provider = provider_;
    }
    req.recentConversation = store_->loadRecent(conv, 10);

    ChatMessage user;
    user.id = uniqueId("msg_");
    user.role = "user";
    user.text = req.question;
    user.references = req.explicitReferences; // immutable snapshot (§16)
    user.createdAt = nowMs();
    {
        std::lock_guard<std::mutex> historyLock(historyMutex_);
        auto& localHistory = history_[conv];
        if (localHistory.empty()) localHistory = req.recentConversation;
        localHistory.push_back(user);
    }

    ChatMessage assistant;
    assistant.id = uniqueId("msg_");
    assistant.role = "assistant";
    assistant.createdAt = nowMs();
    assistant.incomplete = true;

    if (!store_->saveMessage(conv, user)) {
        assistant.text = "Message could not be saved. Check local storage and retry; "
                         "your question is still available in this conversation.";
        std::lock_guard<std::mutex> historyLock(historyMutex_);
        history_[conv].push_back(assistant);
        return assistant;
    }

    const auto prepared = PromptBuilder::prepare(req);
    if (!prepared)
        assistant.text = "[Request error: " + prepared.error + "]";

    StreamCallbacks cb;
    cb.onToken = [&](const std::string& tok) {
        if (token.cancelled() || requestGeneration != cancellationGeneration_.load(std::memory_order_acquire)) return;
        assistant.text += tok;
        if (onToken) onToken(tok);
    };
    cb.onSource = [&](const DocumentAnchor& a) {
        if (!token.cancelled() && requestGeneration == cancellationGeneration_.load(std::memory_order_acquire))
            assistant.sources.push_back(a);
    };
    cb.onDone = [&](const std::string& full) {
        if (token.cancelled() || requestGeneration != cancellationGeneration_.load(std::memory_order_acquire)) return;
        assistant.text = full;
        assistant.incomplete = false;
    };
    cb.onError = [&](const std::string& err) {
        if (token.cancelled() || requestGeneration != cancellationGeneration_.load(std::memory_order_acquire)) return;
        assistant.text += "\n[Error: " + err + "]";
        assistant.incomplete = true;
    };
    if (prepared && provider && !token.cancelled() &&
        requestGeneration == cancellationGeneration_.load(std::memory_order_acquire))
        provider->streamChat(prepared.request, cb, requestToken);
    // Providers may offer source callbacks, but the assistant text is the
    // authoritative citation surface: only bracketed IDs present in this
    // request's context become persisted evidence.
    if (prepared) assistant.sourceRecords = citedSources(prepared.request, assistant.text);
    assistant.sources.clear();
    assistant.sources.reserve(assistant.sourceRecords.size());
    for (const auto& source : assistant.sourceRecords)
        assistant.sources.push_back(source.anchor);
    if (token.cancelled() || requestGeneration != cancellationGeneration_.load(std::memory_order_acquire))
        assistant.incomplete = true;
    if (!store_->saveMessage(conv, assistant)) {
        assistant.incomplete = true;
        if (!assistant.text.empty()) assistant.text += "\n\n";
        assistant.text += "[Storage error: this response could not be saved. Copy it now or retry.]";
    }
    {
        std::lock_guard<std::mutex> historyLock(historyMutex_);
        history_[conv].push_back(assistant);
    }
    return assistant;
}

} // namespace reader
