#pragma once

#include "analysis/PaperAnalysis.h"
#include "document/DocumentModel.h"

// Deterministic, dependency-free academic-style fixture used by reader
// acceptance tests. It deliberately contains enough structure to exercise
// page navigation, two-column extraction, object anchors, links and outlines.
// The vector rectangles are fixtures for hit testing; they are not intended
// to assert that extraction can identify figures or tables semantically.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace reader_test {

inline reader::PaperAnalysis academicAnalysis(const reader::DocumentModel& model) {
    reader::PaperAnalysis analysis;
    analysis.meta.provider = "fixture-grounded";
    analysis.meta.model = "deterministic-ui-acceptance";
    analysis.meta.generatedAt = reader::nowMs();
    analysis.overview.researchQuestion =
        "How can a paper reader preserve evidence anchors while navigation and AI work proceed?";
    analysis.overview.mainIdea =
        "Selections, navigation history, and cited answers share stable document anchors.";
    analysis.overview.mainContribution =
        "A local-first reader flow connects exact PDF geometry to verifiable AI context.";
    analysis.overview.method =
        "The evaluation repeats selection, rotation, search, reopen, and source-jump flows.";
    analysis.overview.architecture =
        "Rendering, extraction, retrieval, and provider work run on separate lanes.";
    analysis.overview.setup =
        "A fixed four-page, two-column PDF supplies known text and object regions.";
    analysis.overview.mainResults = {
        "Selection identity survives rotation.",
        "Cached reopen performs zero provider requests.",
        "Source links return to the exact paper passage."};
    analysis.overview.limitations = {
        "Semantic object extraction remains heuristic; manual region capture is retained."};
    analysis.overview.takeaway =
        "The reader can stay responsive while every AI claim remains source-addressable.";

    for (const auto& section : model.sections) {
        reader::SectionSummary summary;
        summary.sectionId = section.id;
        summary.summaryShort = "Grounded summary of " + section.title + ".";
        summary.summaryDetailed =
            "This section is represented by stable block and page anchors for navigation and evidence.";
        summary.importance = 0.8;
        analysis.sections.push_back(std::move(summary));
    }
    auto findBlock = [&](const std::string& needle) -> const reader::TextBlock* {
        for (const auto& block : model.blocks)
            if (block.text.find(needle) != std::string::npos) return &block;
        return nullptr;
    };
    const auto* selection = findBlock("Selection is represented");
    const auto* history = findBlock("history entry stores");
    const auto* result = findBlock("selection round trip");
    const auto* limitation = findBlock("Limitations:");
    auto annotate = [&](const reader::TextBlock* block, const std::string& type,
                        const std::string& reason) {
        if (!block) return;
        analysis.annotations.push_back(
            {block->id, 0, block->text.size(), type, 0.9, reason});
    };
    annotate(selection, "method", "Defines the typed-anchor selection protocol.");
    annotate(result, "result", "Reports the measured selection round trip.");
    annotate(limitation, "limitation", "States the boundary of object extraction.");
    if (selection)
        analysis.concepts.push_back(
            {"concept_selection", "Typed selection", "A selection stored as text plus geometry.",
             "concept", {selection->id}});
    if (history)
        analysis.concepts.push_back(
            {"concept_history", "Navigation history", "Restorable page, scroll, zoom, and selection.",
             "method", {history->id}});
    if (result)
        analysis.concepts.push_back(
            {"concept_verification", "Source verification", "A cited answer jumps to exact evidence.",
             "result", {result->id}});
    if (analysis.concepts.size() >= 2)
        analysis.relationships.push_back(
            {analysis.concepts[0].id, analysis.concepts[1].id, "is restored by",
             {selection ? selection->id : std::string{}, history ? history->id : std::string{}}});
    if (analysis.concepts.size() >= 3)
        analysis.relationships.push_back(
            {analysis.concepts[1].id, analysis.concepts[2].id, "supports",
             {history ? history->id : std::string{}, result ? result->id : std::string{}}});
    for (const auto& equation : model.equations)
        analysis.equations.push_back(
            {equation.id, "Defines the deterministic training objective.", 0.8});
    for (const auto& figure : model.figures)
        analysis.figures.push_back(
            {figure.id, "Shows an anchor-preserving reader pipeline.", 0.8});
    return analysis;
}

inline std::string pdfEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

inline void addText(std::string& stream, const char* font, int size, int x, int y,
                    const std::string& text) {
    stream += "BT /";
    stream += font;
    stream += " ";
    stream += std::to_string(size);
    stream += " Tf ";
    stream += std::to_string(x);
    stream += " ";
    stream += std::to_string(y);
    stream += " Td (";
    stream += pdfEscape(text);
    stream += ") Tj ET\n";
}

inline void addRule(std::string& stream, int x, int y, int width) {
    stream += std::to_string(x) + " " + std::to_string(y) + " m " +
              std::to_string(x + width) + " " + std::to_string(y) + " l S\n";
}

inline void addRect(std::string& stream, int x, int y, int width, int height) {
    stream += std::to_string(x) + " " + std::to_string(y) + " " +
              std::to_string(width) + " " + std::to_string(height) + " re S\n";
}

inline std::string pageOne() {
    std::string s = "0.15 0.15 0.15 RG 0.15 0.15 0.15 rg\n";
    addText(s, "F2", 19, 54, 734, "A Deterministic Study of Reader Workflows");
    addText(s, "F1", 10, 54, 708, "A. Researcher and B. Reviewer");
    addRule(s, 54, 696, 504);
    addText(s, "F2", 13, 54, 668, "1 Introduction");
    addText(s, "F1", 9, 54, 646,
            "Interactive reading systems connect evidence, navigation, and explanation.");
    addText(s, "F1", 9, 54, 630,
            "We study how a reader preserves anchors while moving through a paper [1].");
    addText(s, "F1", 9, 54, 614,
            "The benchmark uses a two-column layout and explicit source geometry.");
    addText(s, "F1", 9, 54, 586, "Our objective is to keep the selected passage stable.");
    addText(s, "F1", 9, 54, 570, "The reader can jump to Methods using this internal link.");
    addText(s, "F1", 9, 54, 554, "See Methods on page two for the measurement protocol.");
    addText(s, "F1", 9, 54, 524, "L(theta) = sum_i log p(y_i | x_i, theta)");
    addText(s, "F1", 9, 54, 508, "Equation 1. The objective used in every deterministic run.");
    addRect(s, 54, 340, 225, 122);
    addText(s, "F2", 9, 66, 436, "Figure 1");
    addText(s, "F1", 8, 66, 324, "Figure 1. Anchor-preserving reader pipeline.");
    addRule(s, 330, 460, 225);
    addRule(s, 330, 340, 225);
    addRule(s, 330, 340, 120);
    addRule(s, 330, 400, 225);
    addRule(s, 450, 340, 120);
    addText(s, "F2", 9, 342, 476, "Table 1");
    addText(s, "F1", 8, 342, 324, "Table 1. Fixture rows: local 0.91, remote 0.04.");
    addText(s, "F1", 9, 320, 586, "Keywords: anchors, selection, retrieval, reproducibility.");
    addText(s, "F1", 9, 54, 270, "[1] A. Researcher. Stable evidence in interactive documents.");
    addText(s, "F1", 9, 54, 254, "The blue rectangle and table lines are intentional object regions.");
    return s;
}

inline std::string pageTwo() {
    std::string s = "0.15 0.15 0.15 RG 0.15 0.15 0.15 rg\n";
    addText(s, "F2", 16, 54, 734, "2 Methods");
    addRule(s, 54, 722, 504);
    const std::vector<std::string> left = {
        "We render a fixed four-page PDF fixture.",
        "Each page has two columns with known text.",
        "Coordinates remain in PDF points after rotation.",
        "Selection is represented by a typed anchor.",
        "A history entry stores page, scroll, and zoom.",
        "The test repeats the same sequence offline."};
    const std::vector<std::string> right = {
        "The corpus contains one figure and one table.",
        "Outline destinations identify each section.",
        "The internal link targets the Results page.",
        "Search uses literal and semantic tokens.",
        "Annotations are local and document scoped.",
        "No network provider is consulted on open."};
    for (std::size_t i = 0; i < left.size(); ++i) {
        addText(s, "F1", 9, 54, 690 - static_cast<int>(i) * 28, left[i]);
        addText(s, "F1", 9, 320, 690 - static_cast<int>(i) * 28, right[i]);
    }
    addText(s, "F2", 54, 500, 54, "2.1 Selection protocol");
    addText(s, "F1", 9, 54, 478, "A drag selects words in reading order and stores their geometry.");
    addText(s, "F1", 9, 54, 462, "A double click selects one word without changing the document.");
    addText(s, "F1", 9, 54, 432, "2.2 Reopen protocol");
    addText(s, "F1", 9, 54, 410, "Close and reopen must restore the last page and scroll offset.");
    addText(s, "F1", 9, 54, 394, "A stale asynchronous result must not alter the new document.");
    addRect(s, 320, 270, 235, 90);
    addText(s, "F2", 9, 330, 336, "Methods diagram");
    addText(s, "F1", 8, 330, 252, "Figure 2. Reopen and generation check.");
    return s;
}

inline std::string pageThree() {
    std::string s = "0.15 0.15 0.15 RG 0.15 0.15 0.15 rg\n";
    addText(s, "F2", 16, 54, 734, "3 Results");
    addRule(s, 54, 722, 504);
    addText(s, "F1", 9, 54, 690, "The local path preserves selection identity across a rotation.");
    addText(s, "F1", 9, 54, 674, "Cached models reopen without invoking extraction or a remote provider.");
    addText(s, "F1", 9, 54, 658, "The reader restores a history anchor after a source jump.");
    addText(s, "F1", 9, 320, 690, "Result A: page navigation latency 12 ms.");
    addText(s, "F1", 9, 320, 674, "Result B: selection round trip 100 percent.");
    addText(s, "F1", 9, 320, 658, "Result C: cache hit made zero network calls.");
    addText(s, "F2", 13, 54, 610, "3.1 Evaluation table");
    addRule(s, 54, 560, 504);
    addRule(s, 54, 470, 504);
    addRule(s, 54, 515, 504);
    addRule(s, 270, 470, 90);
    addRule(s, 450, 470, 90);
    addText(s, "F2", 64, 532, 64, "Metric");
    addText(s, "F2", 64, 532, 280, "Value");
    addText(s, "F2", 64, 532, 460, "Notes");
    addText(s, "F1", 8, 480, 496, "anchor jump");
    addText(s, "F1", 8, 480, 296, "1.00");
    addText(s, "F1", 8, 480, 476, "stable");
    addText(s, "F1", 8, 480, 496 - 26, "cache hit");
    addText(s, "F1", 8, 480, 296 - 26, "0.00");
    addText(s, "F1", 8, 480, 476 - 26, "network");
    addText(s, "F1", 9, 54, 410, "Limitations: the fixture gives known regions, while semantic object extraction");
    addText(s, "F1", 9, 54, 394, "remains an application heuristic. Manual region capture remains supported.");
    return s;
}

inline std::string pageFour() {
    std::string s = "0.15 0.15 0.15 RG 0.15 0.15 0.15 rg\n";
    addText(s, "F2", 16, 54, 734, "4 References");
    addRule(s, 54, 722, 504);
    addText(s, "F1", 10, 54, 684, "[1] A. Researcher and B. Reviewer, Stable evidence in interactive documents,");
    addText(s, "F1", 10, 72, 666, "Journal of Deterministic Systems, 2026, doi:10.0000/reader.1.");
    addText(s, "F1", 10, 54, 620, "[2] C. Analyst, Local retrieval and source-preserving summaries,");
    addText(s, "F1", 10, 72, 602, "Proceedings of Offline Software, 2025, doi:10.0000/reader.2.");
    addText(s, "F1", 9, 54, 540, "The references are included to exercise bibliography extraction and anchors.");
    addText(s, "F1", 9, 54, 514, "End of deterministic academic fixture.");
    return s;
}

inline void writeAcademicPdf(const std::string& path) {
    const std::vector<std::string> streams = {pageOne(), pageTwo(), pageThree(), pageFour()};
    std::vector<std::string> objects;
    objects.reserve(19);
    objects.push_back("1 0 obj << /Type /Catalog /Pages 2 0 R /Outlines 15 0 R >> endobj");
    objects.push_back("2 0 obj << /Type /Pages /Kids [3 0 R 5 0 R 7 0 R 9 0 R] /Count 4 >> endobj");
    objects.push_back("3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 11 0 R /F2 12 0 R >> >> /Annots [14 0 R] >> endobj");
    objects.push_back("");
    objects.push_back("5 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R /Resources << /Font << /F1 11 0 R /F2 12 0 R >> >> >> endobj");
    objects.push_back("");
    objects.push_back("7 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 8 0 R /Resources << /Font << /F1 11 0 R /F2 12 0 R >> >> >> endobj");
    objects.push_back("");
    objects.push_back("9 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 10 0 R /Resources << /Font << /F1 11 0 R /F2 12 0 R >> >> >> endobj");
    objects.push_back("");
    objects.push_back("11 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj");
    objects.push_back("12 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >> endobj");
    objects.push_back("13 0 obj << /Type /Annot /Subtype /Link /Rect [54 540 300 566] /Dest [5 0 R /Fit] >> endobj");
    objects.push_back("14 0 obj << /Type /Annot /Subtype /Link /Rect [54 540 300 566] /Dest [5 0 R /Fit] >> endobj");
    objects.push_back("15 0 obj << /Type /Outlines /First 16 0 R /Last 18 0 R /Count 3 >> endobj");
    objects.push_back("16 0 obj << /Title (Introduction) /Parent 15 0 R /Dest [3 0 R /Fit] /Next 17 0 R >> endobj");
    objects.push_back("17 0 obj << /Title (Methods) /Parent 15 0 R /Dest [5 0 R /Fit] /Next 18 0 R >> endobj");
    objects.push_back("18 0 obj << /Title (Results) /Parent 15 0 R /Dest [7 0 R /Fit] >> endobj");
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const int objectIndex = 4 + static_cast<int>(i) * 2;
        objects[objectIndex - 1] = std::to_string(objectIndex) + " 0 obj << /Length " +
                                    std::to_string(streams[i].size()) + " >> stream\n" +
                                    streams[i] + "endstream endobj";
    }
    std::string out = "%PDF-1.4\n";
    std::vector<std::size_t> offsets(objects.size() + 1, 0);
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].empty()) continue;
        const std::size_t objectNumber = i + 1;
        offsets[objectNumber] = out.size();
        out += objects[i] + "\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    char buf[32];
    for (std::size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] == 0)
            std::snprintf(buf, sizeof buf, "0000000000 00000 f \n");
        else
            std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", offsets[i]);
        out += buf;
    }
    out += "trailer << /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF";
    std::ofstream f(path, std::ios::binary);
    f << out;
}

} // namespace reader_test
