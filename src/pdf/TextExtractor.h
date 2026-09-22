#pragma once
#include "document/DocumentModel.h"
#include "pdf/PdfEngine.h"
#include <string>
#include <vector>

namespace reader {

// Stage 2 of the pipeline (§44): spans -> ordered TextBlocks.
// Handles two-column layouts, hyphenation, ligatures (§55).
// Optionally captures per-line fragments (reading order) for overlay and
// selection geometry that must never touch the engine on the UI thread.
class TextExtractor {
public:
    std::vector<TextBlock> extract(IPdfEngine& engine, IdFactory& ids,
                                   const DocumentId& docId, int pageCount,
                                   std::vector<TextSpan>* linesOut = nullptr);

private:
    static std::string fixHyphenation(std::string line, bool endsWithHyphen);
    static std::string normalizeLigatures(std::string s);
    static std::vector<std::vector<TextSpan>> columns(std::vector<TextSpan> spans);
};

} // namespace reader
