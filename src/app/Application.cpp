#include "app/Application.h"
#include "ai/OpenAIProvider.h"
#include <QSettings>
#include <cstdlib>
#include <filesystem>

namespace reader {

namespace {
class ConfigurationErrorProvider final : public LlmProvider {
public:
    explicit ConfigurationErrorProvider(std::string message) : message_(std::move(message)) {}
    std::string name() const override { return "configuration-error"; }
    void streamChat(const ChatRequest&, StreamCallbacks callbacks) override {
        if (callbacks.onError) callbacks.onError(message_);
    }

private:
    std::string message_;
};
} // namespace

Application::Application() {
    QSettings settings;
    providerConfig.kind = settings.value("provider/kind", "offline").toString().toStdString();
    if (providerConfig.kind != "offline" && providerConfig.kind != "openai" &&
        providerConfig.kind != "compatible")
        providerConfig.kind = "offline";
    providerConfig.model =
        settings.value("provider/model", QString::fromStdString(providerConfig.model))
            .toString().toStdString();
    providerConfig.baseUrl =
        settings.value("provider/baseUrl", QString::fromStdString(providerConfig.baseUrl))
            .toString().toStdString();
    providerConfig.apiKey = settings.value("openai/apiKey").toString().toStdString();
    if (const char* key = std::getenv("OPENAI_API_KEY")) {
        if (*key) {
            providerConfig.kind = "openai";
            providerConfig.apiKey = key;
        }
    }
    if (const char* m = std::getenv("PAPER_READER_MODEL")) {
        if (*m) providerConfig.model = m;
    }
    openDatabase();
}

Application::~Application() { shutdown(); }

void Application::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    if (chatManager) chatManager->stop();
    // Join every lane before providers, repositories, and the UI-owned
    // objects used by queued callbacks are destroyed.
    networkPool.shutdown();
    analysisPool.shutdown();
    docLane.shutdown();
    searchPool.shutdown();
    extractPool.shutdown();
    renderPool.shutdown();
}

void Application::openDatabase() {
    std::string path = defaultDbPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    db = std::make_unique<Database>(path);
    documents = std::make_unique<DocumentRepository>(db.get());
    annotations = std::make_unique<AnnotationRepository>(db.get());
    chats = std::make_unique<ChatRepository>(db.get());
    // One manager per app lifetime: conversations persist in SQLite (§30),
    // message references are snapshotted per send (§16).
    chatManager = std::make_unique<ChatManager>(buildProvider(), chats.get());
}

std::unique_ptr<LlmProvider> Application::buildProvider() {
    if (providerConfig.kind == "compatible")
        return std::make_unique<OpenAIProvider>(providerConfig);
    if (providerConfig.kind == "openai") {
        if (!providerConfig.apiKey.empty())
            return std::make_unique<OpenAIProvider>(providerConfig);
        return std::make_unique<ConfigurationErrorProvider>(
            "OpenAI requires an API key. Add one in Chat provider settings.");
    }
    // No key (or offline kind): deterministic local provider (§58).
    return makeProvider(providerConfig);
}

} // namespace reader
