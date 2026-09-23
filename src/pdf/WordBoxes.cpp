// Worker-lane word geometry (never the UI thread).
// Primary path: one poppler layout pass per page (~ms) yields real per-word
// boxes; the page text is reconstructed from those words so the
// startIndex/length contract of WordBox is exact by construction. The
// legacy path (one pdfium engine query per word — hundreds per dense page,
// ~1s) remains only as a fallback when the layout is unavailable.
#include "pdf/PdfSelection.h"
#include "pdf/WordIndex.h"
#include <QPdfDocument>
#include <QPdfSelection>
#include <poppler-qt6.h>
#include <memory>

namespace reader {

namespace {

// Common PDF ligature private-use characters to ASCII, matching
// TextExtractor::normalizeLigatures so selection text matches block text.
QString normalizeLigatures(QString s) {
    const std::pair<QChar, const char*> reps[] = {
        {QChar(0xFB00), "ff"}, {QChar(0xFB01), "fi"}, {QChar(0xFB02), "fl"},
        {QChar(0xFB03), "ffi"}, {QChar(0xFB04), "ffl"}, {QChar(0xFB05), "st"},
        {QChar(0x00AD), ""},
    };
    for (const auto& [from, to] : reps)
        s.replace(from, QString::fromLatin1(to));
    return s;
}

std::vector<WordBox> legacyWordBoxes(QPdfDocument& doc, int page, QString& pageTextOut) {
    std::vector<WordBox> out;
    pageTextOut = doc.getAllText(page).text();
    const QString& full = pageTextOut;
    int n = full.size();
    int i = 0;
    while (i < n) {
        while (i < n && full[i].isSpace()) ++i;
        if (i >= n) break;
        int start = i;
        while (i < n && !full[i].isSpace()) ++i;
        QPdfSelection sel = doc.getSelectionAtIndex(page, start, i - start);
        if (!sel.isValid() || sel.text().trimmed().isEmpty()) continue;
        QRectF r = sel.boundingRectangle();
        if (!r.isValid()) continue;
        WordBox w;
        w.text = sel.text();
        w.startIndex = start;
        w.length = i - start;
        w.rect = r;
        out.push_back(std::move(w));
    }
    return out;
}

} // namespace

std::vector<WordBox> buildWordBoxesFromLayout(const QString& path, int page,
                                              QString& pageTextOut) {
    pageTextOut.clear();
    std::unique_ptr<Poppler::Document> pop = Poppler::Document::load(path);
    if (!pop || pop->isLocked() || page < 0 || page >= pop->numPages()) return {};
    auto pg = pop->page(page);
    if (!pg) return {};
    const auto boxes = pg->textList();
    if (boxes.empty()) return {};

    // Words arrive in content-stream order (the same order the old pdfium
    // loop produced). Rebuild the page text as words joined by single
    // spaces, with '\n' whenever the layout starts a new visual row, so
    // character indices line up with the words that own them.
    std::vector<WordBox> out;
    out.reserve(boxes.size());
    QString text;
    int cursor = 0;
    double lastBottom = -1e30;
    for (const auto& box : boxes) {
        if (!box) continue;
        QString word = normalizeLigatures(box->text());
        if (word.isEmpty()) continue;
        QRectF r = box->boundingBox();
        if (!r.isValid()) continue;
        if (!text.isEmpty()) {
            const bool newRow = r.top() >= lastBottom + 1.0 || r.bottom() <= lastBottom - 1.0;
            text += newRow ? '\n' : ' ';
            ++cursor;
        }
        WordBox w;
        w.text = word;
        w.startIndex = cursor;
        w.length = word.size();
        w.rect = r;
        text += word;
        cursor += word.size();
        lastBottom = r.bottom();
        out.push_back(std::move(w));
    }
    pageTextOut = text;
    return out;
}

std::vector<WordBox> buildWordBoxes(QPdfDocument& doc, int page, QString& pageTextOut) {
    return legacyWordBoxes(doc, page, pageTextOut);
}

int snapWordIndex(const std::vector<WordBox>& words, const QPointF& pt) {
    for (int k = 0; k < (int)words.size(); ++k)
        if (words[k].rect.contains(pt)) return k;
    // Same row first: a press in the inter-line gap belongs to the row it
    // visually sits on, not to a word lines away.
    int best = -1;
    double bestDx = 1e30;
    for (int k = 0; k < (int)words.size(); ++k) {
        const QRectF& r = words[k].rect;
        if (pt.y() < r.top() - 3 || pt.y() > r.bottom() + 3) continue;
        double dx = 0;
        if (pt.x() < r.left() - 2) dx = r.left() - 2 - pt.x();
        else if (pt.x() > r.right() + 2) dx = pt.x() - (r.right() + 2);
        if (dx < bestDx) {
            bestDx = dx;
            best = k;
        }
    }
    if (best >= 0) return best;
    // Nearest word within a small tolerance (clicks on gaps).
    best = -1;
    double bestDist = 100.0; // 10pt squared
    for (int k = 0; k < (int)words.size(); ++k) {
        const QRectF& r = words[k].rect;
        double dx = 0, dy = 0;
        if (pt.x() < r.left()) dx = r.left() - pt.x();
        else if (pt.x() > r.right()) dx = pt.x() - r.right();
        if (pt.y() < r.top()) dy = r.top() - pt.y();
        else if (pt.y() > r.bottom()) dy = pt.y() - r.bottom();
        double d = dx * dx + dy * dy;
        if (d < bestDist) {
            bestDist = d;
            best = k;
        }
    }
    return best;
}

} // namespace reader

namespace reader {

bool SelectionIndex::ready(int page) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(page);
    return it != pages_.end() && it->second.ready;
}

PageWords SelectionIndex::snapshot(int page) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(page);
    return it == pages_.end() ? PageWords{} : it->second;
}

void SelectionIndex::store(int page, PageWords words) {
    std::lock_guard<std::mutex> lock(mutex_);
    pages_[page] = std::move(words);
}

void SelectionIndex::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pages_.clear();
}

} // namespace reader
