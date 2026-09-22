#include "document/DocumentAnchor.h"

namespace reader {

std::string DocumentAnchor::displayName(const DocumentModel& model) const {
    std::string loc = "p." + std::to_string(page + 1);
    std::string prefix;
    if (section) {
        if (const Section* s = model.findSection(*section))
            prefix = "§" + s->title;
    }
    if (prefix.empty()) prefix = "§?";
    return prefix + " · " + loc;
}

DocumentAnchor anchorForBlock(const DocumentModel& model, const TextBlock& block) {
    DocumentAnchor a;
    a.document = model.document.id;
    a.page = block.page;
    a.bounds = block.bounds;
    a.anchorText = block.text;
    a.block = block.id;
    if (const Section* s = model.sectionForBlock(block.id)) a.section = s->id;
    return a;
}

DocumentAnchor anchorForEquation(const DocumentModel& model, const Equation& eq) {
    DocumentAnchor a;
    a.document = model.document.id;
    a.page = eq.page;
    a.bounds = eq.bounds;
    a.anchorText = eq.label.empty() ? eq.extractedText : eq.label + " " + eq.extractedText;
    a.objectType = "equation";
    a.objectId = eq.id;
    if (const Section* s = model.sectionForPage(eq.page)) a.section = s->id;
    return a;
}

DocumentAnchor anchorForFigure(const DocumentModel& model, const Figure& fig) {
    DocumentAnchor a;
    a.document = model.document.id;
    a.page = fig.page;
    a.bounds = fig.bounds;
    a.anchorText = fig.caption;
    a.objectType = "figure";
    a.objectId = fig.id;
    if (const Section* s = model.sectionForPage(fig.page)) a.section = s->id;
    return a;
}

DocumentAnchor anchorForTable(const DocumentModel& model, const Table& table) {
    DocumentAnchor a;
    a.document = model.document.id;
    a.page = table.page;
    a.bounds = table.bounds;
    a.anchorText = table.caption;
    a.objectType = "table";
    a.objectId = table.id;
    if (const Section* s = model.sectionForPage(table.page)) a.section = s->id;
    return a;
}

DocumentAnchor anchorForCitation(const DocumentModel& model, const Citation& citation) {
    DocumentAnchor a;
    a.document = model.document.id;
    a.page = citation.page;
    a.anchorText = !citation.title.empty() ? citation.title : citation.raw;
    a.objectType = "citation";
    a.objectId = citation.id;
    if (citation.block) {
        a.block = citation.block;
        if (const TextBlock* block = model.findBlock(*citation.block)) {
            a.bounds = block->bounds;
            if (a.anchorText.empty()) a.anchorText = block->text;
            if (const Section* section = model.sectionForBlock(block->id)) a.section = section->id;
        }
    }
    if (!a.section)
        if (const Section* section = model.sectionForPage(citation.page)) a.section = section->id;
    return a;
}

DocumentAnchor anchorForSection(const DocumentModel& model, const Section& section) {
    DocumentAnchor a;
    a.document = model.document.id;
    a.page = section.startPage;
    a.anchorText = section.title;
    a.section = section.id;
    return a;
}

} // namespace reader
