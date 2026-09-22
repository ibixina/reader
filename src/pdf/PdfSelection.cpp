#include "pdf/PdfSelection.h"

namespace reader {

DocumentAnchor PdfSelection::resolve(const DocumentModel& model, const Drag& drag,
                                     const std::string& selectedText) {
    const TextBlock* best = nullptr;
    float bestArea = 0;
    for (const auto& b : model.blocks) {
        if (b.page != drag.page) continue;
        float x0 = std::max(b.bounds.x, drag.rect.x);
        float y0 = std::max(b.bounds.y, drag.rect.y);
        float x1 = std::min(b.bounds.x + b.bounds.width, drag.rect.x + drag.rect.width);
        float y1 = std::min(b.bounds.y + b.bounds.height, drag.rect.y + drag.rect.height);
        float area = (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) : 0;
        if (area > bestArea) {
            bestArea = area;
            best = &b;
        }
    }
    DocumentAnchor a;
    a.document = model.document.id;
    a.page = drag.page;
    a.bounds = drag.rect;
    if (best) {
        a.block = best->id;
        a.anchorText = selectedText.empty() ? best->text : selectedText;
        if (const Section* s = model.sectionForBlock(best->id)) a.section = s->id;
    } else {
        a.anchorText = selectedText;
    }
    return a;
}

DocumentAnchor PdfSelection::resolveBlock(const DocumentModel& model, const TextBlock& block,
                                          std::size_t start, std::size_t end) {
    DocumentAnchor a = anchorForBlock(model, block);
    if (start < block.text.size())
        a.anchorText = block.text.substr(start, std::min(end, block.text.size()) - start);
    return a;
}

} // namespace reader
