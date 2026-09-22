#pragma once
#include "core/Types.h"
#include "document/DocumentAnchor.h"
#include "document/DocumentModel.h"
#include <string>

class QPdfDocument;

namespace reader {

// Maps a visual drag to a logical span in reading order, then to an anchor
// (§55): visual drag -> logical text span -> reading order -> DocumentAnchor.
class PdfSelection {
public:
    struct Drag {
        int page = 0;
        Rect rect;
    };

    static DocumentAnchor resolve(const DocumentModel& model, const Drag& drag,
                                  const std::string& selectedText);
    static DocumentAnchor resolveBlock(const DocumentModel& model, const TextBlock& block,
                                       std::size_t start, std::size_t end);
};

} // namespace reader
