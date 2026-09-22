#pragma once
#include "core/Types.h"
#include "document/DocumentAnchor.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace reader {

// Every explicit chat reference uses this one type (§15). Internal
// representation stays structured; the model prompt renders it, the UI
// renders chips/cards — never pasted plain text (§25).
enum class ReferenceType {
    TextSelection,
    Paragraph,
    Section,
    Page,
    Equation,
    Figure,
    Table,
    Citation,
    Concept
};

inline std::string referenceTypeLabel(ReferenceType t) {
    switch (t) {
        case ReferenceType::TextSelection: return "Selected paragraph";
        case ReferenceType::Paragraph: return "Paragraph";
        case ReferenceType::Section: return "Section";
        case ReferenceType::Page: return "Page";
        case ReferenceType::Equation: return "Equation";
        case ReferenceType::Figure: return "Figure";
        case ReferenceType::Table: return "Table";
        case ReferenceType::Citation: return "Citation";
        case ReferenceType::Concept: return "Concept";
    }
    return "?";
}

// Optional cropped evidence attached to a figure/table/region reference.
// Bytes are the encoded PNG/JPEG payload; providers choose how to encode it
// for their wire format.  Keeping it on the immutable reference snapshot
// prevents a later crop edit from changing an already-sent message.
struct ReferenceImage {
    std::string mimeType;
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
};

struct ContextReference {
    ReferenceId id;
    ReferenceType type = ReferenceType::TextSelection;
    DocumentAnchor anchor;
    std::string displayName;
    std::string extractedText;
    std::string caption;
    std::string latex;
    std::vector<std::vector<std::string>> tableRows;
    std::vector<DocumentAnchor> relatedSources;
    std::optional<ReferenceImage> image;
    bool pinned = false;
};

// Canonical IDs rendered in prompts and used to resolve cited source chips.
// Typed objects take precedence over text anchors so equation/figure/table
// citations remain stable even when their surrounding block changes.
inline std::string anchorReferenceId(const DocumentAnchor& anchor) {
    if (!anchor.objectId.empty()) return anchor.objectId;
    if (anchor.block) return *anchor.block;
    if (anchor.section) return *anchor.section;
    return "p." + std::to_string(anchor.page + 1);
}

inline std::string contextReferenceId(const ContextReference& reference) {
    if (!reference.anchor.objectId.empty())
        return anchorReferenceId(reference.anchor);
    // A text selection is a range, not the whole block. Prefer its immutable
    // message-local ID so two ranges in one block cannot collapse together.
    if (reference.type == ReferenceType::TextSelection) {
        if (!reference.id.empty()) return reference.id;
        return anchorReferenceId(reference.anchor) + ":" +
               sha256Hex(reference.anchor.anchorText + ":" +
                         std::to_string(reference.anchor.bounds.x) + ":" +
                         std::to_string(reference.anchor.bounds.y) + ":" +
                         std::to_string(reference.anchor.bounds.width) + ":" +
                         std::to_string(reference.anchor.bounds.height));
    }
    if (reference.anchor.block || reference.anchor.section)
        return anchorReferenceId(reference.anchor);
    if (!reference.id.empty()) return reference.id;
    if (!reference.displayName.empty()) return reference.displayName;
    return anchorReferenceId(reference.anchor);
}

// Live composer state: temporary (current selection, replaced) vs pinned
// (survives selections) (§14).
struct ComposerContext {
    std::vector<ContextReference> temporary;
    std::vector<ContextReference> pinned;
};

// Snapshot: references attached to a sent message are immutable (§16).
struct ChatSource {
    std::string citationId;
    DocumentAnchor anchor;
};

struct ChatMessage {
    MessageId id;
    std::string role; // "user" | "assistant"
    std::string text;
    std::vector<ContextReference> references;
    std::vector<DocumentAnchor> sources;
    // Preserve the exact citation token alongside each immutable source
    // anchor. Recomputing it from a block would merge two text selections in
    // the same block after a restart.
    std::vector<ChatSource> sourceRecords;
    TimestampMs createdAt = 0;
    bool incomplete = false;
};

struct PaperMetadata {
    std::string title;
    std::vector<std::string> authors;
    DocumentId documentId;
};

struct ReaderState {
    int page = 0;
    std::optional<SectionId> section;
    std::optional<BlockId> paragraphBlock;
};

struct RetrievedPassage {
    DocumentAnchor anchor;
    std::string text;
    double score = 0;
};

inline std::string retrievedPassageId(const RetrievedPassage& passage) {
    if (passage.anchor.document.empty()) return {};
    return anchorReferenceId(passage.anchor);
}

struct ChatRequest {
    std::string question;
    PaperMetadata paper;
    ReaderState readerState;
    std::vector<ContextReference> explicitReferences;
    std::vector<RetrievedPassage> retrievedPassages;
    std::vector<ChatMessage> recentConversation;
};

// Builds the complete immutable object context used by both PDF clicks and
// @ shorthands. Returns nullopt for a stale or unknown object anchor.
std::optional<ContextReference> contextReferenceForObject(
    const DocumentModel& model, const DocumentAnchor& anchor);

} // namespace reader
