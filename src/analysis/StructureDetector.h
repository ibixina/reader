#pragma once
#include "core/CancellationToken.h"
#include "document/DocumentModel.h"
#include "pdf/PdfEngine.h"
#include <vector>

namespace reader {

// Stage 3 of the pipeline (§43/§44): headings, sections, paragraphs,
// equations, figures, tables, references, citations. Heuristic and fully
// local; the LLM ingest (§5) only annotates stable IDs, never geometry.
class StructureDetector {
public:
    struct Result {
        std::vector<Section> sections;
        std::vector<Equation> equations;
        std::vector<Figure> figures;
        std::vector<Table> tables;
        std::vector<Citation> citations;
        std::vector<BibliographyEntry> bibliography;
        std::string title;
        std::vector<std::string> authors;
    };

    Result detect(DocumentModel& model, IPdfEngine& engine, IdFactory& ids,
                  CancellationToken token = {});

private:
    static bool isHeading(const std::string& text, float avgFont, float font);
    static int headingLevel(const std::string& text);
};

} // namespace reader
