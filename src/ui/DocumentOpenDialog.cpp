#include "ui/DocumentOpenDialog.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

class DocumentFilter : public QSortFilterProxyModel {
public:
    explicit DocumentFilter(QFileSystemModel* files, QObject* parent)
        : QSortFilterProxyModel(parent), files_(files) {
        setSourceModel(files);
        setSortCaseSensitivity(Qt::CaseInsensitive);
        sort(0);
    }

    void setSearch(const QString& folder, const QString& query) {
        static const QRegularExpression whitespace("\\s+");
        const auto terms = query.split(whitespace, Qt::SkipEmptyParts);
        if (folder_ == folder && terms_ == terms) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
#endif
        folder_ = folder;
        terms_ = terms;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        endFilterChange(Direction::Rows);
#else
        invalidateFilter();
#endif
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        const auto info = files_->fileInfo(files_->index(row, 0, parent));
        if (info.isDir()) {
            const QString path = QDir::cleanPath(info.absoluteFilePath());
            const QString prefix = path.endsWith('/') ? path : path + '/';
            // Keep the current folder and its ancestors in the proxy tree.
            if (path == folder_ || folder_.startsWith(prefix)) return true;
        } else if (!info.isFile() || info.suffix().compare("pdf", Qt::CaseInsensitive) != 0) {
            return false;
        }
        if (!info.isReadable()) return false;
        for (const auto& term : terms_)
            if (!info.fileName().contains(term, Qt::CaseInsensitive)) return false;
        return true;
    }

    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        const bool leftDir = files_->isDir(left);
        const bool rightDir = files_->isDir(right);
        if (leftDir != rightDir) return leftDir;
        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    QFileSystemModel* files_;
    QString folder_;
    QStringList terms_;
};

DocumentOpenDialog::DocumentOpenDialog(const QString& folder, QWidget* parent)
    : QDialog(parent) {
    setObjectName("documentOpenDialog");
    setWindowTitle("Open paper");
    resize(740, 540);
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    QPalette colors = palette();
    colors.setColor(QPalette::Window, QColor(dark ? "#202722" : "#f6f5f0"));
    colors.setColor(QPalette::WindowText, QColor(dark ? "#e9ede9" : "#252c27"));
    colors.setColor(QPalette::Text, colors.color(QPalette::WindowText));
    colors.setColor(QPalette::Base, QColor(dark ? "#2b342d" : "#fbfbf8"));
    colors.setColor(QPalette::Highlight, QColor(dark ? "#365a44" : "#dce9dd"));
    colors.setColor(QPalette::HighlightedText, colors.color(QPalette::Text));
    setPalette(colors);
    setStyleSheet(QString(
        "QLineEdit { padding: 10px 12px; border: 1px solid %1; border-radius: 6px; }"
        "QLineEdit:focus { border-color: #5c896e; }"
        "QListView { border: 1px solid %1; border-radius: 6px; padding: 4px; }"
        "QListView::item { padding: 8px; }"
        "QListView::item:selected { background: %4; color: %5; border-radius: 4px; }"
        "QPushButton { padding: 7px 13px; border: 1px solid %1; border-radius: 6px; background: %2; }"
        "QPushButton:hover, QPushButton:focus { border-color: #5c896e; }"
        "QPushButton:disabled { color: %3; }"
        "QLabel#documentFolder, QLabel#documentSearchStatus { color: %3; }")
        .arg(dark ? "#4a554d" : "#d6d9d1", dark ? "#2b342d" : "#fbfbf8",
             dark ? "#a0aaa3" : "#777d77", dark ? "#365a44" : "#dce9dd",
             dark ? "#e9ede9" : "#252c27"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(14);
    auto* heading = new QLabel("Open a paper", this);
    QFont headingFont = heading->font();
    headingFont.setPointSize(20);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto* folderRow = new QHBoxLayout;
    up_ = new QPushButton("Up", this);
    up_->setObjectName("documentUp");
    up_->setToolTip("Go to the parent folder");
    up_->setAutoDefault(false);
    connect(up_, &QPushButton::clicked, this, [this] {
        QDir parentFolder(folder_);
        if (parentFolder.cdUp()) setFolder(parentFolder.absolutePath());
    });
    folderRow->addWidget(up_);
    folderLabel_ = new QLabel(this);
    folderLabel_->setObjectName("documentFolder");
    folderLabel_->setTextFormat(Qt::PlainText);
    folderLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folderLabel_->setWordWrap(true);
    folderRow->addWidget(folderLabel_, 1);
    auto* choose = new QPushButton("Choose folder…", this);
    choose->setAutoDefault(false);
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(this, "Choose documents folder", folder_);
        if (!path.isEmpty()) setFolder(path);
    });
    folderRow->addWidget(choose);
    layout->addLayout(folderRow);

    search_ = new QLineEdit(this);
    search_->setObjectName("documentSearch");
    search_->setPlaceholderText("Search this folder…");
    search_->setAccessibleName("Search document names");
    search_->setToolTip("Filter PDF names as you type. Press Enter to open the selected result.");
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);

    files_ = new QFileSystemModel(this);
    files_->setReadOnly(true);
    files_->setOption(QFileSystemModel::DontUseCustomDirectoryIcons);
    files_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    filter_ = new DocumentFilter(files_, this);
    results_ = new QListView(this);
    results_->setObjectName("documentResults");
    results_->setAccessibleName("Matching documents and folders");
    results_->setModel(filter_);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_->setUniformItemSizes(true);
    layout->addWidget(results_, 1);
    status_ = new QLabel(this);
    status_->setObjectName("documentSearchStatus");
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    open_ = buttons->button(QDialogButtonBox::Open);
    open_->setObjectName("documentOpen");
    for (auto* button : buttons->buttons())
        if (auto* push = qobject_cast<QPushButton*>(button)) push->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &DocumentOpenDialog::openSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(0);
    connect(refreshTimer_, &QTimer::timeout, this, &DocumentOpenDialog::updateResults);
    auto refresh = [this] { refreshTimer_->start(); };
    connect(filter_, &QAbstractItemModel::rowsInserted, this, refresh);
    connect(filter_, &QAbstractItemModel::rowsRemoved, this, refresh);
    connect(filter_, &QAbstractItemModel::modelReset, this, refresh);
    connect(filter_, &QAbstractItemModel::layoutChanged, this, refresh);
    connect(files_, &QFileSystemModel::directoryLoaded, this, refresh);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& query) {
        filter_->setSearch(folder_, query);
        results_->setCurrentIndex({});
        updateResults();
    });
    connect(search_, &QLineEdit::returnPressed, this, &DocumentOpenDialog::openSelection);
    connect(results_, &QListView::activated, this, &DocumentOpenDialog::openSelection);
    connect(results_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) { open_->setEnabled(current.isValid()); });
    const QString start = QFileInfo(folder).isDir() ? folder
        : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    setFolder(QFileInfo(start).isDir() ? start : QDir::homePath());
    setTabOrder(search_, results_);
    setTabOrder(results_, open_);
    search_->setFocus();
}

void DocumentOpenDialog::setFolder(const QString& folder) {
    if (!QFileInfo(folder).isDir()) return;
    folder_ = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    folderLabel_->setText(QDir::toNativeSeparators(folder_));
    up_->setEnabled(!QDir(folder_).isRoot());
    filter_->setSearch(folder_, {});
    results_->setRootIndex(filter_->mapFromSource(files_->setRootPath(folder_)));
    results_->setCurrentIndex({});
    search_->clear();
    updateResults();
    search_->setFocus();
}

void DocumentOpenDialog::updateResults() {
    const auto root = filter_->mapFromSource(files_->index(folder_));
    if (results_->rootIndex() != root) results_->setRootIndex(root);
    int documents = 0;
    QModelIndex firstDocument;
    for (int row = 0; row < filter_->rowCount(root); ++row) {
        const auto index = filter_->index(row, 0, root);
        if (!files_->isDir(filter_->mapToSource(index))) {
            ++documents;
            if (!firstDocument.isValid()) firstDocument = index;
        }
    }
    if (!results_->currentIndex().isValid()) {
        results_->setCurrentIndex(firstDocument);
        if (!firstDocument.isValid() && search_->text().trimmed().isEmpty())
            results_->setCurrentIndex(filter_->index(0, 0, root));
    }
    open_->setEnabled(results_->currentIndex().isValid());
    if (documents == 0) {
        status_->setText(search_->text().trimmed().isEmpty()
            ? "No PDFs in this folder. Open a folder to browse its documents."
            : "No matching PDFs in this folder.");
    } else {
        status_->setText(QString("%1 %2%3").arg(documents)
            .arg(search_->text().trimmed().isEmpty() ? "" : "matching ")
            .arg(documents == 1 ? "PDF" : "PDFs"));
    }
}

void DocumentOpenDialog::openSelection() {
    if (!results_->currentIndex().isValid()) return;
    const auto info = files_->fileInfo(filter_->mapToSource(results_->currentIndex()));
    if (info.isDir()) {
        setFolder(info.absoluteFilePath());
    } else if (info.isFile() && info.isReadable() && info.suffix().compare("pdf", Qt::CaseInsensitive) == 0) {
        selectedFile_ = info.absoluteFilePath();
        accept();
    }
}
