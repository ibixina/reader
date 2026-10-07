#pragma once

#include <QDialog>

class DocumentFilter;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QTimer;

class DocumentOpenDialog : public QDialog {
    Q_OBJECT
public:
    explicit DocumentOpenDialog(const QString& folder, QWidget* parent = nullptr);
    QString selectedFile() const { return selectedFile_; }

private:
    void setFolder(const QString& folder);
    void updateResults();
    void openSelection();

    QString folder_;
    QString selectedFile_;
    QFileSystemModel* files_ = nullptr;
    DocumentFilter* filter_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListView* results_ = nullptr;
    QLabel* folderLabel_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* open_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
};
