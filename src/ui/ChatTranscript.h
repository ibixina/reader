#pragma once
#include <QWidget>
#include <vector>

class QTextBrowser;
class QTimer;

#ifdef HAVE_WEBENGINE
class QWebEngineProfile;
class QWebEngineView;
#endif

// Conversation renderer shared by the native chat UI. CommonMark and code
// render in every Qt build. When WebEngine is available, a local-only KaTeX
// page renders full equations; the QTextBrowser remains the honest fallback.
class ChatTranscript : public QWidget {
    Q_OBJECT
public:
    explicit ChatTranscript(QWidget* parent = nullptr);
    ~ChatTranscript() override;
    void clear();
    void appendRole(const QString& role);
    void appendMarkdown(const QString& markdown,
                        const std::vector<std::pair<QString, int>>& sourceLinks = {});
    void appendHtml(const QString& html);
    void appendPlainText(const QString& text);
    QTextBrowser* fallbackBrowser() const { return fallback_; }
    bool richMathEnabled() const;

signals:
    void sourceActivated(int index);
    void sourceHovered(int index);
    void sourceHoverCleared();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void scheduleRender();
    void renderRich();
    static QString fallbackMath(QString markdown);
    static QString markdownHtml(const QString& markdown,
                                const std::vector<std::pair<QString, int>>& sourceLinks);

    QTextBrowser* fallback_ = nullptr;
    QString bodyHtml_;
    QTimer* renderTimer_ = nullptr;
#ifdef HAVE_WEBENGINE
    QWebEngineProfile* profile_ = nullptr;
    QWebEngineView* rich_ = nullptr;
#endif
};
