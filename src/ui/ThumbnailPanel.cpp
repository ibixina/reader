#include "ui/ThumbnailPanel.h"
#include "ui/PdfView.h"
#include <QImage>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QVBoxLayout>

ThumbnailPanel::ThumbnailPanel(reader::Application* app, PdfView* view, QWidget* parent)
    : QWidget(parent), app_(app), view_(view) {
    auto* layout = new QVBoxLayout(this);
    pages_ = new QListWidget(this);
    pages_->setIconSize(QSize(120, 170));
    layout->addWidget(pages_);
    connect(pages_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        emit pageActivated(item->data(Qt::UserRole).toInt());
    });
}

void ThumbnailPanel::rebuild() {
    pages_->clear();
    if (!view_) return;
    const int generation = ++rebuildGeneration_;
    const int count = view_->pageCount();
    for (int page = 0; page < count; ++page) {
        auto* item = new QListWidgetItem(QString("Page %1").arg(page + 1), pages_);
        item->setData(Qt::UserRole, page);
        // Staggered so a 20+ page document doesn't flood the render pool
        // ahead of the visible page's sharp tiles (which run at higher
        // priority but can't preempt already-started thumbnail renders).
        QPointer<ThumbnailPanel> guard(this);
        QTimer::singleShot(page * 25, this, [guard, page, generation] {
            if (!guard || !guard->view_) return;
            if (generation != guard->rebuildGeneration_) return;
            bool stillThere = false;
            for (int i = 0; i < guard->pages_->count(); ++i) {
                if (guard->pages_->item(i)->data(Qt::UserRole).toInt() == page) {
                    stillThere = true;
                    break;
                }
            }
            if (!stillThere) return;
            guard->view_->requestThumbnail(
                page, QSize(120, 170), [guard, page](const QImage& image) {
                    if (!guard || image.isNull()) return;
                    for (int i = 0; i < guard->pages_->count(); ++i) {
                        auto* candidate = guard->pages_->item(i);
                        if (candidate->data(Qt::UserRole).toInt() == page) {
                            candidate->setIcon(QPixmap::fromImage(image));
                            break;
                        }
                    }
                });
        });
    }
}
