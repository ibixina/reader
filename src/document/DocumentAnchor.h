#pragma once
#include "core/Types.h"
#include "document/DocumentModel.h"
#include <optional>
#include <string>

namespace reader {

// The single pointing-into-the-paper type (§9). Highlights, notes, AI
// sources, search hits, concept nodes, chat references, bookmarks,
// citations and navigation entries all resolve to this.
struct DocumentAnchor {
    DocumentId document;
    int page = 0;
    Rect bounds;
    std::string anchorText;
    std::optional<SectionId> section;
    std::optional<BlockId> block;
    // Object identity is optional for backwards-compatible text anchors.
    // When present, objectType is equation, figure, table, citation, or
    // bibliography and objectId is the model-assigned stable ID.
    std::string objectType;
    std::string objectId;

    bool operator==(const DocumentAnchor& o) const {
        return document == o.document && page == o.page && anchorText == o.anchorText &&
               section == o.section && block == o.block && objectType == o.objectType &&
               objectId == o.objectId;
    }

    std::string displayName(const DocumentModel& model) const;
};

DocumentAnchor anchorForBlock(const DocumentModel& model, const TextBlock& block);
DocumentAnchor anchorForEquation(const DocumentModel& model, const Equation& eq);
DocumentAnchor anchorForFigure(const DocumentModel& model, const Figure& fig);
DocumentAnchor anchorForTable(const DocumentModel& model, const Table& table);
DocumentAnchor anchorForCitation(const DocumentModel& model, const Citation& citation);
DocumentAnchor anchorForSection(const DocumentModel& model, const Section& section);

} // namespace reader
