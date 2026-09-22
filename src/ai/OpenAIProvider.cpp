#include "ai/OpenAIProvider.h"
#include "ai/PromptBuilder.h"
#include <QEventLoop>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <unordered_map>
#include <unordered_set>

namespace reader {

QJsonObject OpenAIProvider::requestBody(const ProviderConfig& config,
                                        const ChatRequest& request) {
    const auto built = PromptBuilder::build(request);
    QJsonObject system{{"role", "system"},
                       {"content", QString::fromStdString(built.system)}};
    QJsonArray content;
    content.append(QJsonObject{{"type", "text"},
                               {"text", QString::fromStdString(built.user)}});
    constexpr std::size_t maxImages = 4;
    constexpr std::size_t maxImageBytes = 8 * 1024 * 1024;
    std::size_t imageCount = 0;
    for (const auto& reference : request.explicitReferences) {
        if (!reference.image || reference.image->bytes.empty() ||
            reference.image->bytes.size() > maxImageBytes || imageCount >= maxImages)
            continue;
        const std::string& mime = reference.image->mimeType;
        if (mime != "image/png" && mime != "image/jpeg" && mime != "image/webp") continue;
        const auto& bytes = reference.image->bytes;
        const QByteArray encoded =
            QByteArray(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<qsizetype>(bytes.size())).toBase64();
        const QString url = QString("data:%1;base64,%2")
                                .arg(QString::fromStdString(mime), QString::fromLatin1(encoded));
        content.append(QJsonObject{{"type", "image_url"},
                                   {"image_url", QJsonObject{{"url", url},
                                                              {"detail", "high"}}}});
        ++imageCount;
    }
    QJsonObject user{{"role", "user"},
                     {"content", imageCount == 0
                                     ? QJsonValue(QString::fromStdString(built.user))
                                     : QJsonValue(content)}};
    return QJsonObject{{"model", QString::fromStdString(config.model)},
                       {"stream", true},
                       {"messages", QJsonArray{system, user}}};
}

OpenAIProvider::OpenAIProvider(ProviderConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {}

void OpenAIProvider::streamChat(const ChatRequest& request, StreamCallbacks cb) {
    streamChat(request, std::move(cb), {});
}

void OpenAIProvider::streamChat(const ChatRequest& request, StreamCallbacks cb,
                                CancellationToken requestToken) {
    const auto requestEpoch = cancellationEpoch_.load(std::memory_order_acquire);
    const QJsonObject body = requestBody(config_, request);
    QNetworkRequest req(QUrl(QString::fromStdString(config_.baseUrl + "/chat/completions")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!config_.apiKey.empty())
        req.setRawHeader("Authorization", ("Bearer " + config_.apiKey).c_str());
    // QNetworkAccessManager must be used from the thread that owns it.  This
    // provider is called by the network lane, so keep the manager and reply
    // local to this invocation instead of sharing a UI-thread QObject.
    QNetworkAccessManager net;
    QNetworkReply* reply = net.post(req, QJsonDocument(body).toJson());

    // ChatManager drives providers synchronously from a worker lane (§7):
    // pump a local event loop here so send() returns the complete message
    // while tokens still stream to the UI incrementally (§49).
    std::string full;
    std::unordered_map<std::string, DocumentAnchor> anchorsById;
    for (const auto& ref : request.explicitReferences) {
        const std::string id = contextReferenceId(ref);
        if (!id.empty()) anchorsById.emplace(id, ref.anchor);
        for (const auto& related : ref.relatedSources)
            anchorsById.emplace(anchorReferenceId(related), related);
    }
    for (const auto& passage : request.retrievedPassages) {
        const std::string id = retrievedPassageId(passage);
        if (!id.empty()) anchorsById.emplace(id, passage.anchor);
    }
    QByteArray buffer;
    QByteArray rawResponse;
    bool malformed = false;
    bool sawData = false;
    QEventLoop loop;
    auto consumeLines = [&] {
        for (;;) {
            const qsizetype newline = buffer.indexOf('\n');
            if (newline < 0) break;
            QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1); // keep all incomplete UTF-8 bytes
            if (line.endsWith('\r')) line.chop(1);
            line = line.trimmed();
            if (!line.startsWith("data:")) continue;
            const QByteArray payload = line.mid(5).trimmed();
            if (payload.isEmpty()) continue;
            sawData = true;
            if (payload == "[DONE]") continue;
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                malformed = true;
                continue;
            }
            const QJsonArray choices = doc.object().value("choices").toArray();
            if (choices.isEmpty()) continue; // valid finish/role-only SSE event
            const QJsonObject delta = choices.first().toObject().value("delta").toObject();
            const QString tok = delta.value("content").toString();
            if (!tok.isEmpty()) {
                full += tok.toUtf8().toStdString();
                if (cb.onToken) cb.onToken(tok.toUtf8().toStdString());
            }
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        const QByteArray bytes = reply->readAll();
        rawResponse += bytes;
        buffer += bytes;
        consumeLines();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancelPoll;
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
        if (requestToken.cancelled() ||
            cancellationEpoch_.load(std::memory_order_acquire) != requestEpoch) reply->abort();
    });
    cancelPoll.start(50);
    bool timedOut = false;
    QTimer::singleShot(120000, &loop, [&] {
        if (!reply->isFinished()) {
            timedOut = true;
            reply->abort();
        }
    });
    loop.exec();
    // A few compatible servers omit the final newline. Preserve and parse the
    // complete final SSE data line without decoding partial network chunks.
    if (!buffer.isEmpty()) {
        buffer.append('\n');
        consumeLines();
    }
    cancelPoll.stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool cancelled = requestToken.cancelled() ||
                           cancellationEpoch_.load(std::memory_order_acquire) != requestEpoch;
    const bool failed = cancelled || timedOut || reply->error() != QNetworkReply::NoError ||
                        (status >= 400) || malformed || !sawData || full.empty();
    std::string err;
    if (cancelled) err = "cancelled";
    else if (timedOut) err = "request timed out";
    else if (reply->error() != QNetworkReply::NoError || status >= 400) {
        QJsonParseError parseError;
        const QJsonDocument errorDocument = QJsonDocument::fromJson(rawResponse, &parseError);
        const QString serviceMessage =
            parseError.error == QJsonParseError::NoError && errorDocument.isObject()
                ? errorDocument.object().value("error").toObject().value("message").toString()
                : QString{};
        err = status >= 400 ? "HTTP " + std::to_string(status) : reply->errorString().toStdString();
        if (!serviceMessage.isEmpty()) err += ": " + serviceMessage.left(500).toStdString();
    }
    else if (malformed) err = "malformed streaming response";
    else if (!sawData || full.empty()) err = "empty streaming response";
    if (failed) {
        if (cb.onError) cb.onError(err);
    } else {
        if (!cancelled && cb.onSource) {
            std::unordered_set<std::string> emitted;
            std::size_t begin = 0;
            while ((begin = full.find('[', begin)) != std::string::npos) {
                const std::size_t end = full.find(']', begin + 1);
                if (end == std::string::npos) break;
                const std::string id = full.substr(begin + 1, end - begin - 1);
                const auto it = anchorsById.find(id);
                if (it != anchorsById.end() && emitted.insert(id).second)
                    cb.onSource(it->second);
                begin = end + 1;
            }
        }
        if (cb.onDone) cb.onDone(full);
    }
}

void OpenAIProvider::cancel() {
    cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
}

void OpenAIProvider::resume() {
    // Epoch cancellation is intentionally one way for each request.  A new
    // request snapshots the current epoch in streamChat/complete.
}

std::string OpenAIProvider::complete(const std::string& systemPrompt,
                                     const std::string& userPrompt) {
    const auto requestEpoch = cancellationEpoch_.load(std::memory_order_acquire);
    QJsonArray messages;
    if (!systemPrompt.empty())
        messages.append(
            QJsonObject{{"role", "system"}, {"content", QString::fromStdString(systemPrompt)}});
    messages.append(QJsonObject{{"role", "user"}, {"content", QString::fromStdString(userPrompt)}});
    QJsonObject body{{"model", QString::fromStdString(config_.model)},
                     {"stream", false},
                     {"messages", messages}};
    QNetworkRequest req(QUrl(QString::fromStdString(config_.baseUrl + "/chat/completions")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!config_.apiKey.empty())
        req.setRawHeader("Authorization", ("Bearer " + config_.apiKey).c_str());
    QNetworkAccessManager net;
    QNetworkReply* reply = net.post(req, QJsonDocument(body).toJson());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancelPoll;
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
        if (cancellationEpoch_.load(std::memory_order_acquire) != requestEpoch) reply->abort();
    });
    cancelPoll.start(50);
    bool timedOut = false;
    QTimer::singleShot(120000, &loop, [&] {
        if (!reply->isFinished()) {
            timedOut = true;
            reply->abort();
        }
    });
    loop.exec();
    cancelPoll.stop();
    std::string out;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!timedOut && cancellationEpoch_.load(std::memory_order_acquire) == requestEpoch &&
        reply->error() == QNetworkReply::NoError && status < 400) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonArray choices = doc.object().value("choices").toArray();
            if (!choices.isEmpty()) {
                out = choices.first().toObject().value("message").toObject()
                          .value("content").toString().toUtf8().toStdString();
            }
        }
    }
    return out;
}

} // namespace reader
