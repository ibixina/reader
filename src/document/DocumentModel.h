#pragma once
#include "core/Types.h"
#include "pdf/PdfEngine.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace reader {

inline constexpr int kExtractionSchemaVersion = 1;

// Application-level document model (§8). Raw PDF-engine objects never leak
// past the pdf/ layer; everything downstream uses these stable-ID types.
struct Document {
    DocumentId id;
    std::string filePath;
    std::string fileHash;
    std::string title;
    std::vector<std::string> authors;
    std::string abstractText;
    std::vector<std::string> keywords;
    int extractionVersion = 0;
    bool extractionComplete = false;
    int pageCount = 0;
};

struct BibliographyEntry {
    std::string id;
    std::string label;
    std::string text;
    std::string doi;
};

struct Footnote {
    std::string id;
    int page = 0;
    std::string text;
};

struct TextBlock {
    BlockId id;
    int page = 0;
    Rect bounds;
    std::string text;
};

struct Section {
    SectionId id;
    std::string title;
    int level = 1;
    int startPage = 0;
    int endPage = 0;
    std::vector<BlockId> blocks;
};

struct Equation {
    EquationId id;
    int page = 0;
    Rect bounds;
    std::string extractedText;
    std::string latex;
    std::string label;
};

struct Figure {
    FigureId id;
    int page = 0;
    Rect bounds;
    std::string caption;
};

struct Table {
    TableId id;
    int page = 0;
    Rect bounds;
    std::string caption;
    std::vector<std::vector<std::string>> rows;
};

struct Citation {
    CitationId id;
    std::string raw;
    std::string label;
    std::string title;
    std::string doi;
    std::string reason;
    int page = 0;
    std::optional<BlockId> block;
};

// Monotonic stable IDs (§5.7): sec_N, block_NNN, eq_N, fig_N, table_N.
class IdFactory {
public:
    SectionId section() { return "sec_" + std::to_string(++sections_); }
    BlockId block() { return "block_" + std::to_string(++blocks_); }
    EquationId equation() { return "eq_" + std::to_string(++equations_); }
    FigureId figure() { return "fig_" + std::to_string(++figures_); }
    TableId table() { return "table_" + std::to_string(++tables_); }

private:
    int sections_ = 0, blocks_ = 0, equations_ = 0, figures_ = 0, tables_ = 0;
};

class DocumentModel {
public:
    Document document;
    std::vector<TextBlock> blocks;
    std::vector<Section> sections;
    std::vector<Equation> equations;
    std::vector<Figure> figures;
    std::vector<Table> tables;
    std::vector<Citation> citations;
    std::vector<BibliographyEntry> bibliography;
    std::vector<Footnote> footnotes;
    // Line-level fragments in reading order (points). Cached at extraction
    // so overlays and selection hit-testing never touch the engine (§7).
    std::vector<TextSpan> lineSpans;

    const TextBlock* findBlock(const BlockId& id) const;
    const Section* findSection(const SectionId& id) const;
    const Equation* findEquation(const EquationId& id) const;
    const Figure* findFigure(const FigureId& id) const;
    const Table* findTable(const TableId& id) const;

    const Section* sectionForPage(int page) const;
    const Section* sectionForBlock(const BlockId& id) const;
    const TextBlock* blockAtPage(int page, float yCenter) const;

    std::string sectionText(const SectionId& id) const;
    void rebuildIndex();

private:
    std::unordered_map<BlockId, std::size_t> blockIndex_;
    std::unordered_map<SectionId, std::size_t> sectionIndex_;
};

} // namespace reader
