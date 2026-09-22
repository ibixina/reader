#include "ui/IngestRawPanel.h"
#include <QFontDatabase>
#include <QTextBrowser>
#include <QVBoxLayout>

IngestRawPanel::IngestRawPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    view_ = new QTextBrowser(this);
    view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view_->setLineWrapMode(QTextBrowser::NoWrap);
    view_->setPlaceholderText("No ingest output yet — click Ingest first.");
    layout->addWidget(view_, 1);
}

void IngestRawPanel::showEmpty() {
    view_->clear();
}

void IngestRawPanel::showJson(const QString& metaLine, const QString& prettyJson) {
    view_->setPlainText(metaLine + "\n\n" + prettyJson);
}

void IngestRawPanel::showRawText(const QString& metaLine, const QString& rawText) {
    view_->setPlainText(metaLine + "\n\n" + rawText);
}
