#include "ui/DocumentShelf.h"
#include "pdf/PopplerBridge.h"
#include "storage/Repositories.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>

namespace {
constexpr int kCardWidth = 224;
constexpr int kCardHeight = 376;
enum ItemRole {
    CoverKeyRole = Qt::UserRole + 1,
    ProgressRole,
    ActivityRole,
    PageRole,
    PageCountRole,
    PreviewUnavailableRole
};

QString recentActivity(bool opened, qint64 timestamp) {
    const QDate date = QDateTime::fromMSecsSinceEpoch(timestamp).date();
    const auto days = date.daysTo(QDate::currentDate());
    QString when;
    if (days <= 0) when = "today";
    else if (days == 1) when = "yesterday";
    else when = QLocale().toString(date, "MMM d");
    return QString("%1 %2").arg(opened ? "Opened" : "Added", when);
}

class PaperCoverDelegate final : public QStyledItemDelegate {
public:
    PaperCoverDelegate(bool dark, QObject* parent) : QStyledItemDelegate(parent), dark_(dark) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {kCardWidth, kCardHeight};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (option.rect.isEmpty()) return;
        painter->save();
        painter->setClipRect(option.rect, Qt::IntersectClip);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const bool dark = dark_;
        const QColor text = dark ? QColor("#e9ede9") : QColor("#252c27");
        const QColor muted = dark ? QColor("#a0aaa3") : QColor("#777d77");
        const QColor accent = dark ? QColor("#8dbca5") : QColor("#3c6c55");
        const QRectF card = option.rect.adjusted(6, 4, -6, -4);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(selected ? QPen(accent, 1) : QPen(Qt::NoPen));
            painter->setBrush(dark ? QColor("#2b3730") : QColor(selected ? "#e5ede7" : "#eeeee8"));
            painter->drawRoundedRect(card, 9, 9);
        }

        const QImage image = index.data(Qt::DecorationRole).value<QImage>();
        const QRectF coverArea(card.left() + 12, card.top() + 12, card.width() - 24, 248);
        const QSizeF paperSize = (image.isNull() ? QSizeF(612, 792) : QSizeF(image.size()))
                                    .scaled(coverArea.size(), Qt::KeepAspectRatio);
        const QRectF paper(coverArea.center().x() - paperSize.width() / 2,
                           coverArea.bottom() - paperSize.height(),
                           paperSize.width(), paperSize.height());
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, dark ? 55 : 14));
        painter->drawRoundedRect(paper.translated(2, 4), 2, 2);
        painter->setBrush(Qt::white);
        painter->drawRect(paper);
        if (!image.isNull()) painter->drawImage(paper, image);
        else {
            const QRectF contents = paper.adjusted(18, 24, -18, -24);
            painter->setBrush(QColor("#e7e9e6"));
            painter->drawRoundedRect(QRectF(contents.left(), contents.top(), contents.width(), 5), 1, 1);
            painter->drawRoundedRect(QRectF(contents.left(), contents.top() + 10, contents.width() * .7, 5), 1, 1);
            for (int i = 0; i < 13; ++i)
                painter->drawRect(QRectF(contents.left(), contents.top() + 34 + i * 9,
                                         contents.width() * (i % 4 == 3 ? .7 : 1), 2));
            if (index.data(PreviewUnavailableRole).toBool()) {
                painter->setPen(QColor("#777d77"));
                painter->drawText(paper.adjusted(6, 0, -6, -10), Qt::AlignBottom | Qt::AlignHCenter,
                                  "Preview unavailable");
            }
        }
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QColor(0, 0, 0, 24));
        painter->drawRect(paper);

        const QRectF titleRect(card.left() + 12, coverArea.bottom() + 16, card.width() - 24, 40);
        QFont titleFont = option.font;
        if (titleFont.pointSizeF() > 0) titleFont.setPointSizeF(titleFont.pointSizeF() + 1);
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(text);
        const QFontMetrics metrics(titleFont);
        const QString title = index.data(Qt::DisplayRole).toString();
        QTextLayout titleLayout(title, titleFont);
        QTextOption wrap;
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        titleLayout.setTextOption(wrap);
        titleLayout.beginLayout();
        auto first = titleLayout.createLine();
        if (first.isValid()) {
            first.setLineWidth(titleRect.width());
            const QString line = title.left(first.textLength()).trimmed();
            painter->drawText(titleRect.topLeft() + QPointF(0, metrics.ascent()), line);
            const QString rest = title.mid(first.textLength()).trimmed();
            painter->drawText(titleRect.topLeft() + QPointF(0, metrics.height() + metrics.ascent()),
                              metrics.elidedText(rest, Qt::ElideRight, int(titleRect.width())));
        }
        titleLayout.endLayout();
        QFont detailFont = option.font;
        if (detailFont.pointSizeF() > 0) detailFont.setPointSizeF(std::max(8.0, detailFont.pointSizeF() - 1));
        painter->setFont(detailFont);
        painter->setPen(index.data(PageRole).isValid() ? accent : muted);
        const QRectF progressRect(titleRect.left(), titleRect.bottom() + 5, titleRect.width(), 18);
        painter->drawText(progressRect, Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(ProgressRole).toString());
        painter->setPen(muted);
        painter->drawText(progressRect.translated(0, 20), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(ActivityRole).toString());
        const int pages = index.data(PageCountRole).toInt();
        if (index.data(PageRole).isValid() && pages > 0) {
            const QRectF bar(titleRect.left(), card.bottom() - 8, titleRect.width(), 3);
            painter->setPen(Qt::NoPen);
            painter->setBrush(dark ? QColor("#38463c") : QColor("#dbe4dc"));
            painter->drawRoundedRect(bar, 1.5, 1.5);
            painter->setBrush(accent);
            const double fraction = std::clamp((index.data(PageRole).toInt() + 1.0) / pages, 0.0, 1.0);
            painter->drawRoundedRect(QRectF(bar.topLeft(), QSizeF(bar.width() * fraction, bar.height())), 1.5, 1.5);
        }
        painter->restore();
    }
private:
    bool dark_;
};
} // namespace

DocumentShelf::DocumentShelf(reader::DocumentRepository* documents, QWidget* parent)
    : QWidget(parent), documents_(documents) {
    setObjectName("documentShelf");
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    QPalette colors = palette();
    colors.setColor(QPalette::Window, QColor(dark ? "#202722" : "#f6f5f0"));
    colors.setColor(QPalette::Base, colors.color(QPalette::Window));
    colors.setColor(QPalette::WindowText, QColor(dark ? "#e9ede9" : "#252c27"));
    colors.setColor(QPalette::Text, colors.color(QPalette::WindowText));
    setPalette(colors);
    setAutoFillBackground(true);
    setStyleSheet(QString(
        "QLineEdit#shelfFolderPath { border: none; background: transparent; color: %1; padding: 0; }"
        "QLineEdit#shelfSearch { padding: 10px 12px; border: 1px solid %2; border-radius: 6px; background: %3; }"
        "QLineEdit#shelfSearch:focus { border-color: #5c896e; }"
        "QPushButton { padding: 7px 13px; border: 1px solid %2; border-radius: 6px; background: %3; }"
        "QPushButton:hover { border-color: %1; }"
        "QPushButton:focus { border-color: #5c896e; }"
        "QLabel#shelfHint, QLabel#shelfMessage { color: %1; }")
        .arg(dark ? "#a0aaa3" : "#777d77", dark ? "#4a554d" : "#d6d9d1", dark ? "#2b342d" : "#fbfbf8"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(16);
    auto* heading = new QLabel("Your documents", this);
    QFont headingFont = heading->font();
    headingFont.setPointSize(24);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);
    auto* hint = new QLabel("Most recently opened or added. Click a paper to continue reading.", this);
    hint->setObjectName("shelfHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* folderRow = new QHBoxLayout;
    folderPath_ = new QLineEdit(this);
    folderPath_->setObjectName("shelfFolderPath");
    folderPath_->setReadOnly(true);
    folderPath_->setFocusPolicy(Qt::ClickFocus);
    folderPath_->setPlaceholderText("Choose a folder for your documents");
    folderPath_->setAccessibleName("Documents folder");
    folderRow->addWidget(folderPath_, 1);
    auto* choose = new QPushButton("Choose folder…", this);
    choose->setObjectName("chooseShelfFolder");
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString start = folder_.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : folder_;
        const QString path = QFileDialog::getExistingDirectory(this, "Choose documents folder", start);
        if (!path.isEmpty()) setFolder(path);
    });
    folderRow->addWidget(choose);
    auto* reload = new QPushButton("Refresh", this);
    reload->setObjectName("refreshShelf");
    connect(reload, &QPushButton::clicked, this, &DocumentShelf::refresh);
    folderRow->addWidget(reload);
    layout->addLayout(folderRow);

    search_ = new QLineEdit(this);
    search_->setObjectName("shelfSearch");
    search_->setPlaceholderText("Search your documents…");
    search_->setAccessibleName("Search library document names");
    search_->setToolTip("Filter PDF names as you type. Press Enter to open the selected paper.");
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);

    message_ = new QLabel(this);
    message_->setObjectName("shelfMessage");
    message_->setWordWrap(true);
    layout->addWidget(message_);
    list_ = new QListWidget(this);
    list_->setObjectName("shelfDocuments");
    list_->setAccessibleName("Document shelf");
    list_->setViewMode(QListView::IconMode);
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setWrapping(true);
    list_->setUniformItemSizes(true);
    list_->setGridSize({kCardWidth, kCardHeight});
    list_->setFrameShape(QFrame::NoFrame);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMouseTracking(true);
    list_->setStyleSheet(QString(
        "QListWidget { background: transparent; border: none; outline: 0; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 0; }"
        "QScrollBar::handle:vertical { background: %1; border-radius: 4px; min-height: 32px; }"
        "QScrollBar::handle:vertical:hover { background: %2; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
        .arg(dark ? "#536158" : "#c8cec4", dark ? "#718178" : "#aab5a5"));
    list_->setItemDelegate(new PaperCoverDelegate(dark, list_));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto activate = [this](QListWidgetItem* item) {
        emit documentActivated(item->data(Qt::UserRole).toString());
    };
    connect(list_, &QListWidget::itemClicked, this, activate);
    auto openCurrent = [this, activate] {
        if (auto* item = list_->currentItem(); item && !item->isHidden()) activate(item);
    };
    connect(search_, &QLineEdit::returnPressed, this, openCurrent);
    connect(search_, &QLineEdit::textChanged, this, [this] {
        coverToken_.cancel();
        applySearch();
    });
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* open = new QShortcut(QKeySequence(key), list_);
        open->setContext(Qt::WidgetShortcut);
        connect(open, &QShortcut::activated, this, openCurrent);
    }
    setTabOrder(search_, list_);
    auto* documentsArea = new QVBoxLayout;
    documentsArea->setSpacing(0);
    documentsArea->addWidget(list_, 1);
    documentsArea->addStretch(0);
    layout->addLayout(documentsArea, 1);

    coverTimer_ = new QTimer(this);
    coverTimer_->setSingleShot(true);
    coverTimer_->setInterval(60);
    connect(coverTimer_, &QTimer::timeout, this, &DocumentShelf::loadVisibleCovers);
    connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { coverTimer_->start(); });
    list_->viewport()->installEventFilter(this);
    coverScan_ = new QFutureWatcher<CoverResult>(this);
    connect(coverScan_, &QFutureWatcher<CoverResult>::finished, this, [this] {
        const auto result = coverScan_->result();
        coverRunning_ = false;
        if (!result.cancelled) {
            if (result.image.isNull()) failedCovers_.insert(result.key);
            for (int i = 0; i < list_->count(); ++i) {
                auto* item = list_->item(i);
                if (item->data(CoverKeyRole).toString() != result.key) continue;
                item->setData(Qt::DecorationRole, result.image);
                item->setData(PreviewUnavailableRole, result.image.isNull());
            }
        }
        if (isVisible()) coverTimer_->start(0);
    });

    folderWatch_ = new QFileSystemWatcher(this);
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(200);
    connect(folderWatch_, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (isVisible()) refreshTimer_->start();
    });
    connect(refreshTimer_, &QTimer::timeout, this, &DocumentShelf::refresh);
    scan_ = new QFutureWatcher<ScanResult>(this);
    connect(scan_, &QFutureWatcher<ScanResult>::finished, this, [this] {
        const auto result = scan_->result();
        scanRunning_ = false;
        if (result.folder == folder_) display(result);
        if (refreshPending_) {
            refreshPending_ = false;
            refresh();
        }
    });
    folder_ = QSettings().value("library/folder").toString();
    folderPath_->setText(folder_);
    search_->setEnabled(!folder_.isEmpty());
    updateFolderWatch();
    message_->setText("Choose a folder to show its PDF documents here.");
    list_->hide();
}

DocumentShelf::~DocumentShelf() {
    // The scan uses the application-owned repository, which must outlive it.
    coverToken_.cancel();
    scan_->waitForFinished();
    coverScan_->waitForFinished();
}

void DocumentShelf::setFolder(const QString& folder) {
    coverToken_.cancel();
    folder_ = folder.isEmpty() ? QString() : QFileInfo(folder).absoluteFilePath();
    QSettings().setValue("library/folder", folder_);
    folderPath_->setText(folder_);
    folderPath_->setToolTip(folder_);
    scanError_.clear();
    list_->clear();
    list_->hide();
    search_->setEnabled(!folder_.isEmpty());
    search_->clear();
    updateFolderWatch();
    refresh();
    if (search_->isEnabled()) search_->setFocus();
}

void DocumentShelf::updateFolderWatch() {
    const auto watched = folderWatch_->directories();
    if (!watched.isEmpty()) folderWatch_->removePaths(watched);
    if (folder_.isEmpty()) return;
    if (QFileInfo(folder_).isDir()) folderWatch_->addPath(folder_);
    const QString parent = QFileInfo(folder_).absolutePath();
    if (parent != folder_ && QFileInfo(parent).isDir()) folderWatch_->addPath(parent);
}

void DocumentShelf::refresh() {
    refreshTimer_->stop();
    if (scanRunning_) {
        refreshPending_ = true;
        return;
    }
    updateFolderWatch();
    if (folder_.isEmpty()) {
        list_->clear();
        list_->hide();
        message_->setText("Choose a folder to show its PDF documents here.");
        return;
    }
    message_->setText("Loading documents…");
    const QString folder = folder_;
    auto* documents = documents_;
    scanRunning_ = true;
    scan_->setFuture(QtConcurrent::run([folder, documents] {
        ScanResult result;
        result.folder = folder;
        const QFileInfo directory(folder);
        if (!directory.isDir() || !directory.isReadable()) {
            result.error = "This folder is unavailable. Reconnect it or choose another folder.";
            return result;
        }
        QHash<QString, reader::DocumentActivity> history;
        if (documents) {
            for (auto& activity : documents->documentActivity()) {
                const QFileInfo file(QString::fromStdString(activity.filePath));
                const QString canonical = file.canonicalFilePath();
                history.insert(canonical.isEmpty() ? file.absoluteFilePath() : canonical,
                               std::move(activity));
            }
        }
        const auto files = QDir(folder).entryInfoList(QDir::Files | QDir::Readable, QDir::NoSort);
        for (const QFileInfo& file : files) {
            if (file.suffix().compare("pdf", Qt::CaseInsensitive) != 0) continue;
            Entry entry;
            entry.path = file.absoluteFilePath();
            entry.name = file.fileName();
            const QByteArray identity = file.canonicalFilePath().toUtf8() + '\0' +
                QByteArray::number(file.size()) + ':' + QByteArray::number(file.lastModified().toMSecsSinceEpoch());
            entry.coverKey = QString::fromLatin1(QCryptographicHash::hash(
                "shelf-cover-v1:" + identity, QCryptographicHash::Sha256).toHex());
            const QDateTime created = file.birthTime().isValid() ? file.birthTime() : file.lastModified();
            entry.activity = created.toMSecsSinceEpoch();
            const auto previous = history.constFind(file.canonicalFilePath());
            if (previous != history.cend()) {
                entry.opened = previous->lastOpened >= entry.activity;
                entry.activity = std::max<qint64>(entry.activity, previous->lastOpened);
                entry.page = previous->page;
                entry.pageCount = previous->pageCount;
            }
            result.entries.push_back(std::move(entry));
        }
        std::sort(result.entries.begin(), result.entries.end(), [](const Entry& a, const Entry& b) {
            if (a.activity != b.activity) return a.activity > b.activity;
            const int nameOrder = QString::compare(a.name, b.name, Qt::CaseInsensitive);
            return nameOrder != 0 ? nameOrder < 0 : a.path < b.path;
        });
        return result;
    }));
}

void DocumentShelf::display(const ScanResult& result) {
    const QString selected = list_->currentItem()
        ? list_->currentItem()->data(Qt::UserRole).toString() : QString();
    QHash<QString, QImage> previousCovers;
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        const auto image = item->data(Qt::DecorationRole).value<QImage>();
        if (!image.isNull()) previousCovers.insert(item->data(CoverKeyRole).toString(), image);
    }
    list_->setUpdatesEnabled(false);
    list_->clear();
    for (const auto& entry : result.entries) {
        auto* item = new QListWidgetItem(QFileInfo(entry.name).completeBaseName(), list_);
        const QString progress = entry.page
            ? QString("Page %1 of %2").arg(*entry.page + 1).arg(entry.pageCount) : "Unread";
        item->setData(Qt::UserRole, entry.path);
        item->setData(CoverKeyRole, entry.coverKey);
        item->setData(ProgressRole, progress);
        item->setData(ActivityRole, recentActivity(entry.opened, entry.activity));
        if (entry.page) item->setData(PageRole, *entry.page);
        item->setData(PageCountRole, entry.pageCount);
        item->setData(Qt::DecorationRole, previousCovers.value(entry.coverKey));
        item->setData(PreviewUnavailableRole, failedCovers_.contains(entry.coverKey));
        item->setData(Qt::AccessibleTextRole, item->text() + ". " + progress);
        item->setToolTip(entry.path + '\n' + progress + '\n' +
            QString("%1 %2").arg(entry.opened ? "Opened" : "Created",
                QLocale().toString(QDateTime::fromMSecsSinceEpoch(entry.activity), QLocale::ShortFormat)));
        if (entry.path == selected) list_->setCurrentItem(item);
    }
    scanError_ = result.error;
    applySearch();
    list_->setUpdatesEnabled(true);
}

void DocumentShelf::applySearch() {
    static const QRegularExpression whitespace("\\s+");
    const auto terms = search_->text().split(whitespace, Qt::SkipEmptyParts);
    int matches = 0;
    QListWidgetItem* firstMatch = nullptr;
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        const QString name = QFileInfo(item->data(Qt::UserRole).toString()).fileName();
        const bool match = std::all_of(terms.cbegin(), terms.cend(), [&](const QString& term) {
            return name.contains(term, Qt::CaseInsensitive);
        });
        item->setHidden(!match);
        if (match) {
            ++matches;
            if (!firstMatch) firstMatch = item;
        }
    }
    auto* selected = list_->currentItem();
    if ((selected && selected->isHidden()) || (!selected && !terms.isEmpty()))
        list_->setCurrentItem(firstMatch);
    list_->setVisible(matches > 0);
    list_->doItemsLayout();
    updateGrid();
    coverTimer_->start();
    if (scanRunning_) return;
    if (folder_.isEmpty()) message_->setText("Choose a folder to show its PDF documents here.");
    else if (!scanError_.isEmpty()) message_->setText(scanError_);
    else if (list_->count() == 0) message_->setText("No PDF documents in this folder yet.");
    else if (matches == 0) message_->setText("No matching documents. Try a different search.");
    else if (!terms.isEmpty())
        message_->setText(QString("%1 of %2 PDF document%3").arg(matches).arg(list_->count())
                              .arg(list_->count() == 1 ? "" : "s"));
    else message_->setText(QString("%1 PDF document%2").arg(list_->count())
                              .arg(list_->count() == 1 ? "" : "s"));
}

void DocumentShelf::showOpenError(const QString& path) {
    message_->setText(QString("Could not open %1. The file may have moved or may not be a readable PDF.")
                          .arg(QFileInfo(path).fileName()));
}

void DocumentShelf::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
    coverTimer_->start();
    if (search_->isEnabled()) search_->setFocus();
}

void DocumentShelf::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    coverTimer_->stop();
    coverToken_.cancel();
}

bool DocumentShelf::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_->viewport() && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, [this] {
            updateGrid();
            coverTimer_->start();
        });
    }
    return QWidget::eventFilter(watched, event);
}

void DocumentShelf::updateGrid() {
    const int width = std::max(1, list_->viewport()->width());
    const int columns = std::max(1, width / kCardWidth);
    const QSize cell(width / columns, kCardHeight);
    if (cell != list_->gridSize()) list_->setGridSize(cell);
}

void DocumentShelf::loadVisibleCovers() {
    if (!isVisible() || !list_->isVisible()) return;
    const QRect visible = list_->viewport()->rect();
    const QRect nearby = visible.adjusted(0, -kCardHeight, 0, kCardHeight);
    QListWidgetItem* next = nullptr;
    bool nextVisible = false;
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        const QRect bounds = list_->visualItemRect(item);
        if (item->isHidden() || !bounds.intersects(nearby)) {
            item->setData(Qt::DecorationRole, QVariant());
            continue;
        }
        if (!item->data(Qt::DecorationRole).value<QImage>().isNull() ||
            failedCovers_.contains(item->data(CoverKeyRole).toString())) continue;
        const bool inView = bounds.intersects(visible);
        if (!next || (inView && !nextVisible)) {
            next = item;
            nextVisible = inView;
        }
    }
    if (!next || coverRunning_) return;
    const QString path = next->data(Qt::UserRole).toString();
    const QString key = next->data(CoverKeyRole).toString();
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/paper-covers";
    coverToken_ = reader::CancellationToken{};
    const auto token = coverToken_;
    coverRunning_ = true;
    coverScan_->setFuture(QtConcurrent::run([path, key, cache, token] {
        CoverResult result;
        result.key = key;
        const QString cached = cache + '/' + key + ".png";
        if (!token.cancelled()) result.image.load(cached);
        if (result.image.isNull() && !token.cancelled()) {
            PopplerBridge document;
            if (document.open(path) && !token.cancelled()) {
                const QSize size = document.pageSize(0).scaled(QSizeF(420, 560), Qt::KeepAspectRatio).toSize();
                result.image = document.renderPage(0, size, [token] { return token.cancelled(); });
                if (!result.image.isNull() && !token.cancelled() && QDir().mkpath(cache)) {
                    QSaveFile output(cached);
                    if (output.open(QIODevice::WriteOnly) && result.image.save(&output, "PNG")) output.commit();
                }
            }
        }
        result.cancelled = token.cancelled();
        return result;
    }));
}
