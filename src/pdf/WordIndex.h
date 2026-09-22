#pragma once
#include <QString>
#include <QRectF>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class QPdfDocument;

namespace reader {

// Exact per-word geometry resolved on a worker lane (never the UI thread:
// each engine query costs milliseconds and dense pages need hundreds).
// Hit-testing and slicing then run engine-free.
struct WordBox {
    QString text;
    int startIndex = 0;
    int length = 0;
    QRectF rect;
};

struct PageWords {
    QString pageText;
    std::vector<WordBox> words;
    bool ready = false;
};

std::vector<WordBox> buildWordBoxes(QPdfDocument& doc, int page, QString& pageTextOut);

class SelectionIndex {
public:
    bool ready(int page) const;
    // Snapshot for UI-thread use (copy under lock, then lock-free).
    PageWords snapshot(int page) const;
    void store(int page, PageWords words);
    void clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<int, PageWords> pages_;
};

} // namespace reader
