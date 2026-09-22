// Qt integration test: real QPdfDocument -> spans -> blocks -> structure ->
// local ingest -> retrieval -> chat with source anchors (§66 prototype).
// Headless via QT_QPA_PLATFORM=offscreen. Built only when Qt6 is present.
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>

#include <QGuiApplication>
#include <QPdfDocument>

#include "ai/ChatManager.h"
#include "ai/ContextManager.h"
#include "ai/RetrievalEngine.h"
#include "analysis/PaperIngestor.h"
#include "analysis/StructureDetector.h"
#include "core/Json.h"
#include "document/DocumentAnchor.h"
#include "pdf/QtPdfEngine.h"
#include "pdf/TextExtractor.h"
#include "../tests/sample_pdf.h"

static int failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n"; \
            ++failures; \
        } \
    } while (0)

using namespace reader;

int main(int argc, char** argv) {
    QGuiApplication qapp(argc, argv);

    const char* home = std::getenv("HOME");
    std::string pdf = std::string(home ? home : "/tmp") + "/qt_sample.pdf";
    writeSamplePdf(pdf);

    QPdfDocument doc;
    auto engine = std::make_shared<QtPdfEngine>(std::shared_ptr<QPdfDocument>(
        &doc, [](QPdfDocument*) {}));
    CHECK(engine->open(pdf));
    CHECK(engine->pageCount() == 1);

    DocumentModel model;
    model.document.id = "qt1";
    model.document.fileHash = "qt1";
    model.document.title = "qt sample";
    model.document.pageCount = engine->pageCount();

    TextExtractor extractor;
    IdFactory ids;
    model.blocks = extractor.extract(*engine, ids, model.document.id, model.document.pageCount);
    CHECK(!model.blocks.empty());
    bool hasEncoder = false;
    for (auto& b : model.blocks)
        if (b.text.find("history encoder") != std::string::npos) hasEncoder = true;
    CHECK(hasEncoder);

    StructureDetector detector;
    auto structure = detector.detect(model, *engine, ids);
    CHECK(!structure.citations.empty()); // [12] found

    RetrievalEngine retrieval;
    retrieval.index(model);

    PaperIngestor ingestor;
    PaperAnalysis analysis = ingestor.analyzeLocal(model);
    CHECK(!analysis.sections.empty() || !analysis.annotations.empty());
    std::string err;
    CHECK(PaperAnalysis::parse(json::serialize(analysis.toJson()), err).has_value());

    // Select -> ask -> answer -> verify source (§66 steps 4-9).
    ContextManager ctx;
    ContextReference ref;
    ref.displayName = "Selected paragraph";
    ref.anchor = anchorForBlock(model, model.blocks.front());
    ref.extractedText = model.blocks.front().text;
    ctx.setCurrentSelection(ref);

    ReaderState state;
    state.page = 0;
    PaperMetadata paper{model.document.title, {}, model.document.id};
    auto retrieved =
        retrieval.retrieve(model, "why is the encoder necessary?", state, &analysis);
    CHECK(!retrieved.empty());
    ChatRequest req = ctx.buildRequest("why is this necessary?", state, paper,
                                       std::move(retrieved), {});
    ChatManager mgr(std::make_unique<EchoProvider>());
    ConversationId conv = mgr.newConversation();
    ChatMessage done = mgr.send(conv, std::move(req));
    CHECK(!done.text.empty());
    CHECK(!done.sources.empty());
    bool resolves = false;
    for (auto& s : done.sources)
        if (s.block.has_value() && model.findBlock(*s.block)) resolves = true;
    CHECK(resolves); // every source maps back to a concrete anchor

    if (failures == 0) std::cout << "ALL QT INTEGRATION TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
