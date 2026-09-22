#include "ai/EmbeddingProviderQt.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>

namespace reader {

EmbeddingProviderQt::EmbeddingProviderQt(EmbeddingProviderQtConfig config)
    : config_(std::move(config)) {}

std::string EmbeddingProviderQt::modelId() const {
    return config_.model + "@" + config_.baseUrl;
}

bool EmbeddingProviderQt::embed(const std::string& text, std::vector<float>& output,
                                const CancellationToken& token, std::string& error) const {
    std::vector<std::vector<float>> outputs;
    if (!embedBatch({text}, outputs, token, error)) return false;
    if (outputs.size() != 1) {
        error = "embedding service returned the wrong batch size";
        return false;
    }
    output = std::move(outputs.front());
    return true;
}

bool EmbeddingProviderQt::embedBatch(const std::vector<std::string>& texts,
                                     std::vector<std::vector<float>>& outputs,
                                     const CancellationToken& token,
                                     std::string& error) const {
    outputs.clear();
    if (texts.empty()) return true;
    if (config_.baseUrl.empty() || config_.model.empty() || config_.dimension == 0) {
        error = "embedding provider requires base URL, model, and dimension";
        return false;
    }
    if (token.cancelled()) {
        error = "embedding cancelled";
        return false;
    }
    QJsonArray input;
    for (const auto& text : texts) input.append(QString::fromStdString(text));
    const QJsonObject body{{"model", QString::fromStdString(config_.model)}, {"input", input}};
    QString url = QString::fromStdString(config_.baseUrl);
    if (!url.endsWith('/')) url += '/';
    QNetworkRequest request(QUrl(url + "embeddings"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!config_.apiKey.empty())
        request.setRawHeader("Authorization", ("Bearer " + config_.apiKey).c_str());

    QNetworkAccessManager network;
    QNetworkReply* reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancelPoll;
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
        if (token.cancelled()) reply->abort();
    });
    cancelPoll.start(25);
    bool timedOut = false;
    QTimer::singleShot(config_.timeoutMs, &loop, [&] {
        if (!reply->isFinished()) {
            timedOut = true;
            reply->abort();
        }
    });
    loop.exec();
    cancelPoll.stop();
    const QByteArray raw = reply->readAll();
    if (token.cancelled()) {
        error = "embedding cancelled";
        return false;
    }
    if (timedOut) {
        error = "embedding request timed out";
        return false;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        error = status >= 400 ? "embedding HTTP " + std::to_string(status)
                              : reply->errorString().toStdString();
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = "embedding response is not valid JSON";
        return false;
    }
    const auto entries = document.object().value("data").toArray();
    outputs.resize(texts.size());
    std::vector<bool> seen(texts.size(), false);
    for (const auto& entry : entries) {
        const auto object = entry.toObject();
        const int index = object.value("index").toInt(-1);
        if (index < 0 || static_cast<std::size_t>(index) >= texts.size() || seen[index]) {
            error = "embedding response has an invalid index";
            outputs.clear();
            return false;
        }
        const auto values = object.value("embedding").toArray();
        if (static_cast<std::size_t>(values.size()) != config_.dimension) {
            error = "embedding response dimension mismatch";
            outputs.clear();
            return false;
        }
        auto& vector = outputs[static_cast<std::size_t>(index)];
        vector.reserve(values.size());
        for (const auto& value : values) {
            const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
                number > std::numeric_limits<float>::max()) {
                error = "embedding response contains a non-finite value";
                outputs.clear();
                return false;
            }
            vector.push_back(static_cast<float>(number));
        }
        seen[static_cast<std::size_t>(index)] = true;
    }
    if (static_cast<std::size_t>(entries.size()) != texts.size() ||
        std::find(seen.begin(), seen.end(), false) != seen.end()) {
        error = "embedding response has the wrong batch size";
        outputs.clear();
        return false;
    }
    return true;
}

} // namespace reader
