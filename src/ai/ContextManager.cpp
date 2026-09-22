#include "ai/ContextManager.h"
#include "core/Types.h"
#include <regex>

namespace reader {

ReferenceId ContextManager::nextReferenceId() const {
    return "ref_" + std::to_string(++counter_);
}

void ContextManager::setCurrentSelection(ContextReference ref) {
    ref.pinned = false;
    if (ref.id.empty()) ref.id = nextReferenceId();
    // Temporary context holds only the latest selection (§14.1).
    context_.temporary.clear();
    context_.temporary.push_back(std::move(ref));
}

void ContextManager::clearCurrentSelection() {
    context_.temporary.clear();
}

void ContextManager::pinReference(const ReferenceId& id) {
    for (auto& t : context_.temporary) {
        if (t.id == id) {
            for (const auto& pinned : context_.pinned)
                if (pinned.id == id) return;
            t.pinned = true;
            context_.pinned.push_back(t);
            context_.temporary.clear();
            return;
        }
    }
}

void ContextManager::unpinReference(const ReferenceId& id) {
    auto& p = context_.pinned;
    for (auto it = p.begin(); it != p.end(); ++it) {
        if (it->id == id) {
            ContextReference r = *it;
            r.pinned = false;
            p.erase(it);
            if (context_.temporary.empty()) context_.temporary.push_back(std::move(r));
            return;
        }
    }
}

void ContextManager::removeReference(const ReferenceId& id) {
    auto removeFrom = [&](std::vector<ContextReference>& v) {
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it->id == id) {
                v.erase(it);
                return true;
            }
        return false;
    };
    if (!removeFrom(context_.pinned)) removeFrom(context_.temporary);
}

ChatRequest ContextManager::buildRequest(const std::string& question, const ReaderState& readerState,
                                         const PaperMetadata& paper,
                                         std::vector<RetrievedPassage> retrieved,
                                         std::vector<ChatMessage> recent) const {
    ChatRequest req;
    req.question = question;
    req.paper = paper;
    req.readerState = readerState;
    // Priority: pinned explicit > current selection; retrieval + metadata
    // appended after (§17). Paragraph/section fallbacks are added by
    // RetrievalEngine as RetrievedPassages, keeping this function pure.
    req.explicitReferences = context_.pinned;
    for (const auto& t : context_.temporary) req.explicitReferences.push_back(t);
    req.retrievedPassages = std::move(retrieved);
    req.recentConversation = std::move(recent);
    return req;
}

std::optional<ContextReference> ContextManager::resolveShorthand(
    const std::string& token, const DocumentModel& model, const ReaderState& state) const {
    std::string t = toLower(trim(token));
    if (t == "@selection" || t == "@sel") {
        if (!context_.temporary.empty()) return context_.temporary.front();
        return std::nullopt;
    }
    auto sectionRef = [&](const Section& s) -> ContextReference {
        ContextReference r;
        r.id = nextReferenceId();
        r.type = ReferenceType::Section;
        r.anchor = anchorForSection(model, s);
        r.displayName = s.title;
        r.extractedText = model.sectionText(s.id).substr(0, 2000);
        return r;
    };
    if (t == "@page") {
        ContextReference r;
        r.id = nextReferenceId();
        r.type = ReferenceType::Page;
        r.anchor.document = model.document.id;
        r.anchor.page = state.page;
        r.displayName = "Page " + std::to_string(state.page + 1);
        for (const auto& b : model.blocks) {
            if (b.page != state.page) continue;
            if (!r.extractedText.empty()) r.extractedText += "\n";
            r.extractedText += b.text;
        }
        r.anchor.anchorText = r.extractedText.substr(0, 2000);
        return r;
    }
    if (t == "@section") {
        const Section* s = state.section && model.findSection(*state.section)
                               ? model.findSection(*state.section)
                               : model.sectionForPage(state.page);
        if (!s) return std::nullopt;
        return sectionRef(*s);
    }
    if (t == "@methods" || t == "@results") {
        std::string want = t == "@methods" ? "method" : "result";
        for (const auto& s : model.sections) {
            if (toLower(s.title).find(want) != std::string::npos) return sectionRef(s);
        }
        return std::nullopt;
    }
    std::smatch m;
    static const std::regex eqRe(R"(@eq\s*_?(\d+))");
    static const std::regex figRe(R"(@fig\s*_?(\d+))");
    static const std::regex tableRe(R"(@table\s*_?(\d+))");
    static const std::regex citationRe(R"(@(?:cite|citation)\s*_?(\d+))");
    if (std::regex_match(t, m, eqRe)) {
        std::string id = "eq_" + m[1].str();
        if (const Equation* e = model.findEquation(id))
            return contextReferenceForObject(model, anchorForEquation(model, *e));
        return std::nullopt;
    }
    if (std::regex_match(t, m, figRe)) {
        std::string id = "fig_" + m[1].str();
        if (const Figure* f = model.findFigure(id))
            return contextReferenceForObject(model, anchorForFigure(model, *f));
        return std::nullopt;
    }
    if (std::regex_match(t, m, tableRe)) {
        std::string id = "table_" + m[1].str();
        if (const Table* tb = model.findTable(id))
            return contextReferenceForObject(model, anchorForTable(model, *tb));
        return std::nullopt;
    }
    if (std::regex_match(t, m, citationRe)) {
        const std::string id = "cite_" + m[1].str();
        for (const auto& citation : model.citations)
            if (citation.id == id)
                return contextReferenceForObject(model, anchorForCitation(model, citation));
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace reader
