#include "analysis/StructureDetector.h"
#include "core/Types.h"
#include <algorithm>
#include <optional>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace reader {

bool StructureDetector::isHeading(const std::string& text, float avgFont, float font) {
    if (text.empty() || text.size() > 160) return false;
    // Captions, footnotes, bibliography entries and equations are never
    // headings, even when short or numbered.
    static const std::regex figRe(R"(^\s*(Figure|Fig\.)\s+(\d+))", std::regex::icase);
    static const std::regex tableRe(R"(^\s*Table\s+(\d+))", std::regex::icase);
    static const std::regex footnoteRe(R"(^\s*Footnote\s+(\d+))", std::regex::icase);
    static const std::regex bibRe(R"(^\s*\[\d+\])");
    static const std::regex eqLabel(R"(\(\s*(\d+)\s*\)\s*$)");
    if (std::regex_search(text, figRe) || std::regex_search(text, tableRe) ||
        std::regex_search(text, footnoteRe) || std::regex_search(text, bibRe))
        return false;
    if (std::regex_search(text, eqLabel) &&
        text.find_first_of("=∑∫∂∇∈∀∃αβγθλµσφψω^_") != std::string::npos)
        return false;
    const bool bigType = avgFont > 0 && font > avgFont * 1.18f;
    static const std::regex numbered(R"(^\s*(\d+(\.\d+)*\.?|Appendix\s+[A-Z]|[IVX]+\.)\s+\S)");
    if (std::regex_search(text, numbered)) {
        // Enumerated list items ("1. Load the data…") share the numbering
        // shape; only accept long ones when the type is also bigger.
        if (text.size() < 100 || bigType) return true;
    }
    static const std::regex sectionWord(
        R"(^\s*(Appendix|Annex|Chapter|Section|Part)\s+[A-Z0-9]+)", std::regex::icase);
    if (std::regex_search(text, sectionWord)) return true;
    static const std::regex theoremLike(
        R"(^\s*(Theorem|Lemma|Proposition|Corollary|Definition|Remark|Example|Proof)\b)",
        std::regex::icase);
    if (std::regex_search(text, theoremLike) && text.size() < 120) return true;
    std::string lower = toLower(trim(text));
    static const char* known[] = {
        "abstract",      "introduction",   "related work",  "background",
        "preliminaries", "method",         "methods",       "methodology",
        "approach",      "model",          "architecture",  "experiments",
        "experimental",  "evaluation",     "datasets",      "data",
        "results",       "discussion",     "analysis",      "ablation",
        "baselines",     "implementation", "conclusion",    "conclusions",
        "future work",   "summary",        "acknowledgment", "acknowledgement",
        "acknowledgments", "references",   "bibliography",  "appendix",
        "supplementary", "limitations",    "limitation",    "ethics",
        "broader impact", "author contributions", "additional", "overview"};
    for (auto k : known) {
        const std::string key(k);
        if (lower == key || lower == key + "s") return true;
        // "Conclusion and future work", "Experimental setup", "Data collection":
        // prefix matches only count for short standalone lines, never for
        // body paragraphs that merely start with the word.
        if (text.size() < 80 && lower.rfind(key, 0) == 0 && lower.size() > key.size() &&
            (lower[key.size()] == ' ' || lower[key.size()] == ':' || lower[key.size()] == '-'))
            return true;
    }
    // ALL-CAPS short lines ("INTRODUCTION", "ACKNOWLEDGMENTS").
    bool hasAlpha = false;
    bool allCaps = true;
    int words = 1;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        const char c = lower[i];
        if (c == ' ') ++words;
        if (c >= 'a' && c <= 'z') {
            hasAlpha = true;
            const char orig = text[i];
            if (orig != static_cast<char>(c - ('a' - 'A'))) allCaps = false;
        }
    }
    if (hasAlpha && allCaps && words <= 8 && text.size() >= 4) return true;
    // Larger type is the strongest signal: short lines set bigger than body.
    if (bigType && words <= 12) return true;
    return false;
}

int StructureDetector::headingLevel(const std::string& text) {
    static const std::regex subsub(R"(^\s*\d+\.\d+\.\d+)");
    if (std::regex_search(text, subsub)) return 3;
    static const std::regex sub(R"(^\s*\d+\.\d+)");
    if (std::regex_search(text, sub)) return 2;
    return 1;
}

StructureDetector::Result StructureDetector::detect(DocumentModel& model, IPdfEngine& engine,
                                                    IdFactory& ids, CancellationToken token) {
    Result out;
    // Structure detection may be retried after extraction completes or after
    // a document is reopened. Rebuild derived objects instead of appending a
    // second set of sections and anchors.
    model.sections.clear();
    model.equations.clear();
    model.figures.clear();
    model.tables.clear();
    model.citations.clear();
    model.bibliography.clear();
    model.footnotes.clear();
    model.rebuildIndex();
    // Recover per-block font hint from the engine for heading detection.
    // TextSpans carry the engine's font size, so the largest span inside a
    // block is its size proxy. Older cached models may lack spans entirely;
    // those blocks fall back to the body average (previous behavior).
    std::vector<float> blockFont(model.blocks.size(), 0);
    {
        std::unordered_map<int, std::vector<std::size_t>> spansByPage;
        for (std::size_t i = 0; i < model.lineSpans.size(); ++i)
            spansByPage[model.lineSpans[i].page].push_back(i);
        for (std::size_t i = 0; i < model.blocks.size(); ++i) {
            const auto& b = model.blocks[i];
            const auto it = spansByPage.find(b.page);
            if (it == spansByPage.end()) continue;
            float best = 0;
            for (std::size_t si : it->second) {
                const auto& s = model.lineSpans[si];
                const float centerY = s.bounds.y + s.bounds.height * 0.5f;
                if (centerY + 1.0f < b.bounds.y ||
                    centerY - 1.0f > b.bounds.y + b.bounds.height)
                    continue;
                best = std::max(best, s.fontSize);
            }
            blockFont[i] = best;
        }
    }
    float avgFont = 0;
    {
        std::vector<float> sizes;
        for (float f : blockFont)
            if (f > 0) sizes.push_back(f);
        if (!sizes.empty()) {
            std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
            avgFont = sizes[sizes.size() / 2];
        }
    }
    if (avgFont <= 0 && model.document.pageCount > 0) {
        std::vector<TextSpan> first = engine.extractSpans(0);
        float fontSum = 0;
        for (const auto& s : first) fontSum += s.fontSize;
        avgFont = first.empty() ? 11.0f : fontSum / static_cast<float>(first.size());
    }
    if (avgFont <= 0) avgFont = 11.0f;

    static const std::regex figRe(R"(^\s*(Figure|Fig\.)\s+(\d+)\s*[\s:.])", std::regex::icase);
    static const std::regex tableRe(R"(^\s*Table\s+(\d+)\s*[\s:.])", std::regex::icase);
    static const std::regex eqLabel(R"(\(\s*(\d+)\s*\)\s*$)");
    static const std::regex citeRe(R"(\[(\d+(?:\s*[,–-]\s*\d+)*)\])");
    static const std::regex authorRe(
        R"(^\s*(?:[A-Z]\.?\s+){1,2}[A-Za-z][A-Za-z'-]+(?:\s+and\s+(?:[A-Z]\.?\s+){1,2}[A-Za-z][A-Za-z'-]+)+\s*$)");
    static const std::regex footnoteRe(R"(^\s*Footnote\s+(\d+)\s*[:.]\s*(.+))",
                                       std::regex::icase);

    const auto objectBounds = [&](const TextBlock& caption, const std::string& kind,
                                  const std::string& number) {
        Rect bounds = caption.bounds;
        const std::regex label("^\\s*" + kind + "\\s+" + number + "\\s*$",
                               std::regex::icase);
        for (const auto& candidate : model.blocks) {
            if (candidate.page != caption.page || candidate.id == caption.id ||
                !std::regex_match(trim(candidate.text), label))
                continue;
            const float verticalGap = caption.bounds.y -
                                      (candidate.bounds.y + candidate.bounds.height);
            if (verticalGap < -8.0f || verticalGap > 400.0f) continue;
            const float left = std::min(bounds.x, candidate.bounds.x);
            const float top = std::min(bounds.y, candidate.bounds.y);
            const float right = std::max(bounds.x + bounds.width,
                                         candidate.bounds.x + candidate.bounds.width);
            const float bottom = std::max(bounds.y + bounds.height,
                                          candidate.bounds.y + candidate.bounds.height);
            bounds = {std::max(0.0f, left - 12.0f), std::max(0.0f, top - 12.0f),
                      right - left + 24.0f, bottom - top + 24.0f};
            break;
        }
        return bounds;
    };

    model.document.keywords.clear();
    bool abstractActive = false;

    std::optional<std::size_t> current;
    std::optional<std::size_t> currentTable;
    auto ensureIntro = [&]() -> std::size_t {
        if (!current) {
            Section s;
            s.id = ids.section();
            s.title = model.blocks.empty() ? "Paper" : "Introduction";
            s.level = 1;
            s.startPage = 0;
            model.sections.push_back(s);
            model.rebuildIndex();
            current = model.sections.size() - 1;
            currentTable.reset();
        }
        return *current;
    };

    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        if (token.cancelled()) break;
        auto& b = model.blocks[i];
        std::smatch m;
        std::string t = trim(b.text);
        if (t.empty()) continue;

        if (b.page == 0 && toLower(t).rfind("keywords:", 0) == 0) {
            std::string keywords = trim(t.substr(9));
            std::size_t start = 0;
            while (start <= keywords.size()) {
                const auto comma = keywords.find(',', start);
                model.document.keywords.push_back(trim(keywords.substr(
                    start, comma == std::string::npos ? comma : comma - start)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }

        // Citations are scanned even in headings; every other object
        // classification below applies to body blocks.
        auto begin = t.cbegin();
        while (std::regex_search(begin, t.cend(), m, citeRe)) {
            Citation c;
            c.id = "cite_" + std::to_string(out.citations.size() + 1);
            c.raw = m[0].str();
            c.page = b.page;
            c.block = b.id;
            out.citations.push_back(std::move(c));
            begin = m.suffix().first;
        }

        if (isHeading(t, avgFont, blockFont[i]) && t.size() < 160) {
            Section s;
            s.id = ids.section();
            s.title = t;
            s.level = headingLevel(t);
            s.startPage = b.page;
            s.endPage = b.page;
            if (current) model.sections[*current].endPage = b.page;
            model.sections.push_back(s);
            model.rebuildIndex();
            current = model.sections.size() - 1;
            std::string lower = toLower(t);
            abstractActive = lower.rfind("abstract", 0) == 0;
            continue;
        }
        if (abstractActive) {
            if (!model.document.abstractText.empty()) model.document.abstractText += "\n";
            model.document.abstractText += t;
            continue;
        }
        model.sections[ensureIntro()].blocks.push_back(b.id);
        model.sections[ensureIntro()].endPage = b.page;

        if (currentTable && b.page == out.tables[*currentTable].page &&
            t.find('|') != std::string::npos) {
            std::vector<std::string> row;
            std::size_t start = 0;
            while (start <= t.size()) {
                const auto end = t.find('|', start);
                const auto cell = trim(t.substr(start, end == std::string::npos ? end : end - start));
                if (!cell.empty()) row.push_back(cell);
                if (end == std::string::npos) break;
                start = end + 1;
            }
            if (!row.empty()) out.tables[*currentTable].rows.push_back(std::move(row));
        }

        if (std::regex_search(t, m, figRe)) {
            Figure f;
            f.id = ids.figure();
            f.page = b.page;
            f.bounds = objectBounds(b, "(?:Figure|Fig\\.)", m[2].str());
            f.caption = t.substr(0, 300);
            out.figures.push_back(f);
        } else if (std::regex_search(t, m, tableRe)) {
            Table tb;
            tb.id = ids.table();
            tb.page = b.page;
            tb.bounds = objectBounds(b, "Table", m[1].str());
            tb.caption = t.substr(0, 300);
            out.tables.push_back(tb);
            currentTable = out.tables.size() - 1;
        }
        if (std::regex_match(t, m, footnoteRe))
            model.footnotes.push_back({"footnote_" + m[1].str(), b.page, m[2].str()});
        if (std::regex_search(t, m, eqLabel) || t.find(" = ") != std::string::npos ||
            t.find("\\frac") != std::string::npos) {
            bool mathy = t.find_first_of("=∑∫∂∇∈∀∃αβγθλµσφψω^_") != std::string::npos;
            if (mathy && t.size() < 600) {
                Equation e;
                e.id = ids.equation();
                e.page = b.page;
                e.bounds = b.bounds;
                e.extractedText = t;
                e.latex = t;
                if (std::regex_search(t, m, eqLabel)) e.label = "(" + m[1].str() + ")";
                out.equations.push_back(e);
            }
        }
    }

    // Merge the PDF's embedded bookmarks: they catch unnumbered sections
    // ("Experimental Setup", "Acknowledgments") the text heuristics miss.
    // Entries duplicating a detected heading are skipped; the rest become
    // block-less sections that still navigate via their start page.
    if (!token.cancelled()) {
        auto native = engine.outline();
        if (!native.empty()) {
            auto normTitle = [](const std::string& title) {
                std::string lower = toLower(trim(title));
                static const std::regex prefix(
                    R"(^(\(?\s*(\d+(\.\d+)*|[ivxlcdm]+|[a-z])[\.\)\:]?\s+|appendix\s+[a-z0-9]+[\.\)\:]?\s*))",
                    std::regex::icase);
                return trim(std::regex_replace(lower, prefix, ""));
            };
            std::unordered_set<std::string> have;
            for (const auto& s : model.sections) have.insert(normTitle(s.title));
            for (const auto& e : native) {
                if (e.title.empty() || e.page < 0 || e.page >= model.document.pageCount)
                    continue;
                const std::string key = normTitle(e.title);
                if (key.empty() || have.count(key)) continue;
                Section s;
                s.id = ids.section();
                s.title = trim(e.title);
                s.level = std::clamp(e.level + 1, 1, 3);
                s.startPage = s.endPage = e.page;
                model.sections.push_back(s);
                have.insert(key);
            }
            std::stable_sort(model.sections.begin(), model.sections.end(),
                             [](const Section& a, const Section& b) {
                                 if (a.startPage != b.startPage) return a.startPage < b.startPage;
                                 return a.level < b.level;
                             });
            model.rebuildIndex();
        }
    }

    if (!model.blocks.empty()) {
        const auto& first = model.blocks.front();
        if (first.page == 0 && first.text.size() < 200 && !model.sections.empty())
            out.title = trim(first.text);
        if (model.document.authors.empty() && model.blocks.size() > 1 &&
            model.blocks[1].page == first.page) {
            const auto authorText = trim(model.blocks[1].text);
            const auto separator = authorText.find(" and ");
            if (std::regex_match(authorText, authorRe) && separator != std::string::npos && separator > 0 &&
                separator + 5 < authorText.size()) {
                model.document.authors = {trim(authorText.substr(0, separator)),
                                          trim(authorText.substr(separator + 5))};
            }
        }
    }

    // A references-page entry is real document text, so retain it as
    // bibliography metadata. No author/DOI is inferred when the text lacks it.
    static const std::regex bibliographyRe(R"(^\s*\[(\d+)\]\s*(.+))");
    int referencesPage = -1;
    for (const auto& section : model.sections)
        if (toLower(section.title).find("reference") != std::string::npos) {
            referencesPage = section.startPage;
            break;
        }
    for (const auto& block : model.blocks) {
        std::smatch match;
        if (referencesPage >= 0 && block.page >= referencesPage &&
            std::regex_match(block.text, match, bibliographyRe)) {
            BibliographyEntry entry;
            entry.id = "bib_" + match[1].str();
            entry.label = "[" + match[1].str() + "]";
            entry.text = trim(match[2].str());
            const auto doi = toLower(entry.text).find("doi:");
            if (doi != std::string::npos) entry.doi = trim(entry.text.substr(doi + 4));
            model.bibliography.push_back(std::move(entry));
        }
    }
    if (!out.title.empty()) model.document.title = out.title;
    out.authors = model.document.authors;
    out.bibliography = model.bibliography;

    model.equations = out.equations;
    model.figures = out.figures;
    model.tables = out.tables;
    model.citations = out.citations;
    model.rebuildIndex();
    out.sections = model.sections;
    return out;
}

} // namespace reader
