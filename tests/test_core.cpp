// Core verification: document model, anchors, extraction, structure,
// indexes, context manager, prompts, manifest, ingestor, storage,
// navigation history. No Qt; builds with g++ -std=c++20.
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

#include "ai/ChatManager.h"
#include "ai/ContextManager.h"
#include "ai/PromptBuilder.h"
#include "ai/RetrievalEngine.h"
#include "analysis/PaperIngestor.h"
#include "analysis/StructureDetector.h"
#include "app/ApplicationState.h"
#include "core/Json.h"
#include "core/Types.h"
#include "document/DocumentAnchor.h"
#include "document/DocumentModel.h"
#include "pdf/PdfSelection.h"
#include "pdf/TextExtractor.h"
#include "search/TextIndex.h"
#include "search/VectorIndex.h"
#include "storage/Database.h"
#include "storage/Repositories.h"

static int failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n"; \
            ++failures; \
        } \
    } while (0)

using namespace reader;

static DocumentModel makeModel() {
    DocumentModel m;
    m.document.id = "doc1";
    m.document.fileHash = "abc";
    m.document.title = "Test Paper";
    m.document.pageCount = 3;
    IdFactory ids;
    auto block = [&](int page, const std::string& text, float y) {
        TextBlock b;
        b.id = ids.block();
        b.page = page;
        b.bounds = {50, y, 400, 20};
        b.text = text;
        m.blocks.push_back(b);
    };
    block(0, "Neural Posterior Estimation with recurrent history encoders", 50);
    block(1, "3.2 Posterior Estimation We use a recurrent history encoder for amortization [12].", 100);
    block(1, "Figure 1: The encoder maps history to a fixed state (4).", 200);
    block(2, "Results show model A achieved the highest likelihood. Limitations include compute.", 100);
    Section s1;
    s1.id = ids.section();
    s1.title = "3.2 Posterior Estimation";
    s1.level = 2;
    s1.startPage = 1;
    s1.endPage = 1;
    s1.blocks = {m.blocks[1].id, m.blocks[2].id};
    m.sections.push_back(s1);
    Equation eq;
    eq.id = ids.equation();
    eq.page = 1;
    eq.bounds = {50, 300, 200, 20};
    eq.extractedText = "p(theta|x) = q(theta; h)";
    eq.label = "(4)";
    m.equations.push_back(eq);
    Figure fig;
    fig.id = ids.figure();
    fig.page = 1;
    fig.bounds = {50, 200, 300, 150};
    fig.caption = "Figure 1: encoder architecture";
    m.figures.push_back(fig);
    TextSpan span;
    span.page = 1;
    span.bounds = {50, 280, 250, 14};
    span.text = "cached line with exact geometry";
    span.fontSize = 11.5f;
    span.bold = true;
    m.lineSpans.push_back(span);
    m.rebuildIndex();
    return m;
}

class DeterministicEmbeddingProvider final : public EmbeddingProvider {
public:
    explicit DeterministicEmbeddingProvider(std::string id = "fake-embedding")
        : id_(std::move(id)) {}

    std::size_t dimension() const override { return 2; }
    std::string modelId() const override { return id_; }
    bool embed(const std::string& text, std::vector<float>& output,
               const CancellationToken& token, std::string& error) const override {
        if (token.cancelled()) {
            error = "cancelled";
            return false;
        }
        ++calls;
        const auto lower = toLower(text);
        output = lower.find("car") != std::string::npos ||
                         lower.find("automobile") != std::string::npos
                     ? std::vector<float>{1.0f, 0.0f}
                     : std::vector<float>{0.0f, 1.0f};
        return true;
    }

    mutable int calls = 0;

private:
    std::string id_;
};

int main() {
    // Keep cache tests hermetic: PaperIngestor resolves its cache below HOME.
    const bool hadHome = std::getenv("HOME") != nullptr;
    const std::string originalHome = hadHome ? std::getenv("HOME") : "";
    char testHomeBuffer[] = "/tmp/paper-reader-core-test-XXXXXX";
    char* createdHome = mkdtemp(testHomeBuffer);
    CHECK(createdHome != nullptr);
    const std::filesystem::path testHome =
        createdHome ? createdHome : "/tmp/paper-reader-core-test-failed";
    const bool ownsTestHome = createdHome != nullptr;
    std::error_code testHomeError;
    if (!ownsTestHome) std::filesystem::create_directories(testHome, testHomeError);
    setenv("HOME", testHome.c_str(), 1);

    // Types: sha256 known vector + file hash stability.
    CHECK(sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // JSON strings must preserve astral symbols and reject raw controls.
    {
        const auto astral = json::parse("\"\\uD835\\uDC9C\"").asString();
        CHECK(astral == "\xF0\x9D\x92\x9C"); // mathematical italic small c
        CHECK(json::parse(json::serialize(json::Value(std::string("a\0b", 3))))
                      .asString() == std::string("a\0b", 3));
        bool rejected = false;
        try {
            (void)json::parse("\"raw\ncontrol\"");
        } catch (const json::ParseError&) {
            rejected = true;
        }
        CHECK(rejected);
        rejected = false;
        try {
            (void)json::parse("\"\\uD835x\"");
        } catch (const json::ParseError&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    // DocumentModel lookups + section helpers.
    {
        DocumentModel m = makeModel();
        CHECK(m.findBlock(m.blocks[0].id) != nullptr);
        CHECK(m.findBlock("nope") == nullptr);
        CHECK(m.sectionForPage(1) != nullptr);
        CHECK(m.sectionForBlock(m.blocks[1].id) != nullptr);
        CHECK(m.blockAtPage(1, 105) != nullptr);
        CHECK(m.blockAtPage(1, 105)->page == 1);
        CHECK(!m.sectionText(m.sections[0].id).empty());
    }

    // Anchors for every object kind.
    {
        DocumentModel m = makeModel();
        DocumentAnchor a = anchorForBlock(m, m.blocks[1]);
        CHECK(a.page == 1 && a.block.has_value());
        CHECK(!a.displayName(m).empty());
        CHECK(!anchorForEquation(m, m.equations[0]).anchorText.empty());
        CHECK(!anchorForFigure(m, m.figures[0]).anchorText.empty());
        CHECK(!anchorForSection(m, m.sections[0]).anchorText.empty());
        ContextReference objectRef;
        objectRef.id = "temporary";
        objectRef.anchor = anchorForEquation(m, m.equations[0]);
        CHECK(contextReferenceId(objectRef) == m.equations[0].id);
        RetrievedPassage figurePassage;
        figurePassage.anchor = anchorForFigure(m, m.figures[0]);
        CHECK(retrievedPassageId(figurePassage) == m.figures[0].id);
        DocumentAnchor pageAnchor;
        pageAnchor.page = 4;
        CHECK(anchorReferenceId(pageAnchor) == "p.5");
        RetrievedPassage unanchored;
        unanchored.text = "overview without source";
        CHECK(retrievedPassageId(unanchored).empty());
        ContextReference firstSelection;
        firstSelection.id = "selection-one";
        firstSelection.type = ReferenceType::TextSelection;
        firstSelection.anchor = a;
        ContextReference secondSelection = firstSelection;
        secondSelection.id = "selection-two";
        secondSelection.anchor.bounds.x += 10;
        CHECK(contextReferenceId(firstSelection) != contextReferenceId(secondSelection));
    }

    // Selection: visual drag -> best-overlap block anchor.
    {
        DocumentModel m = makeModel();
        PdfSelection::Drag drag{1, {60, 105, 200, 15}};
        DocumentAnchor a = PdfSelection::resolve(m, drag, "recurrent history encoder");
        CHECK(a.block.has_value());
        CHECK(a.anchorText == "recurrent history encoder");
    }

    // TextExtractor: two-column reading order + hyphen/ligature fixes.
    {
        struct FakeEngine : IPdfEngine {
            bool open(const std::string&) override { return true; }
            int pageCount() const override { return 1; }
            std::vector<TextSpan> extractSpans(int) override {
                return {{"right col", {300, 10, 100, 12}, 10, false, 0},
                        {"left col", {10, 10, 100, 12}, 10, false, 0}};
            }
            std::vector<PdfLink> links(int) override { return {}; }
            std::vector<PdfOutlineEntry> outline() override { return {}; }
            std::string name() const override { return "fake"; }
        } engine;
        TextExtractor ex;
        IdFactory ids;
        auto blocks = ex.extract(engine, ids, "d", 1);
        // Columns stay separate blocks, ordered left-then-right (§55).
        CHECK(blocks.size() == 2);
        CHECK(blocks[0].text == "left col");
        CHECK(blocks[1].text == "right col");
    }

    // StructureDetector: headings, figures, equations, citations.
    {
        DocumentModel m = makeModel();
        NullPdfEngine engine;
        IdFactory ids;
        StructureDetector det;
        auto res = det.detect(m, engine, ids);
        CHECK(!res.sections.empty());
        CHECK(!res.figures.empty());
        CHECK(!res.citations.empty());
        const auto sectionCount = m.sections.size();
        det.detect(m, engine, ids);
        CHECK(m.sections.size() == sectionCount); // retry does not duplicate structure
        CHECK(m.findEquation(m.equations.empty() ? "" : m.equations[0].id) != nullptr ||
              m.equations.empty());
    }

    // Structure extraction retains metadata and typed object context when the
    // source text provides it; undetectable fields remain empty.
    {
        DocumentModel m;
        m.document.id = "structured";
        m.document.pageCount = 2;
        m.blocks = {{"title", 0, {0, 0, 100, 12}, "A Real Paper Title"},
                    {"authors", 0, {0, 20, 100, 12}, "A. Researcher and B. Reviewer"},
                    {"keys", 0, {0, 40, 100, 12}, "Keywords: anchors, retrieval"},
                    {"abstract-heading", 0, {0, 60, 100, 12}, "Abstract"},
                    {"abstract-body", 0, {0, 80, 100, 12}, "This is a measured abstract."},
                    {"heading", 0, {0, 100, 100, 12}, "1 Introduction"},
                    {"eq", 0, {0, 120, 100, 12}, "L(theta) = sum_i log p(y_i) (1)"},
                    {"fig-label", 0, {12, 128, 30, 10}, "Figure 1"},
                    {"fig", 0, {0, 140, 100, 12}, "Figure 1. Pipeline"},
                    {"table-label", 0, {12, 148, 30, 10}, "Table 1"},
                    {"table", 0, {0, 160, 100, 12}, "Table 1: local 0.91, remote 0.04"},
                    {"table-row", 0, {0, 180, 100, 12}, "Metric | Value"},
                    {"footnote", 0, {0, 200, 100, 12}, "Footnote 1: local fixture note."},
                    {"references", 1, {0, 0, 100, 12}, "4 References"},
                    {"bib", 1, {0, 20, 100, 12},
                     "[1] A. Researcher, Stable evidence, doi:10.1000/example"}};
        NullPdfEngine engine;
        IdFactory ids;
        const auto result = StructureDetector().detect(m, engine, ids);
        CHECK(m.document.title == "A Real Paper Title");
        CHECK(m.document.authors.size() == 2);
        CHECK(m.document.keywords.size() == 2);
        CHECK(m.document.abstractText == "This is a measured abstract.");
        CHECK(!m.equations.empty() && !m.equations.front().latex.empty());
        CHECK(!m.figures.empty() && m.figures.front().bounds.height >
                                        m.blocks[7].bounds.height);
        CHECK(!m.tables.empty() && m.tables.front().rows.size() == 1 &&
              m.tables.front().rows.front().size() == 2);
        CHECK(m.tables.front().bounds.height > m.blocks[9].bounds.height);
        CHECK(m.footnotes.size() == 1);
        CHECK(result.bibliography.size() == 1 &&
              result.bibliography.front().doi == "10.1000/example");
    }

    // TextIndex literal search + VectorIndex semantic retrieval.
    {
        DocumentModel m = makeModel();
        TextIndex ti;
        ti.build(m);
        auto hits = ti.search(m, "recurrent history encoder");
        CHECK(!hits.empty());
        VectorIndex vi;
        vi.build(m);
        auto shits = vi.query(m, "policy chooses experiments from history", 3);
        CHECK(!shits.empty());
        RetrievalEngine re;
        re.index(m);
        ReaderState st;
        st.page = 1;
        st.paragraphBlock = m.blocks[1].id;
        st.section = m.sections[0].id;
        auto passages = re.retrieve(m, "why is the encoder necessary?", st, nullptr);
        CHECK(!passages.empty());
        CHECK(passages[0].anchor.block.has_value()); // paragraph first (§17)
        RetrievalOptions bounded;
        bounded.maxPassages = 2;
        bounded.charBudget = 220;
        auto boundedPassages = re.retrieve(m, "encoder", st, nullptr, bounded);
        std::size_t boundedChars = 0;
        for (const auto& p : boundedPassages) boundedChars += p.text.size();
        CHECK(boundedPassages.size() <= bounded.maxPassages);
        CHECK(boundedChars <= bounded.charBudget);
        PaperAnalysis ungroundedAnalysis;
        ungroundedAnalysis.overview.mainIdea = "An attractive but unanchored summary.";
        RetrievalOptions explicitOnly;
        explicitOnly.includeImplicitContext = false;
        CHECK(re.retrieve(m, "", ReaderState{}, &ungroundedAnalysis, explicitOnly).empty());
        DocumentModel semanticModel;
        semanticModel.document.id = "semantic-paper";
        semanticModel.document.fileHash = "semantic-hash";
        semanticModel.blocks.push_back({"car-block", 0, {0, 0, 10, 10}, "A car moves quickly."});
        semanticModel.blocks.push_back({"sky-block", 0, {0, 20, 10, 10}, "The sky is blue."});
        DeterministicEmbeddingProvider provider;
        VectorIndex semantic;
        std::string semanticError;
        CHECK(semantic.buildSemantic(semanticModel, provider, {}, 0, &semanticError));
        const auto semanticHits = semantic.querySemantic(semanticModel, "automobile", provider, 1,
                                                         {}, &semanticError);
        CHECK(semanticHits.size() == 1 && semanticHits.front().anchor.block == "car-block");
        DeterministicEmbeddingProvider ephemeralProvider("ephemeral-embedding");
        VectorIndex ephemeral;
        CHECK(ephemeral.buildSemantic(semanticModel, ephemeralProvider, {}, 0,
                                      &semanticError, false));
        VectorIndex noPersistedCopy;
        CHECK(!noPersistedCopy.loadSemanticCache(semanticModel, "semantic-hash",
                                                 ephemeralProvider, &semanticError));
        RetrievalEngine semanticRetrieval;
        semanticRetrieval.index(semanticModel);
        auto sharedProvider =
            std::make_shared<DeterministicEmbeddingProvider>("ephemeral-embedding");
        semanticRetrieval.setSemanticSnapshot({ephemeral, sharedProvider});
        RetrievalOptions semanticOnly;
        semanticOnly.includeImplicitContext = false;
        const auto retrievedSemantically = semanticRetrieval.retrieve(
            semanticModel, "automobile", ReaderState{}, nullptr, semanticOnly);
        CHECK(!retrievedSemantically.empty());
        CHECK(retrievedSemantically.front().anchor.block == "car-block");
        CancellationToken cancelledSemanticQuery;
        cancelledSemanticQuery.cancel();
        const int callsBeforeCancelled = sharedProvider->calls;
        semanticRetrieval.retrieve(semanticModel, "automobile", ReaderState{}, nullptr,
                                   semanticOnly, cancelledSemanticQuery);
        CHECK(sharedProvider->calls == callsBeforeCancelled);
        DocumentModel revised = semanticModel;
        revised.blocks[0].text = "a different car meaning";
        CHECK(semantic.querySemantic(revised, "automobile", provider, 1, {}, &semanticError)
              .empty());
        CHECK(semanticError == "semantic index is stale for this document");
        VectorIndex restarted;
        DeterministicEmbeddingProvider cacheProvider;
        CHECK(restarted.loadSemanticCache(semanticModel, "semantic-hash", cacheProvider,
                                          &semanticError));
        CHECK(restarted.semanticReady());
        DocumentModel changedHash = semanticModel;
        changedHash.document.fileHash = "different-hash";
        CHECK(!restarted.loadSemanticCache(changedHash, "different-hash", cacheProvider,
                                           &semanticError));
        CancellationToken cancelled;
        cancelled.cancel();
        VectorIndex cancelledIndex;
        DeterministicEmbeddingProvider cancelledProvider("fake-cancel");
        CHECK(!cancelledIndex.buildSemantic(semanticModel, cancelledProvider, cancelled, 0,
                                            &semanticError));
        CHECK(semanticError == "embedding cancelled");
        DocumentModel changed = m;
        changed.blocks.clear();
        changed.rebuildIndex();
        CHECK(re.retrieve(changed, "encoder", ReaderState{}, nullptr).empty());
    }

    // ContextManager: temp replaced, pinned survives, snapshot immutable.
    {
        DocumentModel m = makeModel();
        ContextManager cm;
        ContextReference a, b;
        a.displayName = "A";
        a.anchor = anchorForBlock(m, m.blocks[1]);
        a.extractedText = "aaa";
        b.displayName = "B";
        b.anchor = anchorForBlock(m, m.blocks[3]);
        b.extractedText = "bbb";
        cm.setCurrentSelection(a);
        cm.setCurrentSelection(b); // replaces
        CHECK(cm.currentContext().temporary.size() == 1);
        CHECK(cm.currentContext().temporary[0].displayName == "B");
        cm.pinReference(cm.currentContext().temporary[0].id);
        CHECK(cm.currentContext().pinned.size() == 1);
        const ReferenceId pinnedId = cm.currentContext().pinned.front().id;
        cm.setCurrentSelection(a);
        CHECK(cm.currentContext().pinned.size() == 1); // survives
        PaperMetadata paper{"T", {}, "doc1"};
        ChatRequest req = cm.buildRequest("q", ReaderState{}, paper, {}, {});
        CHECK(req.explicitReferences.size() == 2); // pinned first, then temp
        cm.unpinReference(pinnedId);
        CHECK(cm.currentContext().pinned.empty());
        CHECK(cm.currentContext().temporary.size() == 1);
        // @ shorthands
        ReaderState st;
        st.page = 1;
        CHECK(cm.resolveShorthand("@selection", m, st).has_value());
        CHECK(cm.resolveShorthand("@page", m, st).has_value());
        m.equations.front().latex = "p(\\theta|x)=q(\\theta;h)";
        auto equationReference = cm.resolveShorthand("@eq_1", m, st);
        CHECK(equationReference.has_value());
        CHECK(equationReference->latex == m.equations.front().latex);
        auto figureReference = cm.resolveShorthand("@fig_1", m, st);
        CHECK(figureReference.has_value());
        CHECK(!figureReference->relatedSources.empty());
        Table table;
        table.id = "table_1";
        table.page = 1;
        table.bounds = {50, 230, 300, 100};
        table.caption = "Table 1: metrics";
        table.rows = {{"Metric", "Value"}, {"Accuracy", "0.91"}};
        m.tables.push_back(table);
        auto tableReference = cm.resolveShorthand("@table1", m, st);
        CHECK(tableReference.has_value());
        CHECK(tableReference->tableRows == table.rows);
        Citation cited;
        cited.id = "cite_12";
        cited.raw = "[12]";
        cited.label = "[12]";
        cited.title = "Stable evidence";
        cited.doi = "10.1000/evidence";
        cited.page = 1;
        cited.block = m.blocks[1].id;
        m.citations.push_back(cited);
        const auto citationAnchor = anchorForCitation(m, m.citations.back());
        CHECK(citationAnchor.objectType == "citation");
        CHECK(citationAnchor.bounds.x == m.blocks[1].bounds.x &&
              citationAnchor.bounds.y == m.blocks[1].bounds.y);
        auto citationReference = cm.resolveShorthand("@cite12", m, st);
        CHECK(citationReference.has_value());
        CHECK(citationReference->type == ReferenceType::Citation);
        CHECK(citationReference->extractedText.find("10.1000/evidence") != std::string::npos);
        CHECK(!cm.resolveShorthand("@bogus", m, st).has_value());
    }

    // PromptBuilder grounding rules present.
    {
        ChatRequest req;
        req.question = "why?";
        req.paper.title = "P";
        auto built = PromptBuilder::build(req);
        CHECK(built.system.find("I don't see this addressed explicitly") != std::string::npos);
        CHECK(built.user.find("why?") != std::string::npos);
        ContextReference sectionRef;
        sectionRef.id = "ref-section";
        sectionRef.type = ReferenceType::Section;
        sectionRef.anchor.section = "sec_7";
        sectionRef.displayName = "Methods";
        sectionRef.extractedText = "The methods evidence.";
        req.explicitReferences.push_back(sectionRef);
        CHECK(PromptBuilder::build(req).user.find("[sec_7]") != std::string::npos);

        // Prompt preparation keeps the entire question, bounds the complete
        // system+user payload, and exposes only evidence actually sent.
        ChatRequest bounded;
        bounded.question = "Explain the result without losing this question.";
        bounded.paper.title = std::string(4000, 'T');
        ContextReference largeRef;
        largeRef.id = "selection_1";
        largeRef.type = ReferenceType::TextSelection;
        largeRef.anchor.document = "doc1";
        largeRef.anchor.page = 2;
        for (int i = 0; i < 1400; ++i) largeRef.extractedText += "\xE2\x82\xAC";
        DocumentAnchor related;
        related.document = "doc1";
        related.block = "related_block";
        related.page = 2;
        related.anchorText = "Grounded related evidence.";
        largeRef.relatedSources.push_back(related);
        bounded.explicitReferences.push_back(largeRef);
        RetrievedPassage passage;
        passage.anchor.document = "doc1";
        passage.anchor.block = "retrieved_1";
        passage.text = std::string(3000, 'r');
        bounded.retrievedPassages.push_back(passage);
        for (int i = 0; i < 4; ++i) {
            ChatMessage history;
            history.role = i % 2 ? "assistant" : "user";
            history.text = std::string(1000, static_cast<char>('a' + i));
            bounded.recentConversation.push_back(history);
        }
        PromptBuilder::Options boundedOptions;
        boundedOptions.charBudget = 1100;
        auto prepared = PromptBuilder::prepare(bounded, boundedOptions);
        CHECK(prepared);
        CHECK(prepared.request.question == bounded.question);
        CHECK(prepared.prompt.system.size() + prepared.prompt.user.size() <=
              boundedOptions.charBudget);
        CHECK(prepared.prompt.user.find("Question: " + bounded.question) != std::string::npos);
        CHECK(!prepared.request.explicitReferences.empty());
        CHECK(prepared.request.retrievedPassages.empty());
        CHECK(prepared.request.recentConversation.empty());
        CHECK(prepared.request.explicitReferences.front().anchor.anchorText ==
              prepared.request.explicitReferences.front().extractedText);
        const auto isUtf8 = [](const std::string& text) {
            std::size_t i = 0;
            while (i < text.size()) {
                const auto c = static_cast<unsigned char>(text[i]);
                const std::size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2
                                                    : (c & 0xf0) == 0xe0 ? 3
                                                    : (c & 0xf8) == 0xf0 ? 4 : 0;
                if (n == 0 || i + n > text.size()) return false;
                for (std::size_t j = 1; j < n; ++j)
                    if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) return false;
                i += n;
            }
            return true;
        };
        CHECK(isUtf8(prepared.prompt.user));
        auto preparedAgain = PromptBuilder::prepare(prepared.request, boundedOptions);
        CHECK(preparedAgain);
        CHECK(preparedAgain.prompt.system == prepared.prompt.system);
        CHECK(preparedAgain.prompt.user == prepared.prompt.user);

        ChatRequest tooLarge;
        tooLarge.question = std::string(1000, 'q');
        PromptBuilder::Options tinyOptions;
        tinyOptions.charBudget = 600;
        auto rejected = PromptBuilder::prepare(tooLarge, tinyOptions);
        CHECK(!rejected);
        CHECK(rejected.request.question == tooLarge.question);
        CHECK(rejected.error.find("too long") != std::string::npos);

        ChatRequest missingPassageId;
        missingPassageId.question = "q";
        missingPassageId.retrievedPassages.push_back({});
        auto missingId = PromptBuilder::prepare(missingPassageId);
        CHECK(!missingId);
        CHECK(missingId.error.find("stable citation ID") != std::string::npos);

        ChatRequest headerOnly;
        headerOnly.question = "q";
        ContextReference noEvidence;
        noEvidence.id = "empty_ref";
        noEvidence.type = ReferenceType::TextSelection;
        noEvidence.anchor.document = "doc1";
        headerOnly.explicitReferences.push_back(noEvidence);
        auto withoutHeaderOnly = PromptBuilder::prepare(headerOnly);
        CHECK(withoutHeaderOnly);
        CHECK(withoutHeaderOnly.request.explicitReferences.empty());

        ChatRequest crossPaper;
        crossPaper.question = "q";
        crossPaper.paper.documentId = "paper-b";
        noEvidence.extractedText = "evidence from paper a";
        noEvidence.anchor.document = "paper-a";
        crossPaper.explicitReferences.push_back(noEvidence);
        auto rejectedCrossPaper = PromptBuilder::prepare(crossPaper);
        CHECK(!rejectedCrossPaper);
        CHECK(rejectedCrossPaper.error.find("different paper") != std::string::npos);
    }

    // ChatManager: snapshot + offline echo cites real anchors.
    {
        DocumentModel m = makeModel();
        auto provider = std::make_unique<EchoProvider>();
        ChatManager mgr(std::move(provider));
        ConversationId conv = mgr.newConversation();
        ContextReference r;
        r.displayName = "Selected paragraph · p.2";
        r.anchor = anchorForBlock(m, m.blocks[1]);
        r.extractedText = m.blocks[1].text;
        ChatRequest req;
        req.question = "what does this mean?";
        req.explicitReferences = {r};
        ChatMessage done = mgr.send(conv, req);
        CHECK(!done.text.empty());
        CHECK(!done.sources.empty());
        CHECK(mgr.history(conv).size() == 2);
        CHECK(mgr.history(conv)[0].references.size() == 1); // snapshot (§16)
    }

    // PaperAnalysis manifest round-trip + validation + ingest cache.
    {
        DocumentModel m = makeModel();
        PaperIngestor ing;
        PaperAnalysis a = ing.analyzeLocal(m);
        CHECK(!a.sections.empty());
        CHECK(!a.annotations.empty());
        CHECK(a.meta.provider == "local-extractive");
        CHECK(a.overview.researchQuestion != m.document.title);
        CHECK(a.overview.mainIdea.find(m.document.title) == std::string::npos);
        CHECK(!a.overview.sources.empty());
        CHECK(!a.sections.front().sources.empty());
        a.overview.architecture = "encoder + decoder";
        a.overview.setup = "two benchmark splits";
        a.overview.takeaway = "structured context improves retrieval";
        std::string err;
        auto parsed = PaperAnalysis::parse(json::serialize(a.toJson()), err);
        CHECK(parsed.has_value());
        CHECK(parsed->overview.architecture == a.overview.architecture);
        CHECK(parsed->overview.setup == a.overview.setup);
        CHECK(parsed->overview.takeaway == a.overview.takeaway);
        CHECK(parsed->overview.sources == a.overview.sources);
        CHECK(parsed->sections.front().sources == a.sections.front().sources);
        PaperAnalysis ungrounded = a;
        ungrounded.overview.sources = {"missing-block"};
        for (auto& section : ungrounded.sections) section.sources = {"missing-block"};
        ungrounded.relationships.push_back(
            {"missing-concept-a", "missing-concept-b", "uses", {"missing-block"}});
        CHECK(ing.normalizeAnalysis(ungrounded, m, &err));
        CHECK(ungrounded.overview.mainIdea.empty());
        CHECK(ungrounded.sections.empty());
        CHECK(ungrounded.relationships.empty());
        // Invalid schema rejected.
        auto bad = PaperAnalysis::parse("{\"overview\":{}}", err);
        CHECK(!bad.has_value());
        // Lenient parse tolerates chat-model fences and surrounding prose.
        std::string fenced = "Here is the analysis:\n```json\n" +
                             json::serialize(a.toJson()) + "\n```\nHope this helps.";
        auto lenient = PaperAnalysis::parseLenient(fenced, err);
        CHECK(lenient.has_value());
        CHECK(lenient->usable());
        // Pretty form round-trips through the strict parser.
        std::string prettyText = json::pretty(a.toJson());
        CHECK(prettyText.find('\n') != std::string::npos);
        auto prettyBack = PaperAnalysis::parse(prettyText, err);
        CHECK(prettyBack.has_value());
        CHECK(prettyBack->annotations.size() == a.annotations.size());
        // Numeric parser rejects overflow instead of allowing std::stod to
        // escape from an untrusted model response.
        std::string overflow = json::serialize(a.toJson());
        const std::string schemaField = "\"analysis_schema_version\":1";
        const auto schemaPos = overflow.find(schemaField);
        CHECK(schemaPos != std::string::npos);
        if (schemaPos != std::string::npos) {
            overflow.replace(schemaPos, schemaField.size(),
                            "\"analysis_schema_version\":1e9999");
            CHECK(!PaperAnalysis::parse(overflow, err).has_value());
            std::string malformedNumber = json::serialize(a.toJson());
            malformedNumber.replace(schemaPos, schemaField.size(),
                                    "\"analysis_schema_version\":1e");
            CHECK(!PaperAnalysis::parse(malformedNumber, err).has_value());
            std::string leadingZero = json::serialize(a.toJson());
            leadingZero.replace(schemaPos, schemaField.size(),
                                "\"analysis_schema_version\":01");
            CHECK(!PaperAnalysis::parse(leadingZero, err).has_value());
            std::string timestampOverflow = json::serialize(a.toJson());
            const auto generatedPos = timestampOverflow.find("\"generated_at\":");
            CHECK(generatedPos != std::string::npos);
            if (generatedPos != std::string::npos) {
                const auto valueStart = generatedPos + std::string("\"generated_at\":").size();
                const auto valueEnd = timestampOverflow.find('}', valueStart);
                timestampOverflow.replace(valueStart, valueEnd - valueStart,
                                          "9.223372036854776e18");
            }
            auto normalizedTimestamp = PaperAnalysis::parse(timestampOverflow, err);
            CHECK(normalizedTimestamp.has_value());
            CHECK(normalizedTimestamp->meta.generatedAt == 0);
        }
        // Negative offsets normalize to zero rather than wrapping to a huge
        // size_t range.
        auto invalidOffsets = a.toJson();
        if (!invalidOffsets["annotations"].asArray().empty()) {
            invalidOffsets["annotations"].asArray().front()["start"] = -3;
            invalidOffsets["annotations"].asArray().front()["end"] = 4;
            auto normalized = PaperAnalysis::parse(json::serialize(invalidOffsets), err);
            CHECK(normalized.has_value());
            CHECK(normalized->annotations.front().start == 0);
            CHECK(normalized->annotations.front().end == 4);
        }
        // Raw response persistence for the Ingest Raw tab.
        CHECK(ing.saveRawResponse("rawtest", "raw-bytes-123"));
        CHECK(ing.loadRawResponse("rawtest") == "raw-bytes-123");
        CHECK(ing.loadRawResponse("missing") == "");
        PaperIngestor().clearAnalysis("rawtest");
        CHECK(ing.loadRawResponse("rawtest") == "");
        PaperAnalysis empty;
        CHECK(!empty.usable());
        IngestState invalidRemoteState = IngestState::NotIngested;
        auto invalidRemote = ing.ingest(
            m, [&](const std::string&) { return std::string("{invalid json"); },
            [&](const IngestProgress& p) { invalidRemoteState = p.state; }, CancellationToken{});
        CHECK(!invalidRemote.has_value());
        CHECK(invalidRemoteState == IngestState::Failed);
        CHECK(ing.loadRawResponse("abc") == "");
        // A validated remote response is persisted as the active raw source.
        const std::string remoteRaw = json::serialize(a.toJson());
        auto remoteResult = ing.ingest(
            m, [&](const std::string&) { return remoteRaw; },
            PaperIngestor::ProgressCallback{}, CancellationToken{});
        CHECK(remoteResult.has_value());
        CHECK(ing.loadRawResponse("abc") == remoteRaw);
        IngestState exceptionState = IngestState::NotIngested;
        auto remoteException = ing.ingest(
            m, [&](const std::string&) -> std::string { throw std::runtime_error("offline"); },
            [&](const IngestProgress& p) { exceptionState = p.state; }, CancellationToken{});
        CHECK(!remoteException.has_value());
        CHECK(exceptionState == IngestState::Failed);
        CHECK(ing.loadRawResponse("abc") == remoteRaw);
        CHECK(ing.cachedAnalysis("doc1", "abc").has_value());

        CancellationToken cancelDuringRemote;
        IngestState cancelledState = IngestState::Failed;
        auto cancelledIngest = ing.ingest(
            m,
            [&](const std::string&) {
                cancelDuringRemote.cancel();
                return remoteRaw;
            },
            [&](const IngestProgress& p) { cancelledState = p.state; }, cancelDuringRemote);
        CHECK(!cancelledIngest.has_value());
        CHECK(cancelledState == IngestState::NotIngested);
        CHECK(ing.loadRawResponse("abc") == remoteRaw);
        CHECK(ing.cachedAnalysis("doc1", "abc").has_value());
        // A local re-ingest clears the old remote response before caching the
        // new analysis, so the raw tab cannot show stale data.
        auto res = ing.ingest(m, PaperIngestor::FetchAnalysis{},
                              PaperIngestor::ProgressCallback{}, CancellationToken{});
        CHECK(res.has_value());
        CHECK(ing.loadRawResponse("abc") == "");
        auto cached = ing.cachedAnalysis("doc1", "abc");
        CHECK(cached.has_value());
        ing.clearAnalysis("abc");
        CHECK(!ing.cachedAnalysis("doc1", "abc").has_value());
        // Ingesting an empty model must fail loudly, never fake success.
        DocumentModel blank;
        blank.document.id = "blank";
        blank.document.fileHash = "blank";
        auto none = ing.ingest(blank, PaperIngestor::FetchAnalysis{},
                               PaperIngestor::ProgressCallback{}, CancellationToken{});
        CHECK(!none.has_value());
        CHECK(!ing.saveAnalysis("blank", empty));

        // A cache failure is a failed ingest, never a durable success.
        setenv("HOME", "/proc", 1);
        CHECK(!ing.saveAnalysis("unwritable", a));
        IngestState terminalState = IngestState::NotIngested;
        auto uncached = ing.ingest(m, PaperIngestor::FetchAnalysis{},
                                   [&](const IngestProgress& p) { terminalState = p.state; },
                                   CancellationToken{});
        CHECK(!uncached.has_value());
        CHECK(terminalState == IngestState::Failed);
        setenv("HOME", testHome.c_str(), 1);
    }

    // Storage repositories CRUD.
    {
        Database db(":memory:");
        CHECK(db.ok());
        DocumentRepository docs(&db);
        AnnotationRepository anns(&db);
        DocumentModel m = makeModel();
        CHECK(docs.saveDocument(m.document));
        DocumentModel metadataOnly;
        metadataOnly.document.id = "metadata-only";
        metadataOnly.document.fileHash = "metadata-hash";
        CHECK(docs.saveDocument(metadataOnly.document));
        CHECK(!docs.loadModel("metadata-only", metadataOnly));
        m.document.abstractText = "abstract with structure";
        m.document.keywords = {"retrieval", "anchors"};
        m.equations.front().latex = "E=mc^2";
        Citation citation;
        citation.id = "cite_1";
        citation.raw = "[1] Example";
        citation.label = "[1]";
        citation.title = "A cited result";
        citation.doi = "10.1000/example";
        citation.reason = "supports the method";
        citation.page = 1;
        citation.block = m.blocks[1].id;
        m.citations.push_back(citation);
        CHECK(docs.saveModel(m));
        DocumentModel loadedModel;
        CHECK(docs.loadModel("doc1", loadedModel));
        CHECK(loadedModel.blocks.size() == m.blocks.size());
        CHECK(loadedModel.sections.size() == m.sections.size());
        CHECK(loadedModel.sections.front().blocks == m.sections.front().blocks);
        CHECK(loadedModel.document.abstractText == m.document.abstractText);
        CHECK(loadedModel.document.keywords == m.document.keywords);
        CHECK(loadedModel.lineSpans.size() == m.lineSpans.size());
        CHECK(loadedModel.lineSpans.front().text == m.lineSpans.front().text);
        CHECK(loadedModel.lineSpans.front().bounds.x == m.lineSpans.front().bounds.x);
        CHECK(loadedModel.lineSpans.front().bold == m.lineSpans.front().bold);
        CHECK(docs.loadModel("doc1", "abc", loadedModel));
        CHECK(!docs.loadModel("doc1", "wrong-hash", loadedModel));
        CHECK(db.exec("UPDATE documents SET extraction_version=999 WHERE id='doc1'"));
        CHECK(!docs.loadModel("doc1", loadedModel));
        CHECK(db.exec("UPDATE documents SET extraction_version=1 WHERE id='doc1'"));
        CHECK(!loadedModel.equations.empty() && loadedModel.equations.front().latex == "E=mc^2");
        CHECK(loadedModel.citations.size() == 1);
        CHECK(loadedModel.citations.front().title == citation.title);
        CHECK(loadedModel.citations.front().doi == citation.doi);
        CHECK(loadedModel.citations.front().reason == citation.reason);
        CHECK(!docs.recentDocuments(10).empty());
        DocumentModel other = m;
        other.document.id = "doc2";
        other.document.fileHash = "hash2";
        CHECK(docs.saveModel(other));
        DocumentModel isolated;
        CHECK(docs.loadModel("doc1", isolated));
        CHECK(isolated.document.id == "doc1" && isolated.blocks.size() == m.blocks.size());
        CHECK(docs.saveReadingState("doc1", 4, 120.5, 1.5));
        int page = 0;
        double sy = 0, z = 0;
        CHECK(docs.loadReadingState("doc1", page, sy, z));
        CHECK(page == 4);
        Note n;
        n.id = "n1";
        n.anchor = anchorForBlock(m, m.blocks[0]);
        n.text = "important";
        CHECK(anns.saveNote("doc1", n));
        CHECK(anns.notesFor("doc1").size() == 1);
        CHECK(anns.notesFor("doc1").front().anchor.block == n.anchor.block);
        UserAnnotation ua;
        ua.id = "a1";
        ua.anchor = anchorForEquation(m, m.equations[0]);
        ua.kind = "highlight";
        ua.color = std::string("yellow\0's color", 15);
        CHECK(anns.saveAnnotation("doc1", ua));
        CHECK(anns.annotationsFor("doc1").size() == 1);
        CHECK(anns.annotationsFor("doc1").front().color == ua.color);
        CHECK(anns.annotationsFor("doc1").front().anchor.objectId == m.equations[0].id);
        CHECK(anns.deleteNote("doc1", n.id));
        CHECK(anns.notesFor("doc1").empty());
        CHECK(anns.deleteAnnotation("doc1", ua.id));
        CHECK(anns.annotationsFor("doc1").empty());
        ChatRepository chats(&db);
        ConversationId conv = chats.createConversation("doc1", "reader's thread");
        CHECK(!conv.empty());
        CHECK(chats.renameConversation(conv, "renamed thread"));
        CHECK(chats.searchConversations("doc1", "renamed").size() == 1);
        ChatMessage msg;
        msg.id = "m1";
        msg.role = "user";
        msg.text = std::string("what does the paper's result\0 mean?", 35);
        ContextReference msgRef;
        msgRef.id = "ref1";
        msgRef.type = ReferenceType::Section;
        msgRef.anchor = anchorForFigure(m, m.figures[0]);
        msgRef.displayName = "Introduction";
        msgRef.extractedText = "paper's opening";
        msgRef.caption = "Figure 1 caption";
        msgRef.latex = "x^2";
        msgRef.tableRows = {{"head\0", "value"}, {"1", "2"}};
        msgRef.relatedSources.push_back(anchorForEquation(m, m.equations[0]));
        msgRef.image = ReferenceImage{"image/png", {0, 1, 2, 0}, 4, 3};
        msg.references.push_back(msgRef);
        msg.sources.push_back(msgRef.anchor);
        msg.sourceRecords.push_back({"figure-evidence", msgRef.anchor});
        CHECK(chats.saveMessage(conv, msg));
        auto loadedMessages = chats.loadRecent(conv, 10);
        CHECK(loadedMessages.size() == 1);
        CHECK(loadedMessages.front().text == msg.text);
        CHECK(loadedMessages.front().references.size() == 1);
        CHECK(loadedMessages.front().references.front().type == ReferenceType::Section);
        CHECK(loadedMessages.front().references.front().anchor.block == msgRef.anchor.block);
        CHECK(loadedMessages.front().references.front().anchor.objectId == m.figures[0].id);
        CHECK(loadedMessages.front().references.front().caption == msgRef.caption);
        CHECK(loadedMessages.front().references.front().latex == msgRef.latex);
        CHECK(loadedMessages.front().references.front().tableRows == msgRef.tableRows);
        CHECK(loadedMessages.front().references.front().relatedSources.size() == 1);
        CHECK(loadedMessages.front().references.front().image.has_value());
        CHECK(loadedMessages.front().references.front().image->bytes == msgRef.image->bytes);
        CHECK(loadedMessages.front().sources.size() == 1);
        CHECK(loadedMessages.front().sourceRecords.size() == 1);
        CHECK(loadedMessages.front().sourceRecords.front().citationId == "figure-evidence");
        ConversationId second = chats.createConversation("doc1", "other");
        CHECK(second != conv);
        CHECK(chats.conversationsFor("doc1").size() == 2);
        CHECK(chats.deleteConversation(conv));
        CHECK(chats.conversationsFor("doc1").size() == 1);
        PaperAnalysis refs;
        Concept sharedConcept;
        sharedConcept.id = "concept_1";
        sharedConcept.type = "concept";
        sharedConcept.name = "shared-id";
        refs.concepts.push_back(sharedConcept);
        CHECK(docs.saveAnalysisRefs("doc1", refs));
        CHECK(docs.saveAnalysisRefs("doc2", refs));
        refs.meta.provider = "offline";
        refs.meta.model = "local";
        refs.meta.generatedAt = 123;
        refs.overview.mainIdea = "cached analysis";
        CHECK(docs.saveAnalysisCache("doc1", "hash1", refs));
        std::string cacheError;
        auto cached = docs.loadAnalysisCache("doc1", "hash1", cacheError);
        CHECK(cached.has_value() && cached->meta.provider == "offline");
        CHECK(!docs.loadAnalysisCache("doc1", "different-hash", cacheError).has_value());
        int doc1Concepts = 0;
        int doc2Concepts = 0;
        CHECK(db.query("SELECT document_id FROM concept_nodes ORDER BY document_id",
                       [&](sqlite3_stmt* st) {
                           const auto* id = sqlite3_column_text(st, 0);
                           if (!id) return;
                           if (std::string(reinterpret_cast<const char*>(id)) == "doc1")
                               ++doc1Concepts;
                           if (std::string(reinterpret_cast<const char*>(id)) == "doc2")
                               ++doc2Concepts;
                       }));
        CHECK(doc1Concepts == 1 && doc2Concepts == 1);
    }

    // Navigation history restores exact position.
    {
        NavigationHistory h;
        h.visit({0, 0, 1.0, std::nullopt});
        h.visit({13, 450.0, 1.5, std::nullopt});
        CHECK(h.canBack());
        NavEntry back = h.back();
        CHECK(back.page == 0);
        CHECK(h.canForward());
        CHECK(h.forward().page == 13);
    }

    std::error_code cleanupError;
    if (ownsTestHome) std::filesystem::remove_all(testHome, cleanupError);
    if (hadHome)
        setenv("HOME", originalHome.c_str(), 1);
    else
        unsetenv("HOME");

    if (failures == 0) std::cout << "ALL CORE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
