#include "pdf/TextExtractor.h"
#include "core/Types.h"
#include <algorithm>
#include <cmath>

namespace reader {

std::string TextExtractor::fixHyphenation(std::string line, bool endsWithHyphen) {
    line = trim(line);
    if (endsWithHyphen && !line.empty() && line.back() == '-') line.pop_back();
    return line;
}

std::string TextExtractor::normalizeLigatures(std::string s) {
    // Common PDF ligature private-use mappings to ASCII.
    const std::pair<const char*, const char*> reps[] = {
        {"\xEF\xAC\x80", "ff"}, {"\xEF\xAC\x81", "fi"}, {"\xEF\xAC\x82", "fl"},
        {"\xEF\xAC\x83", "ffi"}, {"\xEF\xAC\x84", "ffl"}, {"\xEF\xAC\x85", "st"},
        {"\xC2\xAD", ""},
    };
    for (auto [from, to] : reps) {
        std::string f(from);
        std::size_t pos = 0;
        while ((pos = s.find(f, pos)) != std::string::npos) {
            s.replace(pos, f.size(), to);
            pos += std::char_traits<char>::length(to);
        }
    }
    return s;
}

std::vector<std::vector<TextSpan>> TextExtractor::columns(std::vector<TextSpan> spans) {
    if (spans.empty()) return {};
    float minX = spans[0].bounds.x, maxX = spans[0].bounds.x + spans[0].bounds.width;
    for (const auto& s : spans) {
        minX = std::min(minX, s.bounds.x);
        maxX = std::max(maxX, s.bounds.x + s.bounds.width);
    }
    float mid = (minX + maxX) * 0.5f;
    bool twoCol = false;
    bool hasLeft = false, hasRight = false;
    for (const auto& s : spans) {
        float cx = s.bounds.x + s.bounds.width * 0.5f;
        if (cx < mid) hasLeft = true;
        else hasRight = true;
    }
    // Only split when content genuinely spans both halves with a visible gap.
    if (hasLeft && hasRight) {
        float gapScore = 0;
        for (const auto& s : spans) {
            float cx = s.bounds.x + s.bounds.width * 0.5f;
            if (std::fabs(cx - mid) > (maxX - minX) * 0.18f) gapScore += 1;
        }
        twoCol = gapScore > static_cast<float>(spans.size()) * 0.5f;
    }
    std::sort(spans.begin(), spans.end(), [](const TextSpan& a, const TextSpan& b) {
        if (std::fabs(a.bounds.y - b.bounds.y) > 3.0f) return a.bounds.y < b.bounds.y;
        return a.bounds.x < b.bounds.x;
    });
    if (!twoCol) return {spans};
    std::vector<TextSpan> left, right;
    for (auto& s : spans) {
        float cx = s.bounds.x + s.bounds.width * 0.5f;
        (cx < mid ? left : right).push_back(s);
    }
    // Reading order: left column top-to-bottom, then right column.
    return {left, right};
}

std::vector<TextBlock> TextExtractor::extract(IPdfEngine& engine, IdFactory& ids,
                                              const DocumentId& docId, int pageCount,
                                              std::vector<TextSpan>* linesOut) {
    (void)docId;
    std::vector<TextBlock> blocks;
    for (int page = 0; page < pageCount; ++page) {
        auto spans = engine.extractSpans(page);
        if (spans.empty()) continue;
        for (auto& col : columns(std::move(spans))) {
            // Group spans into lines by y, then merge lines into a block
            // until a paragraph gap appears.
            std::sort(col.begin(), col.end(), [](const TextSpan& a, const TextSpan& b) {
                if (std::fabs(a.bounds.y - b.bounds.y) > 2.0f) return a.bounds.y < b.bounds.y;
                return a.bounds.x < b.bounds.x;
            });
            if (linesOut) linesOut->insert(linesOut->end(), col.begin(), col.end());
            std::string para;
            Rect bounds{1e30f, 1e30f, 0, 0};
            bool has = false;
            float lastBottom = 0;
            float lastFont = 0;
            auto flush = [&] {
                if (!has || trim(para).empty()) return;
                TextBlock b;
                b.id = ids.block();
                b.page = page;
                b.bounds = bounds;
                b.text = normalizeLigatures(trim(para));
                blocks.push_back(std::move(b));
                para.clear();
                has = false;
            };
            for (std::size_t i = 0; i < col.size(); ++i) {
                const auto& s = col[i];
                float gap = has ? s.bounds.y - lastBottom : 0;
                float fontChange = has && lastFont > 0 ? std::fabs(s.fontSize - lastFont) : 0;
                bool paraBreak = has && (gap > s.fontSize * 1.1f || fontChange > 2.5f);
                if (paraBreak) flush();
                bool cont = !para.empty() && para.back() == '-';
                std::string piece = fixHyphenation(s.text, false);
                if (cont) para.pop_back();
                else if (!para.empty()) para += ' ';
                para += piece;
                if (!has) {
                    bounds = s.bounds;
                    has = true;
                } else {
                    float x0 = std::min(bounds.x, s.bounds.x);
                    float y0 = std::min(bounds.y, s.bounds.y);
                    float x1 = std::max(bounds.x + bounds.width, s.bounds.x + s.bounds.width);
                    float y1 = std::max(bounds.y + bounds.height, s.bounds.y + s.bounds.height);
                    bounds = {x0, y0, x1 - x0, y1 - y0};
                }
                lastBottom = s.bounds.y + s.bounds.height;
                lastFont = s.fontSize;
            }
            flush();
        }
    }
    return blocks;
}

} // namespace reader
