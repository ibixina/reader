#include "ai/References.h"
#include <algorithm>
#include <cmath>

namespace reader {
namespace {

std::string objectToken(const std::string& caption, const std::string& fallback) {
    const auto punctuation = caption.find_first_of(":.");
    const std::string token = trim(caption.substr(0, punctuation));
    return token.empty() ? fallback : token;
}

void addRelatedSources(ContextReference& reference, const DocumentModel& model,
                       const std::string& token) {
    struct Candidate {
        int priority = 0;
        float distance = 0;
        const TextBlock* block = nullptr;
    };
    std::vector<Candidate> candidates;
    for (const auto& block : model.blocks) {
        if (reference.anchor.block && block.id == *reference.anchor.block) continue;
        const bool mentions = !token.empty() && block.text.find(token) != std::string::npos;
        const float objectCenter = reference.anchor.bounds.y + reference.anchor.bounds.height / 2;
        const float blockCenter = block.bounds.y + block.bounds.height / 2;
        const float distance = std::abs(objectCenter - blockCenter);
        const bool nearby = block.page == reference.anchor.page && distance <= 180.0f;
        if (!mentions && !nearby) continue;
        candidates.push_back({mentions ? 0 : 1, distance, &block});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                       const Candidate& right) {
        if (left.priority != right.priority) return left.priority < right.priority;
        return left.distance < right.distance;
    });
    for (const auto& candidate : candidates) {
        if (reference.relatedSources.size() >= 4) break;
        auto anchor = anchorForBlock(model, *candidate.block);
        if (!anchor.anchorText.empty()) reference.relatedSources.push_back(std::move(anchor));
    }
}

} // namespace

std::optional<ContextReference> contextReferenceForObject(
    const DocumentModel& model, const DocumentAnchor& anchor) {
    ContextReference reference;
    reference.anchor = anchor;
    reference.id = anchor.objectId;
    std::string token;

    if (anchor.objectType == "equation") {
        const Equation* equation = model.findEquation(anchor.objectId);
        if (!equation) return std::nullopt;
        reference.type = ReferenceType::Equation;
        reference.anchor = anchorForEquation(model, *equation);
        reference.displayName = equation->label.empty() ? equation->id : "Equation " + equation->label;
        reference.extractedText = equation->extractedText;
        reference.caption = equation->label;
        reference.latex = equation->latex;
        token = equation->label;
    } else if (anchor.objectType == "figure") {
        const Figure* figure = model.findFigure(anchor.objectId);
        if (!figure) return std::nullopt;
        reference.type = ReferenceType::Figure;
        reference.anchor = anchorForFigure(model, *figure);
        reference.displayName = objectToken(figure->caption, figure->id);
        reference.extractedText = figure->caption;
        reference.caption = figure->caption;
        token = objectToken(figure->caption, figure->id);
    } else if (anchor.objectType == "table") {
        const Table* table = model.findTable(anchor.objectId);
        if (!table) return std::nullopt;
        reference.type = ReferenceType::Table;
        reference.anchor = anchorForTable(model, *table);
        reference.displayName = objectToken(table->caption, table->id);
        reference.extractedText = table->caption;
        reference.caption = table->caption;
        reference.tableRows = table->rows;
        token = objectToken(table->caption, table->id);
    } else if (anchor.objectType == "citation") {
        const Citation* citation = nullptr;
        for (const auto& item : model.citations)
            if (item.id == anchor.objectId) {
                citation = &item;
                break;
            }
        if (!citation) return std::nullopt;
        reference.type = ReferenceType::Citation;
        reference.anchor = anchorForCitation(model, *citation);
        reference.displayName = !citation->label.empty() ? citation->label : citation->id;
        reference.extractedText = citation->raw;
        if (!citation->title.empty()) reference.extractedText += " " + citation->title;
        if (!citation->doi.empty()) reference.extractedText += " DOI: " + citation->doi;
        reference.caption = citation->reason;
        token = !citation->label.empty() ? citation->label : citation->raw;
    } else {
        return std::nullopt;
    }

    addRelatedSources(reference, model, token);
    reference.anchor.anchorText = reference.extractedText;
    return reference;
}

} // namespace reader
