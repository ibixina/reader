// Worker-lane word geometry (never the UI thread): one engine query per
// word is milliseconds each; dense pages need hundreds.
#include "pdf/PdfSelection.h"
#include "pdf/WordIndex.h"
#include <QPdfDocument>
#include <QPdfSelection>

namespace reader {

std::vector<WordBox> buildWordBoxes(QPdfDocument& doc, int page, QString& pageTextOut) {
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
