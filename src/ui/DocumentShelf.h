#pragma once
#include "core/CancellationToken.h"
#include <QFutureWatcher>
#include <QImage>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <optional>

class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QShowEvent;
class QHideEvent;
class QTimer;
class QListWidget;

namespace reader {
class DocumentRepository;
}

class DocumentShelf : public QWidget {
    Q_OBJECT
public:
    explicit DocumentShelf(reader::DocumentRepository* documents, QWidget* parent = nullptr);
    ~DocumentShelf() override;
    QString folder() const { return folder_; }
    void setFolder(const QString& folder);
    void refresh();
    void showOpenError(const QString& path);

signals:
    void documentActivated(const QString& path);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        QString path;
        QString name;
        qint64 activity = 0;
        bool opened = false;
        std::optional<int> page;
        int pageCount = 0;
        QString coverKey;
    };
    struct ScanResult {
        QString folder;
        QString error;
        QVector<Entry> entries;
    };
    struct CoverResult {
        QString key;
        QImage image;
        bool cancelled = false;
    };
    void updateFolderWatch();
    void display(const ScanResult& result);
    void applySearch();
    void updateGrid();
    void loadVisibleCovers();
    reader::DocumentRepository* documents_;
    QString folder_;
    QString scanError_;
    QLineEdit* folderPath_;
    QLineEdit* search_;
    QListWidget* list_;
    QLabel* message_;
    QFileSystemWatcher* folderWatch_;
    QTimer* refreshTimer_;
    QFutureWatcher<ScanResult>* scan_;
    QFutureWatcher<CoverResult>* coverScan_;
    QTimer* coverTimer_;
    reader::CancellationToken coverToken_;
    QSet<QString> failedCovers_;
    bool scanRunning_ = false;
    bool coverRunning_ = false;
    bool refreshPending_ = false;
};
