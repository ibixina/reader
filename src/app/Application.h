#pragma once
#include "ai/ChatManager.h"
#include "ai/ContextManager.h"
#include "ai/LlmProvider.h"
#include "ai/RetrievalEngine.h"
#include "analysis/PaperIngestor.h"
#include "app/ApplicationState.h"
#include "core/ThreadPool.h"
#include "document/DocumentModel.h"
#include "search/TextIndex.h"
#include "search/VectorIndex.h"
#include "storage/Database.h"
#include "storage/Repositories.h"
#include <memory>
#include <optional>

namespace reader {

// Composition root (§52 app/). Owns lanes (§7): render, extraction,
// analysis, search, network — plus the per-document managers.
class Application {
public:
    Application();
    ~Application();

    // Stop background work while providers, repositories, and UI owners are
    // still alive. Safe to call more than once.
    void shutdown();

    ThreadPool renderPool{2};
    ThreadPool extractPool{2};
    ThreadPool analysisPool{2};
    ThreadPool searchPool{2};
    ThreadPool networkPool{2};
    // Single lane serializing ALL direct pdfium (QPdfDocument) background
    // use: the engine is not thread-safe, so extraction, word indexing and
    // any other background document work queue here in order.
    ThreadPool docLane{1};

    ApplicationState state;
    ContextManager context;
    RetrievalEngine retrieval;
    PaperIngestor ingestor;
    ProviderConfig providerConfig;
    std::unique_ptr<LlmProvider> provider = std::make_unique<EchoProvider>();

    std::unique_ptr<Database> db;
    std::unique_ptr<DocumentRepository> documents;
    std::unique_ptr<AnnotationRepository> annotations;
    std::unique_ptr<ChatRepository> chats;
    std::unique_ptr<ChatManager> chatManager;

    DocumentModel model;
    std::optional<PaperAnalysis> analysis;

    void openDatabase();
    std::unique_ptr<LlmProvider> buildProvider();

private:
    bool shutDown_ = false;
};

} // namespace reader
