#include "pdf/QtPdfEngine.h"
#include <QEventLoop>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QTimer>
#include <QUrl>
#include <poppler-qt6.h>
#include <memory>

bool QtPdfEngine::open(const std::string& path) {
    if (!doc_) {
        lastError_ = "no document object";
        return false;
    }
    const QPdfDocument::Error err =
        doc_->load(QString::fromStdString(path));
    if (doc_->status() != QPdfDocument::Status::Ready) {
        switch (err) {
        case QPdfDocument::Error::FileNotFound: lastError_ = "file not found"; break;
        case QPdfDocument::Error::IncorrectPassword: lastError_ = "password-protected"; break;
        case QPdfDocument::Error::InvalidFileFormat: lastError_ = "not a valid PDF"; break;
        default: lastError_ = "unreadable or corrupt"; break;
        }
        return false;
    }
    lastError_.clear();
    path_ = path;
    layout_.reset();
    layoutTried_ = false;
    return true;
}

int QtPdfEngine::pageCount() const {
    return doc_ ? doc_->pageCount() : 0;
}

namespace {

// One span per poppler layout line, in reading order. Text and geometry
// come from the same word boxes, so they can never drift apart; one pass
// costs milliseconds per page. Empty when the layout is unavailable.
std::vector<reader::TextSpan> layoutSpans(Poppler::Document* pop, int page) {
    std::vector<reader::TextSpan> out;
    if (!pop || page < 0 || page >= pop->numPages()) return out;
    std::unique_ptr<Poppler::Page> pg = pop->page(page);
    if (!pg) return out;
    const auto boxes = pg->textList();
    QString line;
    QRectF rect;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const auto& box = boxes[i];
        if (!box) continue;
        const QRectF r = box->boundingBox();
        if (r.isValid()) {
            line += box->text();
            rect = rect.isNull() ? r : rect.united(r);
        }
        // Poppler links the words of one line through nextWord().
        const bool lineEnd = !box->nextWord() || i + 1 == boxes.size() ||
                             boxes[i + 1].get() != box->nextWord();
        if (!lineEnd) {
            if (box->hasSpaceAfter() && !line.isEmpty()) line += ' ';
            continue;
        }
        line = line.trimmed();
        if (!line.isEmpty() && rect.isValid()) {
            reader::TextSpan s;
            s.text = line.toStdString();
            s.bounds = {float(rect.x()), float(rect.y()), float(rect.width()),
                        float(rect.height())};
            s.fontSize = float(rect.height());
            s.page = page;
            out.push_back(std::move(s));
        }
        line.clear();
        rect = {};
    }
    return out;
}

} // namespace

Poppler::Document* QtPdfEngine::layoutDocument() {
    if (!layoutTried_ && !path_.empty()) {
        layoutTried_ = true;
        std::unique_ptr<Poppler::Document> pop =
            Poppler::Document::load(QString::fromStdString(path_));
        if (pop && !pop->isLocked()) layout_ = std::move(pop);
    }
    return layout_.get();
}

std::vector<reader::TextSpan> QtPdfEngine::extractSpans(int page) {
    std::vector<reader::TextSpan> out;
    if (!doc_ || page < 0 || page >= doc_->pageCount()) return out;
    out = layoutSpans(layoutDocument(), page);
    if (!out.empty()) return out;
    QPdfSelection all = doc_->getAllText(page);
    if (!all.isValid()) return out;
    // Fallback (no layout): resolve each text line's geometry from its own
    // character range. all.bounds() is one polygon per engine fragment,
    // not per line: superscripts, rotated margin stamps and font changes
    // add fragments, so pairing lines with polygons by position shifts
    // every later line onto the wrong rectangle. One engine query per
    // line is slow (~0.4 s per dense page), hence the layout path first.
    const QString text = all.text();
    int pos = 0;
    while (pos < text.size()) {
        int end = text.indexOf('\n', pos);
        if (end < 0) end = text.size();
        int a = pos, b = end;
        pos = end + 1;
        while (a < b && text[a].isSpace()) ++a;
        while (b > a && text[b - 1].isSpace()) --b;
        if (a == b) continue;
        QPdfSelection sel = doc_->getSelectionAtIndex(page, a, b - a);
        if (!sel.isValid()) continue;
        const QString line = sel.text().trimmed();
        const QRectF r = sel.boundingRectangle();
        if (line.isEmpty() || !r.isValid()) continue;
        reader::TextSpan s;
        s.text = line.toStdString();
        s.bounds = {float(r.x()), float(r.y()), float(r.width()), float(r.height())};
        s.fontSize = float(r.height());
        s.page = page;
        out.push_back(std::move(s));
    }
    if (out.empty() && !all.text().trimmed().isEmpty()) {
        QRectF r = all.boundingRectangle();
        reader::TextSpan s;
        s.text = all.text().simplified().toStdString();
        s.bounds = {float(r.x()), float(r.y()), float(r.width()), float(r.height())};
        s.fontSize = 11;
        s.page = page;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<reader::PdfOutlineEntry> QtPdfEngine::outline() {
    std::vector<reader::PdfOutlineEntry> out;
    if (!doc_) return out;
    QPdfBookmarkModel model;
    model.setDocument(doc_.get());
    // The bookmark model populates asynchronously, so rowCount() is always 0
    // right after setDocument(). A short bounded wait catches healthy
    // documents (populated in ~10 ms, measured); a 1.5 s wait here used to
    // stall every cold open of documents whose bookmark tree never loads.
    // Both signals are needed: trees arrive as row insertions, but some
    // backends swap the whole model under a reset.
    if (model.rowCount() == 0) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&model, &QPdfBookmarkModel::rowsInserted, &loop, &QEventLoop::quit);
        QObject::connect(&model, &QPdfBookmarkModel::modelReset, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(150);
        loop.exec();
    }
    std::function<void(const QModelIndex&, int)> walk = [&](const QModelIndex& parent, int level) {
        for (int row = 0; row < model.rowCount(parent); ++row) {
            QModelIndex idx = model.index(row, 0, parent);
            reader::PdfOutlineEntry e;
            e.title =
                model.data(idx, int(QPdfBookmarkModel::Role::Title)).toString().toStdString();
            e.page = model.data(idx, int(QPdfBookmarkModel::Role::Page)).toInt();
            e.level = level;
            out.push_back(std::move(e));
            walk(idx, level + 1);
        }
    };
    walk(QModelIndex{}, 0);
    return out;
}
