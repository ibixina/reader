#include "ai/ChatManager.h"
#include "storage/Database.h"
#include "storage/Repositories.h"
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
using namespace reader;
namespace {
int fail(const char* message) {
    std::cerr << "chat lifecycle check failed: " << message << '\n';
    return 1;
}
struct BlockingProvider final : LlmProvider {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    ChatRequest captured;
    std::string name() const override { return "blocking"; }
    void streamChat(const ChatRequest& request, StreamCallbacks cb, CancellationToken token) override {
        std::unique_lock<std::mutex> lock(mutex);
        captured = request;
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release || token.cancelled(); });
        lock.unlock();
        if (token.cancelled()) {
            if (cb.onError) cb.onError("cancelled");
        } else if (cb.onDone) {
            cb.onDone("ok");
        }
    }
    void streamChat(const ChatRequest& r, StreamCallbacks cb) override {
        streamChat(r, std::move(cb), {});
    }
};
struct ErrorThenEcho final : LlmProvider {
    bool failed = true;
    std::string name() const override { return "recovering"; }
    void streamChat(const ChatRequest&, StreamCallbacks cb) override {
        if (failed) {
            failed = false;
            if (cb.onError) cb.onError("expected failure");
        } else if (cb.onDone) {
            cb.onDone("recovered");
        }
    }
};
struct CitationProvider final : LlmProvider {
    std::string name() const override { return "citations"; }
    void streamChat(const ChatRequest&, StreamCallbacks cb) override {
        if (cb.onDone) cb.onDone("Known [block-x], repeated [block-x], fake [invented].");
    }
};
struct SelectionCitationProvider final : LlmProvider {
    std::string name() const override { return "selection-citations"; }
    void streamChat(const ChatRequest&, StreamCallbacks callbacks) override {
        if (callbacks.onDone) callbacks.onDone("Compare [sel-a] with [sel-b].");
    }
};
struct CapturingCitationProvider final : LlmProvider {
    int calls = 0;
    ChatRequest captured;
    std::string name() const override { return "capturing-citations"; }
    void streamChat(const ChatRequest& request, StreamCallbacks callbacks) override {
        ++calls;
        captured = request;
        if (callbacks.onDone) callbacks.onDone("Sent [block-0], omitted [block-9].");
    }
};
struct FailingStore final : IChatStore {
    int saveCalls = 0;
    int failOnCall = 1;
    std::vector<ChatMessage> saved;

    std::vector<ChatMessage> loadRecent(const ConversationId&, std::size_t) override {
        return saved;
    }
    bool saveMessage(const ConversationId&, const ChatMessage& message) override {
        ++saveCalls;
        if (saveCalls == failOnCall) return false;
        saved.push_back(message);
        return true;
    }
};
}
int main() {
    char tempTemplate[] = "/tmp/reader-chat-lifecycle-XXXXXX";
    char* tempDir = mkdtemp(tempTemplate);
    if (!tempDir) return fail("could not create unique fixture directory");
    const std::string path = (std::filesystem::path(tempDir) / "chat.sqlite").string();
    ConversationId conv;
    {
        Database db(path);
        ChatRepository store(&db);
        conv = store.createConversation("paper-a", "thread");
        ChatManager manager(std::make_unique<EchoProvider>(), &store);
        ContextReference reference;
        reference.id = "r1";
        reference.displayName = "selection";
        reference.extractedText = "original";
        reference.anchor.objectType = "equation";
        reference.anchor.objectId = "eq_7";
        reference.latex = R"(\frac{x}{y})";
        reference.image = ReferenceImage{"image/png", {1, 2, 3}, 1, 1};
        ChatRequest request;
        request.question = "first";
        request.explicitReferences = {reference};
        manager.send(conv, request);
        reference.extractedText = "mutated";
        manager.openConversation(conv);
        const auto history = manager.history(conv);
        if (history.size() != 2 || history.front().references.empty())
            return fail("persisted rich reference snapshot is missing");
        const auto& saved = history.front().references.front();
        if (saved.extractedText != "original" || saved.anchor.objectId != "eq_7" ||
            saved.latex != R"(\frac{x}{y})" || !saved.image ||
            saved.image->bytes != std::vector<std::uint8_t>({1, 2, 3}))
            return fail("immutable rich reference snapshot did not round-trip through SQLite");
    }
    {
        Database db(path);
        ChatRepository store(&db);
        ChatManager manager(std::make_unique<EchoProvider>(), &store);
        manager.openConversation(conv);
        if (manager.history(conv).size() != 2)
            return fail("conversation did not reopen from SQLite");
        ChatRequest request;
        request.question = "second";
        manager.send(conv, request);
        const auto history = manager.history(conv);
        if (history.size() != 4 || history[0].id == history[2].id)
            return fail("reopened history did not append unique message IDs");
    }
    auto provider = std::make_unique<BlockingProvider>();
    auto* raw = provider.get();
    ChatManager manager(std::move(provider));
    const auto id = manager.newConversation();
    ChatRequest request;
    request.question = "blocking";
    std::thread active([&] { manager.send(id, request); });
    {
        std::unique_lock<std::mutex> lock(raw->mutex);
        raw->cv.wait(lock, [&] { return raw->entered; });
    }
    CancellationToken queuedToken;
    std::thread queued([&] { manager.send(id, request, {}, queuedToken); });
    queuedToken.cancel();
    manager.stop();
    {
        std::lock_guard<std::mutex> lock(raw->mutex);
        raw->release = true;
        raw->cv.notify_all();
    }
    active.join();
    queued.join();
    if (manager.history(id).size() != 2 || !manager.history(id).back().incomplete)
        return fail("stop did not persist exactly one incomplete active exchange");
    {
        std::lock_guard<std::mutex> lock(raw->mutex);
        for (const auto& prior : raw->captured.recentConversation)
            if (prior.text == "blocking")
                return fail("request leaked into its own recent-history snapshot");
    }
    manager.setProvider(std::make_unique<ErrorThenEcho>());
    const auto bad = manager.send(id, request);
    if (!bad.incomplete || bad.text.find("expected failure") == std::string::npos)
        return fail("provider error was not retained as incomplete");
    const auto good = manager.send(id, request);
    if (good.incomplete || good.text != "recovered")
        return fail("later request did not recover after provider error");
    ChatManager citations(std::make_unique<CitationProvider>());
    const auto citationConversation = citations.newConversation();
    ContextReference known;
    known.id = "ref";
    known.type = ReferenceType::Paragraph;
    known.anchor.block = "block-x";
    known.extractedText = "known evidence";
    ChatRequest citationRequest;
    citationRequest.question = "cite";
    citationRequest.explicitReferences = {known};
    const auto cited = citations.send(citationConversation, citationRequest);
    if (cited.sources.size() != 1 || cited.sources.front().block != "block-x" ||
        cited.sources.front().anchorText != "known evidence") {
        std::cerr << "citation sources=" << cited.sources.size();
        if (!cited.sources.empty())
            std::cerr << " block=" << cited.sources.front().block.value_or("<none>")
                      << " text=" << cited.sources.front().anchorText;
        std::cerr << '\n';
        return fail("invented or duplicate citation escaped source resolution");
    }
    {
        Database database(path);
        ChatRepository store(&database);
        const auto selectionConversation = store.createConversation("paper-a", "selections");
        ChatManager selectionManager(std::make_unique<SelectionCitationProvider>(), &store);
        ContextReference first;
        first.id = "sel-a";
        first.type = ReferenceType::TextSelection;
        first.anchor.document = "paper-a";
        first.anchor.page = 2;
        first.anchor.block = "shared-block";
        first.anchor.bounds = {10, 20, 40, 10};
        first.extractedText = "first distinct selection";
        ContextReference second = first;
        second.id = "sel-b";
        second.anchor.bounds = {60, 20, 50, 10};
        second.extractedText = "second distinct selection";
        ChatRequest selectionRequest;
        selectionRequest.question = "compare";
        selectionRequest.explicitReferences = {first, second};
        const auto answer = selectionManager.send(selectionConversation, selectionRequest);
        if (answer.sourceRecords.size() != 2 || answer.sourceRecords[0].citationId != "sel-a" ||
            answer.sourceRecords[1].citationId != "sel-b")
            return fail("canonical selection citation IDs were lost before persistence");
        ChatManager reopened(std::make_unique<EchoProvider>(), &store);
        reopened.openConversation(selectionConversation);
        const auto persisted = reopened.history(selectionConversation);
        if (persisted.size() != 2 || persisted.back().sourceRecords.size() != 2 ||
            persisted.back().sourceRecords[0].citationId != "sel-a" ||
            persisted.back().sourceRecords[1].citationId != "sel-b" ||
            persisted.back().sourceRecords[0].anchor.bounds.x ==
                persisted.back().sourceRecords[1].anchor.bounds.x ||
            persisted.back().sourceRecords[0].anchor.anchorText != "first distinct selection" ||
            persisted.back().sourceRecords[1].anchor.anchorText != "second distinct selection")
            return fail("distinct same-block selection sources did not survive reopen");
    }
    auto boundedProvider = std::make_unique<CapturingCitationProvider>();
    auto* boundedCapture = boundedProvider.get();
    ChatManager boundedManager(std::move(boundedProvider));
    const auto boundedConversation = boundedManager.newConversation();
    ChatRequest boundedRequest;
    boundedRequest.question = "Which evidence was actually sent?";
    for (int i = 0; i < 10; ++i) {
        ContextReference reference;
        reference.id = "ref-" + std::to_string(i);
        reference.type = ReferenceType::Paragraph;
        reference.anchor.block = "block-" + std::to_string(i);
        reference.extractedText = std::string(3000, static_cast<char>('a' + i));
        boundedRequest.explicitReferences.push_back(std::move(reference));
    }
    const auto boundedAnswer = boundedManager.send(boundedConversation, boundedRequest);
    const auto boundedHistory = boundedManager.history(boundedConversation);
    if (boundedCapture->calls != 1 ||
        boundedCapture->captured.question != boundedRequest.question ||
        boundedCapture->captured.explicitReferences.empty() ||
        boundedCapture->captured.explicitReferences.size() >=
            boundedRequest.explicitReferences.size() ||
        boundedHistory.front().references.size() != boundedRequest.explicitReferences.size())
        return fail("ChatManager did not separate immutable references from the bounded wire request");
    if (boundedAnswer.sourceRecords.size() != 1 ||
        boundedAnswer.sourceRecords.front().citationId != "block-0" ||
        boundedAnswer.sourceRecords.front().anchor.anchorText !=
            boundedCapture->captured.explicitReferences.front().extractedText)
        return fail("citation resolution included omitted evidence or lost the sent excerpt");
    ChatRequest oversizedRequest;
    oversizedRequest.question = std::string(13000, 'q');
    const auto oversizedAnswer = boundedManager.send(boundedConversation, oversizedRequest);
    if (!oversizedAnswer.incomplete ||
        oversizedAnswer.text.find("question is too long") == std::string::npos ||
        boundedCapture->calls != 1 ||
        boundedManager.history(boundedConversation).at(2).text != oversizedRequest.question)
        return fail("oversized question was truncated/sent or its retry text was discarded");
    FailingStore userFailure;
    ChatManager unsavedUser(std::make_unique<EchoProvider>(), &userFailure);
    const auto failedConversation = unsavedUser.newConversation();
    ChatRequest failedRequest;
    failedRequest.question = "keep this question";
    const auto failed = unsavedUser.send(failedConversation, failedRequest);
    const auto failedHistory = unsavedUser.history(failedConversation);
    if (!failed.incomplete || failed.text.find("could not be saved") == std::string::npos ||
        failedHistory.size() != 2 || failedHistory.front().text != "keep this question" ||
        userFailure.saveCalls != 1)
        return fail("failed user persistence was silent or discarded retry text");
    FailingStore answerFailure;
    answerFailure.failOnCall = 2;
    ChatManager unsavedAnswer(std::make_unique<EchoProvider>(), &answerFailure);
    const auto answerConversation = unsavedAnswer.newConversation();
    const auto answer = unsavedAnswer.send(answerConversation, failedRequest);
    if (!answer.incomplete || answer.text.find("Storage error") == std::string::npos ||
        answerFailure.saved.size() != 1 || answerFailure.saved.front().role != "user")
        return fail("failed assistant persistence was reported as complete");
    std::filesystem::remove_all(tempDir);
    std::cout << "chat lifecycle checks passed\n";
    return 0;
}
