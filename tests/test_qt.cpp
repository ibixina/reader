// Qt integration test: real QPdfDocument -> spans -> blocks -> structure ->
// local ingest -> retrieval -> chat with source anchors (§66 prototype).
// Headless via QT_QPA_PLATFORM=offscreen. Built only when Qt6 is present.
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>

#include <QElapsedTimer>
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
#include "../tests/academic_fixture.h"
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

    // Outline: the academic fixture carries three native bookmarks. The
    // engine must return all of them promptly — a 1.5 s stall (and an
    // empty result) used to run on every cold open of documents whose
    // bookmark model did not populate within the wait window.
    {
        const std::string fixturePdf = std::string(home ? home : "/tmp") + "/qt_outline.pdf";
        reader_test::writeAcademicPdf(fixturePdf);
        QPdfDocument doc2;
        auto engine2 = std::make_shared<QtPdfEngine>(std::shared_ptr<QPdfDocument>(
            &doc2, [](QPdfDocument*) {}));
        CHECK(engine2->open(fixturePdf));
        QElapsedTimer outlineTimer;
        outlineTimer.start();
        const auto entries = engine2->outline();
        const qint64 outlineMs = outlineTimer.elapsed();
        CHECK(entries.size() == 3);
        CHECK(outlineMs < 1000);
        if (entries.size() == 3) {
            CHECK(entries[0].title == "Introduction");
            CHECK(entries[0].page == 0);
            CHECK(entries[1].title == "Methods");
            CHECK(entries[1].page == 1);
            CHECK(entries[2].title == "Results");
            CHECK(entries[2].page == 2);
        }
    }

    // Line geometry: superscripted affiliation marks and a rotated arXiv
    // margin stamp split pdfium's fragments away from text lines. Every
    // body line must still sit on its own baseline — shifted bounds made
    // clicks on the abstract wash an unrelated paragraph.
    {
        const std::string shiftedPdf = std::string(home ? home : "/tmp") + "/qt_shifted.pdf";
        writeTextPdf(shiftedPdf,
            "BT /F1 18 Tf 72 720 Td (Shifted Geometry Paper) Tj ET\n"
            "BT /F1 12 Tf 72 690 Td (Jane Doe) Tj /F1 7 Tf 4 Ts (a,1) Tj /F1 12 Tf 0 Ts "
            "(, John Roe) Tj /F1 7 Tf 4 Ts (b,2) Tj ET\n"
            "BT /F1 20 Tf 0 1 -1 0 40 250 Tm (arXiv:0000.00000v1 [cs.NE]) Tj ET\n"
            "BT /F1 12 Tf 72 600 Td (Abstract) Tj ET\n"
            "BT /F1 10 Tf 72 580 Td (First abstract line about learning.) Tj ET\n"
            "BT /F1 10 Tf 72 566 Td (Second abstract line about circuits.) Tj ET\n"
            "BT /F1 12 Tf 72 400 Td (Body paragraph with a citation [7].) Tj ET");
        QPdfDocument shiftedDoc;
        QtPdfEngine shifted(std::shared_ptr<QPdfDocument>(&shiftedDoc, [](QPdfDocument*) {}));
        CHECK(shifted.open(shiftedPdf));
        const auto spans = shifted.extractSpans(0);
        auto baselineOf = [&](const std::string& needle) -> float {
            for (const auto& s : spans)
                if (s.text.find(needle) != std::string::npos)
                    return s.bounds.y + s.bounds.height;
            return -1;
        };
        // PDF baselines (bottom-up) mapped to top-down page points.
        const std::pair<const char*, float> expected[] = {
            {"Abstract", 792 - 600}, {"First abstract", 792 - 580},
            {"Second abstract", 792 - 566}, {"citation [7]", 792 - 400}};
        for (const auto& [needle, baseline] : expected) {
            const float bottom = baselineOf(needle);
            CHECK(bottom > 0);
            CHECK(std::fabs(bottom - baseline) < 5.0f);
        }
    }

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
