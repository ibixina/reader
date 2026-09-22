#include "ai/EmbeddingProviderQt.h"
#include "document/DocumentModel.h"
#include "search/VectorIndex.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "embedding check failed: " #condition << '\n'; \
            ++failures; \
        } \
    } while (false)
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        std::cout << "embedding adapter test skipped: local TCP bind unavailable ("
                  << server.errorString().toStdString() << ")\n";
        return 77;
    }
    std::atomic<int> mode{0};
    bool authSeen = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            auto* socket = server.nextPendingConnection();
            socket->setProperty("requestBuffer", QByteArray{});
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &mode, &authSeen] {
                auto request = socket->property("requestBuffer").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestBuffer", request);
                const auto headerEnd = request.indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                int contentLength = 0;
                for (const auto& line : request.left(headerEnd).split('\n')) {
                    const auto normalized = line.trimmed();
                    if (normalized.toLower().startsWith("content-length:"))
                        contentLength = normalized.mid(sizeof("content-length:") - 1).trimmed().toInt();
                }
                if (request.size() < headerEnd + 4 + contentLength) return;
                authSeen = request.contains("Authorization: Bearer test-key");
                if (mode.load() == 4) return; // timeout path deliberately leaves the socket open
                if (mode.load() == 1) {
                    const QByteArray body = "not-json";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                    return;
                }
                if (mode.load() == 2) {
                    const QByteArray body =
                        "{\"data\":[{\"index\":0,\"embedding\":[1e39,0]}]}";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Content-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                    return;
                }
                const auto bodyStart = request.indexOf("\r\n\r\n") + 4;
                const auto requestJson = QJsonDocument::fromJson(request.mid(bodyStart));
                const auto input = requestJson.object().value("input").toArray();
                QJsonArray data;
                for (int i = input.size() - 1; i >= 0; --i)
                    data.append(QJsonObject{{"index", i},
                                            {"embedding", i == 0 ? QJsonArray{1.0, 0.0}
                                                                   : QJsonArray{0.0, 1.0}}});
                const QJsonObject body{{"data", data}};
                const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
                const QByteArray status = mode.load() == 3 ? "500 Internal Server Error" : "200 OK";
                const QByteArray response = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n"
                                             "Content-Length: " + QByteArray::number(payload.size()) +
                                             "\r\nConnection: close\r\n\r\n" + payload;
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    });

    reader::DocumentModel model;
    model.document.id = "embedding-http-test";
    model.document.fileHash = "embedding-http-hash";
    model.blocks = {{"car", 0, {}, "car"}, {"sky", 0, {}, "sky"}};
    reader::EmbeddingProviderQtConfig config;
    config.baseUrl = "http://127.0.0.1:" + std::to_string(server.serverPort());
    config.model = "local-test";
    config.dimension = 2;
    config.apiKey = "test-key";
    reader::EmbeddingProviderQt provider(config);
    reader::VectorIndex index;
    std::string error;
    CHECK(index.buildSemantic(model, provider, {}, 0, &error));
    CHECK(authSeen);
    const auto hits = index.querySemantic(model, "automobile", provider, 1, {}, &error);
    CHECK(hits.size() == 1 && hits.front().anchor.block == "car");

    reader::EmbeddingProviderQtConfig badConfig = config;
    badConfig.dimension = 3;
    reader::EmbeddingProviderQt badProvider(badConfig);
    reader::VectorIndex badIndex;
    CHECK(!badIndex.buildSemantic(model, badProvider, {}, 0, &error));
    CHECK(error == "embedding response dimension mismatch");
    std::vector<float> output;
    mode = 1;
    CHECK(!provider.embed("bad-json", output, {}, error));
    CHECK(error == "embedding response is not valid JSON");
    mode = 2;
    CHECK(!provider.embed("nonfinite", output, {}, error));
    CHECK(error == "embedding response contains a non-finite value");
    mode = 3;
    CHECK(!provider.embed("http-error", output, {}, error));
    CHECK(error == "embedding HTTP 500");
    mode = 4;
    auto timeoutConfig = config;
    timeoutConfig.timeoutMs = 20;
    reader::EmbeddingProviderQt timeoutProvider(timeoutConfig);
    CHECK(!timeoutProvider.embed("timeout", output, {}, error));
    CHECK(error == "embedding request timed out");
    mode = 4;
    reader::CancellationToken inFlightCancel;
    std::thread canceller([&inFlightCancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        inFlightCancel.cancel();
    });
    auto cancelConfig = config;
    cancelConfig.timeoutMs = 1000;
    reader::EmbeddingProviderQt cancelProvider(cancelConfig);
    CHECK(!cancelProvider.embed("in-flight-cancel", output, inFlightCancel, error));
    canceller.join();
    CHECK(error == "embedding cancelled");
    reader::CancellationToken cancelled;
    cancelled.cancel();
    mode = 0;
    CHECK(!provider.embed("cancelled", output, cancelled, error));
    CHECK(error == "embedding cancelled");
    return failures == 0 ? 0 : 1;
}
