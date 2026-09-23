#include "pdf/QtPdfEngine.h"
#include <QEventLoop>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QTimer>
#include <QUrl>

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
    return true;
}

int QtPdfEngine::pageCount() const {
    return doc_ ? doc_->pageCount() : 0;
}

std::vector<reader::TextSpan> QtPdfEngine::extractSpans(int page) {
    std::vector<reader::TextSpan> out;
    if (!doc_ || page < 0 || page >= doc_->pageCount()) return out;
    QPdfSelection all = doc_->getAllText(page);
    if (!all.isValid()) return out;
    // bounds() yields one polygon per engine fragment (usually a line);
    // align fragments with text lines in order.
    QStringList lines = all.text().split('\n');
    QList<QPolygonF> polys = all.bounds();
    std::size_t n = std::min<std::size_t>(lines.size(), polys.size());
    for (std::size_t i = 0; i < n; ++i) {
        QString line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        QRectF r = polys[i].boundingRect();
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
