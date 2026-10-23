#pragma once
#include <QString>
#include <QRectF>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class QPdfDocument;
namespace Poppler { class Document; }

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
// Fast path: one poppler layout pass per page (~ms; the legacy path above
// costs ~1s per dense page in per-word engine queries). pageTextOut is
// reconstructed from the layout words, so startIndex/length always index
// it exactly. Empty vector when the layout is unavailable — callers should
// fall back to buildWordBoxes.
std::vector<WordBox> buildWordBoxesFromLayout(const QString& path, int page,
                                              QString& pageTextOut);

// One poppler document per open file, shared by every per-page layout pass
// (reloading the file per page re-parses its xref and fonts each time).
// Loaded lazily on first use. Not thread-safe: use it from one serialized
// lane only.
class PageLayoutSource {
public:
    explicit PageLayoutSource(QString path);
    ~PageLayoutSource();
    PageLayoutSource(const PageLayoutSource&) = delete;
    PageLayoutSource& operator=(const PageLayoutSource&) = delete;
    // nullptr when the file cannot be opened by poppler.
    Poppler::Document* document();

private:
    QString path_;
    std::unique_ptr<Poppler::Document> doc_;
    bool tried_ = false;
};

std::vector<WordBox> buildWordBoxesFromLayout(PageLayoutSource& source, int page,
                                              QString& pageTextOut);

bool sameTextRow(const QRectF& a, const QRectF& b);

// Press/drag endpoint resolution (UI-thread safe: pure geometry, no engine
// calls). Exact hit first; then the row the point visually sits on (a press
// in the inter-line gap belongs to its row, not to a word lines away);
// finally the globally nearest word within a small tolerance, else -1.
int snapWordIndex(const std::vector<WordBox>& words, const QPointF& pt);

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
