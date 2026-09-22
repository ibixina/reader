#include "pdf/QtPdfEngine.h"
#include <QEventLoop>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QTimer>
#include <QUrl>
#include <functional>

bool QtPdfEngine::open(const std::string& path) {
    if (!doc_) return false;
    doc_->load(QString::fromStdString(path));
    return doc_->status() == QPdfDocument::Status::Ready;
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

std::vector<reader::PdfLink> QtPdfEngine::links(int page) {
    std::vector<reader::PdfLink> out;
    if (!doc_) return out;
    QPdfLinkModel model;
    model.setDocument(doc_.get());
    // The model populates asynchronously; bound the wait (worker lane).
    if (model.rowCount(QModelIndex{}) == 0) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&model, &QPdfLinkModel::rowsInserted, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(1500);
        loop.exec();
    }
    for (int row = 0; row < model.rowCount(QModelIndex{}); ++row) {
        QModelIndex idx = model.index(row, 0, QModelIndex{});
        QPdfLink link = model.data(idx, int(QPdfLinkModel::Role::Link)).value<QPdfLink>();
        if (link.page() != page) continue;
        for (const QRectF& r : link.rectangles()) {
            reader::PdfLink l;
            l.page = page;
            l.bounds = {float(r.x()), float(r.y()), float(r.width()), float(r.height())};
            l.targetPage = -1;
            l.targetUri = link.url().toString().toStdString();
            out.push_back(std::move(l));
        }
    }
    return out;
}

std::vector<reader::PdfOutlineEntry> QtPdfEngine::outline() {
    std::vector<reader::PdfOutlineEntry> out;
    if (!doc_) return out;
    QPdfBookmarkModel model;
    model.setDocument(doc_.get());
    if (model.rowCount() == 0) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&model, &QPdfBookmarkModel::rowsInserted, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(1500);
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
