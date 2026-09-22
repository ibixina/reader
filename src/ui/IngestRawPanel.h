#pragma once
#include <QWidget>

class QTextBrowser;

namespace reader {
struct PaperAnalysis;
}

// Ingest Raw tab: the actual ingest output, unvarnished. For local ingest
// this is the PaperAnalysis manifest as pretty JSON; for ChatGPT ingest it
// is the model's raw response text exactly as received.
class IngestRawPanel : public QWidget {
    Q_OBJECT
public:
    explicit IngestRawPanel(QWidget* parent = nullptr);
    void showEmpty();
    void showJson(const QString& metaLine, const QString& prettyJson);
    void showRawText(const QString& metaLine, const QString& rawText);

private:
    QTextBrowser* view_ = nullptr;
};
