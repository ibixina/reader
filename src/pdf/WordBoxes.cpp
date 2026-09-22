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
