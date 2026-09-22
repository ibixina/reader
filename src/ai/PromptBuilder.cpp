#include "ai/PromptBuilder.h"
#include <algorithm>
#include <sstream>

namespace reader {

namespace {

constexpr std::size_t kTitleLimit = 200;
constexpr std::size_t kSectionLimit = 160;
constexpr std::size_t kDisplayNameLimit = 160;
constexpr std::size_t kReferenceTextLimit = 3000;
constexpr std::size_t kCaptionLimit = 1000;
constexpr std::size_t kLatexLimit = 2000;
constexpr std::size_t kPassageLimit = 2500;
constexpr std::size_t kHistoryMessageLimit = 2000;

bool validUtf8(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const auto c = static_cast<unsigned char>(value[i]);
        std::size_t length = 0;
        if (c <= 0x7f) length = 1;
        else if (c >= 0xc2 && c <= 0xdf) length = 2;
        else if (c >= 0xe0 && c <= 0xef) length = 3;
        else if (c >= 0xf0 && c <= 0xf4) length = 4;
        else return false;
        if (i + length > value.size()) return false;
        for (std::size_t j = 1; j < length; ++j)
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        if (length == 3) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0)) return false;
        } else if (length == 4) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) return false;
        }
        i += length;
    }
    return true;
}

std::string utf8Prefix(const std::string& value, std::size_t limit) {
    if (value.size() <= limit) return value;
    std::size_t end = limit;
    while (end > 0 &&
           (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
        --end;
    return value.substr(0, end);
}

std::string renderSystem(const ChatRequest& req) {
    std::ostringstream sys;
    sys << "You are an assistant embedded in a research-paper reader. "
           "Answer about the paper below.\n"
           "Rules:\n"
           "- Distinguish direct paper statements (cite them) from your own "
           "interpretation and from external knowledge.\n"
           "- Every paper-specific factual claim must cite at least one "
           "reference ID from the context, e.g. [block_143] or [eq_7].\n"
           "- If the paper does not support an answer, say "
           "\"I don't see this addressed explicitly in the paper.\" rather "
           "than fabricating evidence.\n"
           "- Keep answers focused; use Markdown and LaTeX where helpful.\n";
    if (!req.paper.title.empty()) sys << "Paper: " << req.paper.title << "\n";
    return sys.str();
}

std::string renderReference(const ContextReference& r) {
    std::ostringstream line;
    line << "- [" << contextReferenceId(r) << "] " << referenceTypeLabel(r.type);
    if (!r.displayName.empty()) line << " " << r.displayName;
    line << " (page " << (r.anchor.page + 1) << "): " << r.extractedText << "\n";
    if (!r.caption.empty()) line << "  Caption: " << r.caption << "\n";
    if (!r.latex.empty()) line << "  LaTeX: " << r.latex << "\n";
    if (!r.tableRows.empty()) {
        line << "  Table rows:\n";
        for (const auto& row : r.tableRows) {
            line << "  |";
            for (const auto& cell : row) line << " " << cell << " |";
            line << "\n";
        }
    }
    for (const auto& related : r.relatedSources) {
        line << "  - [" << anchorReferenceId(related) << "] Related source (page "
             << (related.page + 1) << "): " << related.anchorText << "\n";
    }
    return line.str();
}

std::string renderUser(const ChatRequest& req) {
    std::ostringstream user;
    user << "Location: page " << (req.readerState.page + 1);
    if (req.readerState.section) user << ", section " << *req.readerState.section;
    user << "\n\n";
    if (!req.explicitReferences.empty()) {
        user << "Explicit references:\n";
        for (const auto& ref : req.explicitReferences) user << renderReference(ref);
        user << "\n";
    }
    if (!req.retrievedPassages.empty()) {
        user << "Retrieved passages:\n";
        for (const auto& passage : req.retrievedPassages)
            user << "- [" << retrievedPassageId(passage) << "] " << passage.text << "\n";
        user << "\n";
    }
    if (!req.recentConversation.empty()) {
        user << "Conversation so far:\n";
        for (const auto& message : req.recentConversation)
            user << (message.role == "user" ? "User: " : "AI: ") << message.text << "\n";
        user << "\n";
    }
    user << "Question: " << req.question;
    return user.str();
}

std::size_t renderedSize(const ChatRequest& request) {
    return renderSystem(request).size() + renderUser(request).size();
}

template <class Rendered, class Setter>
void fitText(const std::string& source, std::size_t fieldLimit, std::size_t totalLimit,
             Rendered&& renderedSizeForValue, Setter&& setValue) {
    std::size_t low = 0;
    std::size_t high = std::min(fieldLimit, source.size());
    while (low < high) {
        const std::size_t mid = low + (high - low + 1) / 2;
        const std::string prefix = utf8Prefix(source, mid);
        setValue(prefix);
        if (renderedSizeForValue() <= totalLimit) low = mid;
        else high = mid - 1;
    }
    setValue(utf8Prefix(source, low));
    while (renderedSizeForValue() > totalLimit && low > 0) {
        --low;
        setValue(utf8Prefix(source, low));
    }
}

} // namespace

PromptBuilder::Built PromptBuilder::build(const ChatRequest& req) {
    return build(req, Options{});
}

PromptBuilder::Built PromptBuilder::build(const ChatRequest& req, const Options& options) {
    return prepare(req, options).prompt;
}

PromptBuilder::Prepared PromptBuilder::prepare(const ChatRequest& req) {
    return prepare(req, Options{});
}

PromptBuilder::Prepared PromptBuilder::prepare(const ChatRequest& req,
                                                const Options& options) {
    Prepared result;
    if (!validUtf8(req.question)) {
        result.error = "The question contains invalid UTF-8 text.";
        return result;
    }
    if (!req.paper.documentId.empty()) {
        for (const auto& reference : req.explicitReferences) {
            if (!reference.anchor.document.empty() &&
                reference.anchor.document != req.paper.documentId) {
                result.error = "A reference belongs to a different paper; remove it and retry.";
                return result;
            }
            for (const auto& related : reference.relatedSources)
                if (!related.document.empty() && related.document != req.paper.documentId) {
                    result.error =
                        "A related source belongs to a different paper; remove it and retry.";
                    return result;
                }
        }
        for (const auto& passage : req.retrievedPassages)
            if (!passage.anchor.document.empty() &&
                passage.anchor.document != req.paper.documentId) {
                result.error =
                    "A retrieved passage belongs to a different paper; retry the question.";
                return result;
            }
    }
    for (const auto& passage : req.retrievedPassages) {
        if (retrievedPassageId(passage).empty()) {
            result.error = "A retrieved passage is missing a stable citation ID.";
            return result;
        }
    }

    result.request = req;
    result.request.explicitReferences.clear();
    result.request.retrievedPassages.clear();
    result.request.recentConversation.clear();
    result.request.paper.title = utf8Prefix(req.paper.title, kTitleLimit);
    if (req.readerState.section)
        result.request.readerState.section = utf8Prefix(*req.readerState.section, kSectionLimit);

    // Metadata has a hard cap and yields before the user's complete question.
    while (renderedSize(result.request) > options.charBudget &&
           !result.request.paper.title.empty())
        result.request.paper.title =
            utf8Prefix(result.request.paper.title, result.request.paper.title.size() - 1);
    if (renderedSize(result.request) > options.charBudget &&
        result.request.readerState.section)
        result.request.readerState.section.reset();
    if (renderedSize(result.request) > options.charBudget) {
        result.error = "The question is too long for the configured prompt budget; shorten it and retry.";
        result.request = req;
        return result;
    }

    // Explicit references have first claim on the remaining budget. A
    // reference is retained only after its complete citation header fits.
    for (const auto& source : req.explicitReferences) {
        const std::string id = contextReferenceId(source);
        if (id.empty()) {
            result.error = "An explicit reference is missing a stable citation ID.";
            result.request = req;
            return result;
        }
        ContextReference normalized = source;
        normalized.displayName = utf8Prefix(source.displayName, kDisplayNameLimit);
        normalized.extractedText.clear();
        normalized.caption.clear();
        normalized.latex.clear();
        normalized.tableRows.clear();
        normalized.relatedSources.clear();
        result.request.explicitReferences.push_back(normalized);
        if (renderedSize(result.request) > options.charBudget) {
            result.request.explicitReferences.pop_back();
            continue;
        }
        auto& target = result.request.explicitReferences.back();
        const auto size = [&] { return renderedSize(result.request); };
        fitText(source.extractedText, kReferenceTextLimit, options.charBudget, size,
                [&](const std::string& value) { target.extractedText = value; });
        fitText(source.caption, kCaptionLimit, options.charBudget, size,
                [&](const std::string& value) { target.caption = value; });
        fitText(source.latex, kLatexLimit, options.charBudget, size,
                [&](const std::string& value) { target.latex = value; });

        const auto rowCount = std::min<std::size_t>(source.tableRows.size(), 12);
        for (std::size_t row = 0; row < rowCount; ++row) {
            std::vector<std::string> normalizedRow;
            const auto cellCount = std::min<std::size_t>(source.tableRows[row].size(), 12);
            for (std::size_t cell = 0; cell < cellCount; ++cell)
                normalizedRow.push_back(utf8Prefix(source.tableRows[row][cell], 300));
            target.tableRows.push_back(std::move(normalizedRow));
            if (renderedSize(result.request) > options.charBudget) {
                target.tableRows.pop_back();
                break;
            }
        }
        const auto relatedCount = std::min<std::size_t>(source.relatedSources.size(), 8);
        for (std::size_t i = 0; i < relatedCount; ++i) {
            const auto& related = source.relatedSources[i];
            if (related.anchorText.empty()) continue;
            DocumentAnchor normalizedRelated = related;
            normalizedRelated.anchorText = utf8Prefix(related.anchorText, kPassageLimit);
            target.relatedSources.push_back(std::move(normalizedRelated));
            if (renderedSize(result.request) > options.charBudget) {
                target.relatedSources.pop_back();
                break;
            }
        }
        target.anchor.anchorText = target.extractedText;
        const bool hasTextEvidence = !target.extractedText.empty() || !target.caption.empty() ||
                                     !target.latex.empty() || !target.tableRows.empty() ||
                                     !target.relatedSources.empty();
        if (!hasTextEvidence && (!target.image || target.image->bytes.empty()))
            result.request.explicitReferences.pop_back();
    }

    // Retrieved passages follow explicit user references.
    for (const auto& source : req.retrievedPassages) {
        RetrievedPassage normalized = source;
        normalized.text.clear();
        result.request.retrievedPassages.push_back(normalized);
        if (renderedSize(result.request) > options.charBudget) {
            result.request.retrievedPassages.pop_back();
            continue;
        }
        auto& target = result.request.retrievedPassages.back();
        fitText(source.text, kPassageLimit, options.charBudget,
                [&] { return renderedSize(result.request); },
                [&](const std::string& value) { target.text = value; });
        target.anchor.anchorText = target.text;
        if (target.text.empty()) result.request.retrievedPassages.pop_back();
    }

    // Add newest history first, retaining chronological order in the prompt.
    for (auto it = req.recentConversation.rbegin(); it != req.recentConversation.rend(); ++it) {
        ChatMessage normalized;
        normalized.id = it->id;
        normalized.role = it->role == "user" ? "user" : "assistant";
        normalized.createdAt = it->createdAt;
        normalized.incomplete = it->incomplete;
        normalized.text = utf8Prefix(it->text, kHistoryMessageLimit);
        result.request.recentConversation.insert(result.request.recentConversation.begin(),
                                                 std::move(normalized));
        if (renderedSize(result.request) > options.charBudget)
            result.request.recentConversation.erase(result.request.recentConversation.begin());
    }

    result.prompt = {renderSystem(result.request), renderUser(result.request)};
    return result;
}

} // namespace reader
