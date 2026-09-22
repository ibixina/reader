#include "ui/MainWindow.h"
#include "ai/OpenAIProvider.h"
#include "analysis/StructureDetector.h"
#include "app/Application.h"
#include "core/Json.h"
#include "core/Types.h"
#include "pdf/QtPdfEngine.h"
#include "pdf/TextExtractor.h"
#include "ui/ChatPanel.h"
#include "ui/CommandPalette.h"
#include "ui/IngestRawPanel.h"
#include "ui/MapPanel.h"
#include "ui/OutlinePanel.h"
#include "ui/PdfView.h"
#include "ui/SearchPanel.h"
#include "ui/SummaryPanel.h"
#include "ui/ThumbnailPanel.h"
#include "ui/WebPanel.h"
#include <QFileDialog>
#include <QApplication>
#include <QFileInfo>
#include <QCheckBox>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QPdfDocument>
#include <QInputDialog>
#include <QPushButton>
#include <QPointer>
#include <QRectF>
#include <QSettings>
#include <QStandardPaths>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QToolBar>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>
#include <memory>
#include <optional>

namespace {

std::optional<reader::ReferenceImage> encodedEvidence(const QImage& image) {
    if (image.isNull()) return std::nullopt;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return std::nullopt;
    reader::ReferenceImage result;
    result.mimeType = "image/png";
    result.bytes.assign(bytes.cbegin(), bytes.cend());
    result.width = image.width();
    result.height = image.height();
    return result;
}

} // namespace

MainWindow::MainWindow(reader::Application* app, QWidget* parent)
    : QMainWindow(parent), app_(app) {
    QSettings settings;
    // Local parsing is the privacy boundary and a prerequisite for native
    // selection/search. It is an invariant rather than a best-effort toggle.
    app_->state.settings.parseLocally = true;
    app_->state.settings.storeEmbeddingsLocally =
        settings.value("reader/storeEmbeddingsLocally", true).toBool();
    app_->state.settings.sendOnlyRetrievedPassages =
        settings.value("reader/sendOnlyRetrievedPassages", true).toBool();
    app_->state.settings.allowCompleteUpload =
        settings.value("reader/allowCompleteUpload", false).toBool();
    app_->state.settings.copySelectionToClipboard =
        settings.value("reader/copySelectionToClipboard", false).toBool();
    app_->state.settings.aiEmphasis =
        settings.value("reader/aiEmphasis", "normal").toString().toStdString();
    app_->state.settings.showImportant = settings.value("reader/showImportant", true).toBool();
    app_->state.settings.showDefinitions =
        settings.value("reader/showDefinitions", true).toBool();
    app_->state.settings.showResults = settings.value("reader/showResults", true).toBool();
    app_->state.settings.showLimitations =
        settings.value("reader/showLimitations", true).toBool();
    app_->state.settings.showMethods = settings.value("reader/showMethods", false).toBool();
    app_->state.aiPaneVisible = settings.value("reader/aiPaneVisible", true).toBool();
    app_->state.aiPaneCollapsed = settings.value("reader/aiPaneCollapsed", false).toBool();
    aiPaneLeft_ = settings.value("reader/aiPaneLeft", false).toBool();
    const bool savedAiPaneDetached = settings.value("reader/aiPaneDetached", false).toBool();
    readingStateTimer_ = new QTimer(this);
    readingStateTimer_->setSingleShot(true);
    connect(readingStateTimer_, &QTimer::timeout, this, &MainWindow::saveReadingState);
    toolbar_ = addToolBar("Reader");
    toolbar_->setObjectName("readerToolbar");
    auto addAction = [&](const QString& text, auto fn) {
        QAction* act = toolbar_->addAction(text);
        connect(act, &QAction::triggered, this, fn);
        return act;
    };
    addAction("Open", [this] {
        QString path = QFileDialog::getOpenFileName(this, "Open paper", "", "PDF (*.pdf)");
        if (!path.isEmpty()) openFile(path);
    });
    addAction("Back", [this] {
        auto e = app_->state.history.back();
        navigateTo(e);
    });
    addAction("Forward", [this] {
        auto e = app_->state.history.forward();
        navigateTo(e);
    });
    addAction("Fit width", [this] { pdf_->fitWidth(); });
    addAction("Fit page", [this] { pdf_->fitPage(); });
    addAction("Rotate", [this] { pdf_->rotate(); });
    addAction("Page mode", [this] { pdf_->setPageMode(!pdf_->pageMode()); });
    addAction("Fullscreen", [this] {
        if (isFullScreen()) showNormal();
        else showFullScreen();
    });
    addAction("Move AI left", [this] {
        aiPaneLeft_ = !aiPaneLeft_;
        if (aiPaneLeft_) {
            splitter_->insertWidget(0, tabs_);
            splitter_->insertWidget(1, pdf_);
        } else {
            splitter_->insertWidget(0, pdf_);
            splitter_->insertWidget(1, tabs_);
        }
        persistWindowLayout();
    });
    addAction("Collapse AI", [this] { toggleAiCollapse(); });
    addAction("Detach AI", [this] { toggleAiDetach(); });
    addAction("Toggle AI", [this] { toggleAiPane(); });
    addAction("Reader tools", [this] { openReaderTools(); });
    addAction("Library", [this] { openLibrary(); });
    addAction("Annotations", [this] { openAnnotationManager(); });
    addAction("Settings", [this] { openSettings(); });

    ingestButton_ = new QPushButton("Ingest", this);
    ingestButton_->setEnabled(false); // enabled once text extraction lands
    ingestMenu_ = new QMenu(this);
    ingestMenu_->addAction("Re-ingest", [this] { runIngest(true); });
#ifdef HAVE_WEBENGINE
    ingestMenu_->addAction("Ingest via ChatGPT", [this] { runChatIngest(); });
#endif
    ingestMenu_->addAction("Clear AI analysis", [this] {
        app_->ingestor.clearAnalysis(app_->model.document.fileHash);
        app_->analysis.reset();
        ingestState_ = reader::IngestState::NotIngested;
        updateIngestButton();
        pdf_->refreshAiOverlays();
        map_->rebuild();
        summary_->rebuild();
        ingestRaw_->showEmpty();
    });
    ingestButton_->setMenu(ingestMenu_);
    connect(ingestButton_, &QPushButton::clicked, [this] {
        if (ingestState_ == reader::IngestState::Ingested) ingestMenu_->popup(QCursor::pos());
        else runIngest(false);
    });
    toolbar_->addWidget(ingestButton_);
#ifdef HAVE_WEBENGINE
    addAction("Browser Chat", [this] { openWebChat(); });
#endif
    toolbar_->addSeparator();
    QAction* pinToolbarAct = toolbar_->addAction("Pin toolbar");
    pinToolbarAct->setCheckable(true);
    pinToolbarAct->setToolTip("Keep the top toolbar visible instead of auto-hiding");
    toolbarPinned_ = settings.value("reader/toolbarPinned", false).toBool();
    pinToolbarAct->setChecked(toolbarPinned_);
    connect(pinToolbarAct, &QAction::triggered, this, [this](bool checked) {
        setToolbarPinned(checked);
    });

    splitter_ = new QSplitter(this);
    pdf_ = new PdfView(app_, splitter_);
    tabs_ = new QTabWidget(splitter_);
    chat_ = new ChatPanel(app_, tabs_);
    QPointer<PdfView> reader(pdf_);
    chat_->setReaderServices(
        [reader]() -> std::optional<reader::SemanticSearchSnapshot> {
            return reader ? reader->semanticSnapshot() : std::nullopt;
        },
        [reader](const reader::ContextReference& reference,
                 std::function<void(std::optional<reader::ReferenceImage>)> callback) mutable {
            if (!reader) {
                callback(std::nullopt);
                return;
            }
            reader->requestReferenceImage(reference, std::move(callback));
        });
    tabs_->addTab(chat_, "Chat");
    map_ = new MapPanel(app_, tabs_);
    summary_ = new SummaryPanel(app_, tabs_);
    ingestRaw_ = new IngestRawPanel(tabs_);
    tabs_->addTab(map_, "Map");
    tabs_->addTab(summary_, "Summary");
    tabs_->addTab(ingestRaw_, "Ingest Raw");
#ifdef HAVE_WEBENGINE
    ensureWebPanel();
    tabs_->setCurrentWidget(web_);
#endif
    splitter_->addWidget(pdf_);
    splitter_->addWidget(tabs_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    setCentralWidget(splitter_);
    if (!app_->state.aiPaneVisible) tabs_->hide();

    // Reader-owned navigation/search tools stay separate from AI tabs and
    // remain usable while ingest or chat work is running.
    readerDock_ = new QDockWidget("Reader tools", this);
    readerDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    readerTabs_ = new QTabWidget(readerDock_);
    searchPanel_ = new SearchPanel(app_, pdf_, readerTabs_);
    outlinePanel_ = new OutlinePanel(app_, readerTabs_);
    thumbnailPanel_ = new ThumbnailPanel(app_, pdf_, readerTabs_);
    readerTabs_->addTab(searchPanel_, "Search");
    readerTabs_->addTab(outlinePanel_, "Outline");
    readerTabs_->addTab(thumbnailPanel_, "Pages");
    readerDock_->setWidget(readerTabs_);
    addDockWidget(Qt::LeftDockWidgetArea, readerDock_);
    readerDock_->hide();

    statusPage_ = new QLabel("No document", this);
    statusAi_ = new QLabel("Ready", this);
    statusAi_->setToolTip("Document AI state: click Ingest for highlights, summaries and Map data.");
    statusBar()->addWidget(statusPage_, 1);
    statusBar()->addWidget(statusAi_);

    connect(pdf_, &PdfView::selectionChanged, this, [this](const reader::DocumentAnchor& anchor) {
#ifdef HAVE_WEBENGINE
        if (web_) {
            web_->refreshContext();
            if (!anchor.anchorText.empty()) web_->focusQuestion();
        }
#endif
        chat_->refreshContextChips();
        if (app_->state.settings.copySelectionToClipboard && !anchor.anchorText.empty())
            QGuiApplication::clipboard()->setText(QString::fromStdString(anchor.anchorText));
        statusPage_->setText(
            QString("p.%1 · selection → context").arg(app_->state.page + 1));
    });
    connect(pdf_, &PdfView::pageChanged, this, [this](int page) {
        app_->state.page = page;
        app_->state.readerState.page = page;
        if (const reader::Section* s = app_->model.sectionForPage(page))
            app_->state.readerState.section = s->id;
        if (const reader::TextBlock* b = app_->model.blockAtPage(page, 400))
            app_->state.readerState.paragraphBlock = b->id;
        pdf_->schedulePrefetchAround(page);
#ifdef HAVE_WEBENGINE
        if (web_) web_->refreshContext();
#endif
        chat_->refreshContextChips();
        statusPage_->setText(QString("p.%1").arg(page + 1));
        if (!restoringHistory_) {
            const auto state = pdf_->captureState();
            app_->state.history.visit({state.page, static_cast<double>(state.scrollY), state.zoom,
                                       state.selection});
        }
        scheduleReadingStateSave();
    });
    connect(pdf_, &PdfView::zoomChanged, this,
            [this](double) { scheduleReadingStateSave(); });
    connect(pdf_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
                app_->state.scrollY = value;
                scheduleReadingStateSave();
            });
    connect(chat_, &ChatPanel::sourceClicked, this,
            [this](const reader::DocumentAnchor& a) { navigateToAnchor(a); });
    connect(chat_, &ChatPanel::sourceHovered, this,
            [this](const reader::DocumentAnchor& a) { pdf_->setHoverAnchor(a); });
    connect(chat_, &ChatPanel::sourceHoverCleared, this,
            [this] { pdf_->clearHoverAnchor(); });
    connect(pdf_, &PdfView::sourceActivated, this,
            [this](const reader::DocumentAnchor& a) { navigateToAnchor(a); });
    connect(pdf_, &PdfView::linkActivated, this, [this](int page, const QString& uri) {
        if (uri.isEmpty()) {
            const auto state = pdf_->captureState();
            app_->state.history.visit({state.page, static_cast<double>(state.scrollY), state.zoom,
                                       state.selection});
            pdf_->goToPage(page);
        } else {
            QDesktopServices::openUrl(QUrl(uri));
        }
    });
    connect(pdf_, &PdfView::objectClicked, this,
            [this](const reader::DocumentAnchor& a, const QString& kind) {
                (void)kind;
                navigateToAnchor(a);
                auto built = reader::contextReferenceForObject(app_->model, a);
                if (!built) return;
                reader::ContextReference reference = std::move(*built);
                const auto id = reference.id;
                const auto document = reference.anchor.document;
                app_->context.setCurrentSelection(reference);
                chat_->refreshContextChips();
                if (reference.type != reader::ReferenceType::Figure &&
                    reference.type != reader::ReferenceType::Table)
                    return;
                pdf_->requestReferenceImage(
                    reference,
                    [this, reference = std::move(reference), id, document](
                        std::optional<reader::ReferenceImage> image) mutable {
                        if (!image || app_->model.document.id != document) return;
                        const auto context = app_->context.currentContext();
                        if (context.temporary.empty() || context.temporary.front().id != id)
                            return;
                        reference.image = std::move(image);
                        app_->context.setCurrentSelection(std::move(reference));
                        chat_->refreshContextChips();
                    });
            });
    connect(pdf_, &PdfView::bookmarkRequested, this,
            [this](const reader::DocumentAnchor& a) {
                if (app_->annotations && !app_->model.document.id.empty()) {
                    reader::UserAnnotation bookmark;
                    bookmark.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                    bookmark.anchor = a;
                    bookmark.kind = "bookmark";
                    bookmark.color = "#9b59b6";
                    app_->annotations->saveAnnotation(app_->model.document.id, bookmark);
                    pdf_->refreshUserOverlays();
                }
                navigateToAnchor(a);
            });
    connect(pdf_, &PdfView::regionCaptured, this,
            [this](const reader::DocumentAnchor& a, const QImage& image) {
                if (image.isNull() || app_->model.document.id.empty()) return;
                const auto root = QDir(QStandardPaths::writableLocation(
                                           QStandardPaths::AppDataLocation))
                                      .filePath(QStringLiteral("regions"));
                QDir().mkpath(root);
                const std::string id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                image.save(root + "/" + QString::fromStdString(id) + ".png");
                if (app_->annotations) {
                    reader::UserAnnotation region;
                    region.id = id;
                    region.anchor = a;
                    region.kind = "region";
                    region.color = "#4f8cff";
                    app_->annotations->saveAnnotation(app_->model.document.id, region);
                    pdf_->refreshUserOverlays();
                }
                if (auto encoded = encodedEvidence(image)) {
                    reader::ContextReference reference;
                    reference.type = reader::ReferenceType::TextSelection;
                    reference.anchor = a;
                    reference.displayName = "Captured region";
                    reference.extractedText = a.anchorText;
                    reference.caption = "Locally captured PDF region";
                    reference.image = std::move(encoded);
                    app_->context.setCurrentSelection(std::move(reference));
                    chat_->refreshContextChips();
                }
            });
    connect(searchPanel_, &SearchPanel::anchorActivated, this,
            [this](const reader::DocumentAnchor& a) { navigateToAnchor(a); });
    connect(outlinePanel_, &OutlinePanel::anchorActivated, this,
            [this](const reader::DocumentAnchor& a) { navigateToAnchor(a); });
    connect(thumbnailPanel_, &ThumbnailPanel::pageActivated, this, [this](int page) {
        if (pdf_) {
            const auto state = pdf_->captureState();
            app_->state.history.visit({state.page, static_cast<double>(state.scrollY), state.zoom,
                                       state.selection});
            pdf_->goToPage(page);
        }
    });
    connect(map_, &MapPanel::askAbout, this, [this](const QString& id) {
        if (app_->analysis) {
            for (const auto& node : app_->analysis->concepts) {
                if (QString::fromStdString(node.id) != id || node.sources.empty()) continue;
                if (const auto* block = app_->model.findBlock(node.sources.front())) {
                    const auto anchor = reader::anchorForBlock(app_->model, *block);
                    reader::ContextReference reference;
                    reference.id = id.toStdString();
                    reference.type = reader::ReferenceType::Concept;
                    reference.anchor = anchor;
                    reference.displayName = node.name;
                    reference.extractedText = node.description;
                    app_->context.setCurrentSelection(std::move(reference));
                    chat_->refreshContextChips();
                }
                break;
            }
        }
        tabs_->setCurrentWidget(chat_);
        chat_->focusQuestion();
    });
    connect(map_, &MapPanel::jumpToSource, this, [this](const QString& id) {
        if (!app_->analysis) return;
        for (const auto& node : app_->analysis->concepts) {
            if (QString::fromStdString(node.id) != id) continue;
            if (!node.sources.empty()) {
                if (const auto* block = app_->model.findBlock(node.sources.front()))
                    navigateToAnchor(reader::anchorForBlock(app_->model, *block));
            }
        }
    });
    connect(map_, &MapPanel::sourceActivated, this,
            [this](const reader::DocumentAnchor& anchor) { navigateToAnchor(anchor); });
    connect(summary_, &SummaryPanel::sourceClicked, this,
            [this](const reader::DocumentAnchor& a) { navigateToAnchor(a); });
    connect(pdf_, &PdfView::askAiRequested, this, [this] {
        tabs_->setCurrentWidget(chat_);
        chat_->focusQuestion();
    });
#ifdef HAVE_WEBENGINE
    connect(pdf_, &PdfView::quickAskRequested, this, [this](const QString& seed) {
        if (web_) {
            tabs_->setCurrentWidget(web_);
            web_->focusQuestionWithSeed(seed);
        } else {
            tabs_->setCurrentWidget(chat_);
            if (seed.isEmpty()) chat_->focusQuestion();
            else chat_->prepareQuestion(seed);
        }
    });
#else
    connect(pdf_, &PdfView::quickAskRequested, this, [this](const QString& seed) {
        tabs_->setCurrentWidget(chat_);
        if (seed.isEmpty()) chat_->focusQuestion();
        else chat_->prepareQuestion(seed);
    });
#endif
    connect(searchPanel_, &SearchPanel::anchorActivated, this,
            [this](const reader::DocumentAnchor&) {
                edgeRevealActive_ = false;
                readerDock_->hide();
            });
    connect(pdf_, &PdfView::findRequested, this, [this] {
        if (readerDock_) readerDock_->show();
        edgeRevealActive_ = false;
        if (searchPanel_) searchPanel_->focusQuery();
    });
    connect(pdf_, &PdfView::commandPaletteRequested, this, [this] {
        CommandPalette palette(app_, this);
        connect(&palette, &CommandPalette::commandChosen, this,
                &MainWindow::executeCommand);
        palette.exec();
    });
    connect(pdf_, &PdfView::sidecarToggleRequested, this, &MainWindow::toggleAiPane);
    connect(pdf_, &PdfView::tabRequested, this, [this](int index) {
        if (index >= 0 && index < tabs_->count()) tabs_->setCurrentIndex(index);
    });
    connect(pdf_, &PdfView::historyBackRequested, this, [this] {
        navigateTo(app_->state.history.back());
    });
    connect(pdf_, &PdfView::historyForwardRequested, this, [this] {
        navigateTo(app_->state.history.forward());
    });

    setupShortcuts();
    setupToolbarAutoHide();
    setupEdgeReveal();
    setWindowTitle("Paper Reader");
    resize(1280, 800);
    const QByteArray savedGeometry = settings.value("reader/windowGeometry").toByteArray();
    if (!savedGeometry.isEmpty()) restoreGeometry(savedGeometry);
    // Sane initial divider: 60% reader, 40% AI, with a usable minimum so
    // the PDF pane can never collapse into an unreadable sliver.
    pdf_->setMinimumWidth(320);
    QTimer::singleShot(0, this, [this, savedAiPaneDetached] {
        int w = width() > 0 ? width() : 1280;
        const auto saved = QSettings().value("reader/splitterSizes").value<QByteArray>();
        if (!saved.isEmpty()) {
            QList<int> sizes;
            for (const auto& value : saved.split(',')) sizes.push_back(value.toInt());
            // A zero-width pane is never intentional (dragged past the edge
            // or a stale pre-fix layout): fall back to the default split
            // instead of stranding the AI chat invisible. An explicit hide
            // via Toggle AI is still honored through aiPaneVisible.
            constexpr int kMinPane = 200;
            const int total = sizes.value(0, 0) + sizes.value(1, 0);
            if (sizes.size() == 2 && total >= 2 * kMinPane && sizes[0] >= kMinPane &&
                sizes[1] >= kMinPane)
                splitter_->setSizes(sizes);
            else
                splitter_->setSizes({w * 3 / 5, w * 2 / 5});
        } else {
            splitter_->setSizes({w * 3 / 5, w * 2 / 5});
        }
        if (savedAiPaneDetached) toggleAiDetach();
        if (app_->state.aiPaneCollapsed) toggleAiCollapse();
    });
}

MainWindow::~MainWindow() {
    persistWindowLayout();
    saveReadingState();
    documentToken_.cancel();
    ingestToken_.cancel();
    if (activeIngestProvider_) activeIngestProvider_->cancel();
#ifdef HAVE_WEBENGINE
    if (web_) web_->cancelPending();
    ++chatIngestRequestId_;
#endif
    if (chat_) chat_->cancelPending();
    if (app_) app_->shutdown();
}

#ifdef HAVE_WEBENGINE
WebPanel* MainWindow::ensureWebPanel() {
    if (!web_) {
        web_ = new WebPanel(app_, tabs_);
        tabs_->addTab(web_, "Browser");
        connect(web_, &WebPanel::chatIngestResponse, this,
                &MainWindow::onChatIngestResponse, Qt::UniqueConnection);
    }
    return web_;
}

void MainWindow::openWebChat() {
    WebPanel* panel = ensureWebPanel();
    tabs_->setCurrentWidget(panel);
    panel->focusQuestion();
}
#endif

void MainWindow::scheduleReadingStateSave() {
    if (readingStateTimer_) readingStateTimer_->start(150);
}

void MainWindow::saveReadingState() {
    if (!app_ || !app_->documents || app_->state.openDocument.empty() || !pdf_) return;
    app_->state.scrollY = pdf_->verticalScrollBar()->value();
    app_->documents->saveReadingState(app_->state.openDocument, app_->state.page,
                                      app_->state.scrollY, app_->state.zoom);
}

void MainWindow::openFile(const QString& path) {
    // Stage 1 (§44): page 1 visible immediately; never wait for analysis.
    // Shared ownership with deleteLater deleter: background doc-lane jobs
    // may still reference the document after a reopen supersedes them.
    saveReadingState();
    const QString currentPath = QString::fromStdString(app_->model.document.filePath);
    const bool switchingDocuments = !currentPath.isEmpty() &&
                                    QFileInfo(currentPath).canonicalFilePath() !=
                                        QFileInfo(path).canonicalFilePath();
    if (switchingDocuments) {
        const auto context = app_->context.currentContext();
        for (const auto& reference : context.temporary)
            app_->context.removeReference(reference.id);
        for (const auto& reference : context.pinned)
            app_->context.removeReference(reference.id);
        if (chat_) chat_->refreshContextChips();
    }
    documentToken_.cancel();
    documentToken_ = reader::CancellationToken{};
    ingestToken_.cancel();
    if (activeIngestProvider_) activeIngestProvider_->cancel();
    activeIngestProvider_.reset();
    const unsigned long generation = ++openGeneration_;
#ifdef HAVE_WEBENGINE
    ++chatIngestRequestId_;
    if (web_) web_->cancelPending();
#endif
    if (chat_) chat_->cancelPending();
    auto deleter = [](QPdfDocument* d) { d->deleteLater(); };
    pdfDoc_ = std::shared_ptr<QPdfDocument>(new QPdfDocument, deleter);
    engine_ = std::make_shared<QtPdfEngine>(pdfDoc_);
    if (!engine_->open(path.toStdString()) || engine_->pageCount() == 0) {
        statusPage_->setText("Could not open PDF");
        return;
    }
    reader::DocumentModel& model = app_->model;
    model = reader::DocumentModel{};
    model.document.filePath = path.toStdString();
    model.document.title = QFileInfo(path).baseName().toStdString();
    model.document.pageCount = engine_->pageCount();
    pdf_->attachDocument(pdfDoc_, path);
    thumbnailPanel_->rebuild();
    outlinePanel_->rebuild();
    app_->state.page = 0;
    app_->state.scrollY = 0;
    app_->state.history.clear();
    app_->state.history.visit({0, 0, app_->state.zoom, std::nullopt});

    statusPage_->setText(QString("p.%1 / %2 · identifying…")
                             .arg(app_->state.page + 1)
                             .arg(model.document.pageCount));

    // Stages 2-4 on background lanes: extract -> structure -> index.
    // Cached ingest analysis loads immediately when present (§5.10).
    // Ingest stays disabled until text exists: ingesting an empty model
    // used to report a fake "Ingested ✓" with nothing to show.
    // Threading: the worker builds a LOCAL model; publishing to app_->model
    // happens in one queued UI-thread step. The UI never reads a model
    // that a worker is still mutating (that race crashed the app).
    docReady_ = false;
    updateIngestButton();
    const reader::CancellationToken documentToken = documentToken_;
    std::shared_ptr<QtPdfEngine> engine = engine_;
    const std::string sourcePath = path.toStdString();
    // Hashing can read a large PDF; keep it off the GUI thread. Identity and
    // every cache publication remain bound to this open generation.
    app_->extractPool.submit(
        [this, engine, sourcePath, generation, documentToken] {
            if (documentToken.cancelled()) return;
            const std::string fileHash = reader::sha256File(sourcePath);
            QMetaObject::invokeMethod(
                this,
                [this, engine, fileHash, generation, documentToken]() mutable {
                    if (generation != openGeneration_ || documentToken.cancelled()) return;
                    if (fileHash.empty()) {
                        statusPage_->setText("Could not identify PDF");
                        return;
                    }
                    auto& model = app_->model;
                    model.document.fileHash = fileHash;
                    model.document.id = fileHash;
                    app_->documents->saveDocument(model.document);
                    app_->state.openDocument = model.document.id;
                    chat_->refreshConversations();

                    int savedPage = 0;
                    double savedScroll = 0, savedZoom = 0;
                    if (app_->documents->loadReadingState(model.document.id, savedPage,
                                                          savedScroll, savedZoom)) {
                        pdf_->setZoom(savedZoom > 0 ? savedZoom : app_->state.zoom);
                        pdf_->goToPage(savedPage);
                        app_->state.scrollY = savedScroll;
                        app_->state.history.clear();
                        app_->state.history.visit({savedPage, savedScroll,
                                                   savedZoom > 0 ? savedZoom : app_->state.zoom,
                                                   std::nullopt});
                        QTimer::singleShot(0, this, [this, savedScroll, generation] {
                            if (generation != openGeneration_ || !pdf_) return;
                            pdf_->verticalScrollBar()->setValue(static_cast<int>(savedScroll));
                        });
                    }
                    statusPage_->setText(QString("p.%1 / %2")
                                             .arg(app_->state.page + 1)
                                             .arg(model.document.pageCount));

                    const reader::Document docInfo = model.document;
                    // Serial doc lane: pdfium is used from one background thread at a time.
                    app_->docLane.submit(
                        [this, engine, docInfo, generation, documentToken] {
                            if (documentToken.cancelled()) return;
                            reader::IdFactory ids;
                            reader::DocumentModel local;
                            local.document = docInfo;
                            const bool loadedModel =
                                app_->documents->loadModel(docInfo.id, docInfo.fileHash, local) &&
                                !local.blocks.empty();
                            if (!loadedModel) {
                                local.document = docInfo;
                                reader::TextExtractor extractor;
                                local.blocks = extractor.extract(*engine, ids, docInfo.id,
                                                                 docInfo.pageCount, &local.lineSpans);
                                reader::StructureDetector detector;
                                detector.detect(local, *engine, ids);
                                app_->documents->saveModel(local);
                            }
                            auto retrieval = std::make_shared<reader::RetrievalEngine>();
                            retrieval->index(local);
                            std::string cacheError;
                            auto cached = app_->documents->loadAnalysisCache(
                                docInfo.id, docInfo.fileHash, cacheError);
                            if (!cached)
                                cached = app_->ingestor.cachedAnalysis(docInfo.id, docInfo.fileHash);
                            QMetaObject::invokeMethod(
                                this,
                                [this, local = std::move(local), cached, loadedModel,
                                 retrieval = std::move(retrieval), cacheError,
                                 generation]() mutable {
                                    if (generation != openGeneration_) return;
                                    app_->model = std::move(local);
                                    app_->retrieval = std::move(*retrieval);
                                    docReady_ = !app_->model.blocks.empty();
                                    pdf_->distributeLines();
                                    pdf_->schedulePrefetchAround(app_->state.page);
                                    outlinePanel_->rebuild();
                                    thumbnailPanel_->rebuild();
                                    if (app_->state.settings.storeEmbeddingsLocally) {
                                        QSettings semanticSettings;
                                        const int dimension = semanticSettings
                                                                  .value("reader/embeddingDimension", 0)
                                                                  .toInt();
                                        const QString base = semanticSettings
                                                                 .value("reader/embeddingBaseUrl")
                                                                 .toString();
                                        const QString model = semanticSettings
                                                                  .value("reader/embeddingModel")
                                                                  .toString();
                                        if (dimension > 0 && !base.isEmpty() && !model.isEmpty()) {
                                            pdf_->configureSemantic({
                                                base.toStdString(),
                                                semanticSettings.value("reader/embeddingApiKey")
                                                    .toString()
                                                    .toStdString(),
                                                model.toStdString(),
                                                static_cast<std::size_t>(dimension), 120000});
                                            pdf_->loadSemanticCacheAsync(
                                                [this](bool loaded, const QString& error) {
                                                    if (!loaded && !error.contains("cache miss"))
                                                        statusAi_->setText(
                                                            "Semantic cache error: " + error);
                                                });
                                        }
                                    }
                                    if (cached && cached->usable()) {
                                        app_->analysis = *cached;
                                        ingestState_ = reader::IngestState::Ingested;
                                        updateIngestButton();
                                        pdf_->refreshAiOverlays();
                                        map_->rebuild();
                                        summary_->rebuild();
                                        showIngestOutput();
                                        statusAi_->setText("Ingested ✓ (cached)");
                                    } else {
                                        ingestState_ = reader::IngestState::NotIngested;
                                        updateIngestButton();
                                        statusAi_->setText(
                                            loadedModel
                                                ? "Loaded cached text · click Ingest for AI analysis"
                                                : (docReady_
                                                       ? "Click Ingest for AI highlights + summaries"
                                                       : "Extracting text…"));
                                        if (!cacheError.empty() && cacheError != "analysis cache miss")
                                            statusAi_->setText(
                                                "Analysis cache error: " +
                                                QString::fromStdString(cacheError));
                                    }
                                    statusPage_->setText(
                                        QString("p.%1 / %2 · %3 blocks")
                                            .arg(app_->state.page + 1)
                                            .arg(app_->model.document.pageCount)
                                            .arg(app_->model.blocks.size()));
                                    pdf_->refreshUserOverlays();
                                },
                                Qt::QueuedConnection);
                        },
                        documentToken);
                },
                Qt::QueuedConnection);
        },
        documentToken);
}

void MainWindow::runIngest(bool reingest) {
    (void)reingest;
    if (!docReady_ || app_->model.blocks.empty()) {
        statusAi_->setText("Still extracting text — try again in a moment.");
        return;
    }
    ingestState_ = reader::IngestState::Preparing;
    updateIngestButton();
    statusAi_->setText("Ingesting…");
    // Analysis lane; the reader stays interactive (§7).
    if (activeIngestProvider_) activeIngestProvider_->cancel();
    activeIngestProvider_.reset();
    const std::string ingestHash = app_->model.document.fileHash;
    const unsigned long generation = openGeneration_;
    const reader::DocumentModel modelSnapshot = app_->model;
    const reader::CancellationToken token = [&] {
        ingestToken_.cancel();
        ingestToken_ = reader::CancellationToken{};
        return ingestToken_;
    }();
    const bool allowUpload = app_->state.settings.allowCompleteUpload;
    const reader::ProviderConfig providerConfig = app_->providerConfig;
    std::shared_ptr<reader::OpenAIProvider> remoteProvider;
    if (allowUpload && !providerConfig.apiKey.empty()) {
        remoteProvider = std::make_shared<reader::OpenAIProvider>(providerConfig);
        activeIngestProvider_ = remoteProvider;
    }
    app_->analysisPool.submit(
        [this, ingestHash, generation, modelSnapshot, token, remoteProvider] {
        // Remote whole-paper step only with explicit opt-in (§59); the
        // default path stays fully local with zero document upload.
        reader::PaperIngestor::FetchAnalysis remote;
        if (remoteProvider) {
            remote = [remoteProvider](const std::string& prompt) {
                if (remoteProvider) return remoteProvider->complete("", prompt);
                return std::string{};
            };
        }
        auto result = app_->ingestor.ingest(
            modelSnapshot, remote,
            [this, generation, ingestHash](const reader::IngestProgress& p) {
                QMetaObject::invokeMethod(
                    this,
                    [this, p, generation, ingestHash] {
                        if (generation != openGeneration_ ||
                            ingestHash != app_->model.document.fileHash)
                            return;
                        onIngestProgress(p);
                    },
                    Qt::QueuedConnection);
            },
            token);
        QMetaObject::invokeMethod(
            this,
            [this, result, ingestHash, generation, token, remoteProvider] {
                if (generation != openGeneration_ ||
                    ingestHash != app_->model.document.fileHash || token.cancelled())
                    return; // reopened or cancelled meanwhile
                if (activeIngestProvider_ == remoteProvider) activeIngestProvider_.reset();
                if (result && result->usable()) {
                    app_->analysis = *result;
                    const bool cacheSaved = app_->documents->saveAnalysisCache(
                        app_->model.document.id, ingestHash, *result);
                    const bool refsSaved =
                        app_->documents->saveAnalysisRefs(app_->model.document.id, *result);
                    ingestState_ = reader::IngestState::Ingested;
                    statusAi_->setText(QString("Ingested ✓ · %1 passages, %2 concepts")
                                           .arg(app_->analysis->annotations.size())
                                           .arg(app_->analysis->concepts.size()));
                    if (!cacheSaved || !refsSaved)
                        statusAi_->setText("Ingested, but the local analysis cache could not be saved.");
                    pdf_->refreshAiOverlays();
                    map_->rebuild();
                    summary_->rebuild();
                    showIngestOutput();
                    tabs_->setCurrentWidget(summary_);
                } else {
                    ingestState_ = reader::IngestState::Failed;
                    statusAi_->setText("Ingest failed — no usable analysis produced.");
                }
                updateIngestButton();
            },
            Qt::QueuedConnection);
        }, token);
}

#ifdef HAVE_WEBENGINE
void MainWindow::runChatIngest() {
    if (!docReady_ || app_->model.blocks.empty()) {
        statusAi_->setText("Still extracting text — try again in a moment.");
        return;
    }
    WebPanel* panel = ensureWebPanel();
    tabs_->setCurrentWidget(panel);
    ingestState_ = reader::IngestState::Analyzing;
    updateIngestButton();
    statusAi_->setText("Asking ChatGPT to analyze the paper…");
    std::string prompt = app_->ingestor.buildWholePaperPrompt(app_->model);
    // Bound the upload: the whole-paper prompt can exceed what the web
    // composer accepts; analysis quality degrades gracefully.
    if (prompt.size() > 120000) {
        prompt.resize(120000);
        prompt += "\n[TRUNCATED: remaining blocks omitted]\n";
    }
    chatIngestHash_ = app_->model.document.fileHash;
    const quint64 requestId = ++chatIngestRequestId_;
    // Preferred path: the PDF rides along as an attachment and the model
    // reads full content natively; this short ID map is all the grounding
    // the manifest needs. The full text stays as the manual fallback.
    std::string skeleton = app_->ingestor.buildGroundedSkeletonPrompt(app_->model);
    panel->ingestViaChat(QString::fromStdString(app_->model.document.filePath),
                         QString::fromStdString(prompt),
                         QString::fromStdString(skeleton), requestId);
}

void MainWindow::onChatIngestResponse(const QString& responseText, quint64 requestId) {
    if (requestId != chatIngestRequestId_ ||
        chatIngestHash_ != app_->model.document.fileHash)
        return;
    if (responseText.isEmpty()) {
        ingestState_ = reader::IngestState::Failed;
        statusAi_->setText("ChatGPT ingest timed out or was interrupted.");
        updateIngestButton();
        return;
    }
    std::string error;
    auto parsed =
        reader::PaperAnalysis::parseLenient(responseText.toStdString(), error);
    if (!parsed || !parsed->usable()) {
        ingestState_ = reader::IngestState::Failed;
        statusAi_->setText(QString::fromStdString(
            "ChatGPT's answer wasn't usable JSON" + (error.empty() ? "." : ": " + error)));
        updateIngestButton();
        return;
    }
    parsed->meta.provider = "chatgpt-web";
    parsed->meta.model = "chatgpt.com";
    parsed->meta.generatedAt = reader::nowMs();
    if (!app_->ingestor.normalizeAnalysis(*parsed, app_->model, &error)) {
        ingestState_ = reader::IngestState::Failed;
        statusAi_->setText(QString::fromStdString(
            "ChatGPT ingest contained ungrounded paper sources" +
            (error.empty() ? "." : ": " + error)));
        updateIngestButton();
        return;
    }
    app_->ingestor.saveRawResponse(app_->model.document.fileHash,
                                   responseText.toStdString());
    if (!app_->ingestor.saveAnalysis(app_->model.document.fileHash, *parsed)) {
        ingestState_ = reader::IngestState::Failed;
        statusAi_->setText("ChatGPT ingest produced nothing usable.");
        updateIngestButton();
        return;
    }
    app_->analysis = *parsed;
    ingestState_ = reader::IngestState::Ingested;
    statusAi_->setText(QString("Ingested ✓ via ChatGPT · %1 passages, %2 concepts")
                           .arg(parsed->annotations.size())
                           .arg(parsed->concepts.size()));
    updateIngestButton();
    pdf_->refreshAiOverlays();
    map_->rebuild();
    summary_->rebuild();
    showIngestOutput();
    tabs_->setCurrentWidget(summary_);
}
#endif

void MainWindow::onIngestProgress(const reader::IngestProgress& p) {
    ingestState_ = p.state;
    updateIngestButton();
    if (!p.statusText.empty()) statusAi_->setText(QString::fromStdString(p.statusText));
}

void MainWindow::showIngestOutput() {
    if (!app_->analysis) {
        ingestRaw_->showEmpty();
        return;
    }
    const auto& a = *app_->analysis;
    QString meta = QString("provider=%1 model=%2 schema=%3 passages=%4 concepts=%5 sections=%6")
                       .arg(QString::fromStdString(a.meta.provider))
                       .arg(QString::fromStdString(a.meta.model))
                       .arg(a.meta.schemaVersion)
                       .arg(a.annotations.size())
                       .arg(a.concepts.size())
                       .arg(a.sections.size());
    std::string raw = app_->ingestor.loadRawResponse(app_->model.document.fileHash);
    if (!raw.empty()) ingestRaw_->showRawText(meta, QString::fromStdString(raw));
    else ingestRaw_->showJson(meta, QString::fromStdString(reader::json::pretty(a.toJson())));
}

void MainWindow::updateIngestButton() {    ingestButton_->setText(QString::fromStdString(reader::ingestStateLabel(ingestState_)));
    // Enabled once text exists and no ingest is running (§5.1, §5.2).
    bool busy = ingestState_ == reader::IngestState::Preparing ||
                ingestState_ == reader::IngestState::Uploading ||
                ingestState_ == reader::IngestState::Analyzing ||
                ingestState_ == reader::IngestState::Applying;
    ingestButton_->setEnabled(docReady_ && !busy);
}

void MainWindow::toggleAiPane() {
    const bool visible = !tabs_->isVisible();
    tabs_->setVisible(visible);
    app_->state.aiPaneVisible = visible;
    persistWindowLayout();
}

void MainWindow::toggleAiCollapse() {
    if (aiPaneDetached_) return;
    if (!app_->state.aiPaneCollapsed) {
        const QList<int> current = splitter_->sizes();
        expandedSplitterSizes_ = {current.value(0), current.value(1)};
        splitter_->setSizes(aiPaneLeft_ ? QList<int>{0, splitter_->width()}
                                        : QList<int>{splitter_->width(), 0});
        app_->state.aiPaneCollapsed = true;
    } else {
        if (expandedSplitterSizes_.size() == 2)
            splitter_->setSizes({expandedSplitterSizes_[0], expandedSplitterSizes_[1]});
        else
            splitter_->setSizes({splitter_->width() * 3 / 5, splitter_->width() * 2 / 5});
        app_->state.aiPaneCollapsed = false;
    }
    persistWindowLayout();
}

void MainWindow::toggleAiDetach() {
    if (aiPaneDetached_) {
        tabs_->setParent(splitter_);
        tabs_->setWindowFlag(Qt::Window, false);
        if (aiPaneLeft_) splitter_->insertWidget(0, tabs_);
        else splitter_->addWidget(tabs_);
        tabs_->show();
        aiPaneDetached_ = false;
        app_->state.aiPaneVisible = true;
    } else {
        tabs_->setParent(nullptr);
        tabs_->setWindowFlag(Qt::Window, true);
        tabs_->setWindowTitle("Paper Reader AI");
        tabs_->resize(520, 720);
        tabs_->show();
        aiPaneDetached_ = true;
        app_->state.aiPaneVisible = true;
    }
    persistWindowLayout();
}

void MainWindow::persistWindowLayout() {
    if (!splitter_ || !tabs_) return;
    QSettings settings;
    settings.setValue("reader/aiPaneVisible", tabs_->isVisible());
    settings.setValue("reader/aiPaneCollapsed", app_->state.aiPaneCollapsed);
    settings.setValue("reader/aiPaneLeft", aiPaneLeft_);
    settings.setValue("reader/aiPaneDetached", aiPaneDetached_);
    settings.setValue("reader/toolbarPinned", toolbarPinned_);
    const QList<int> sizes = splitter_->sizes();
    if (sizes.size() == 2)
        settings.setValue("reader/splitterSizes",
                          QByteArray::number(sizes[0]) + "," + QByteArray::number(sizes[1]));
    settings.setValue("reader/windowGeometry", saveGeometry());
}

void MainWindow::setupToolbarAutoHide() {
    if (!toolbar_ || toolbarRevealTimer_) return;
    toolbarRevealTimer_ = new QTimer(this);
    toolbarRevealTimer_->setInterval(120);
    connect(toolbarRevealTimer_, &QTimer::timeout, this, &MainWindow::updateToolbarAutoHide);
    toolbarRevealTimer_->start();
    updateToolbarAutoHide();
}

void MainWindow::updateToolbarAutoHide() {
    if (!toolbar_) return;
    if (toolbarPinned_) {
        if (!toolbar_->isVisible()) toolbar_->show();
        return;
    }
    if (ingestMenu_ && ingestMenu_->isVisible()) return;
    const QPoint cursor = QCursor::pos();
    const QPoint local = mapFromGlobal(cursor);
    if (!toolbar_->isVisible()) {
        constexpr int kRevealHeight = 6;
        if (local.y() >= 0 && local.y() <= kRevealHeight && local.x() >= 0 &&
            local.x() < width()) {
            if (QWidget* at = QApplication::widgetAt(cursor);
                !at || window()->isAncestorOf(at) || at == window())
                toolbar_->show();
        }
        return;
    }
    if (toolbar_->underMouse()) return;
    if (QWidget* focus = QApplication::focusWidget();
        focus && toolbar_->isAncestorOf(focus))
        return;
    const int hideBelow = toolbar_->height() + 12;
    if (local.y() > hideBelow || local.x() < 0 || local.x() >= width() || local.y() < 0)
        toolbar_->hide();
}

void MainWindow::setToolbarPinned(bool pinned) {
    toolbarPinned_ = pinned;
    QSettings settings;
    settings.setValue("reader/toolbarPinned", pinned);
    updateToolbarAutoHide();
    if (pinned && toolbar_ && !toolbar_->isVisible()) toolbar_->show();
    persistWindowLayout();
}

void MainWindow::setupEdgeReveal() {
    if (edgeRevealTimer_) return;
    edgeRevealTimer_ = new QTimer(this);
    edgeRevealTimer_->setInterval(150);
    connect(edgeRevealTimer_, &QTimer::timeout, this, &MainWindow::updateEdgeReveal);
    edgeRevealTimer_->start();
}

void MainWindow::updateEdgeReveal() {
    if (!readerDock_) return;
    const QPoint cursor = QCursor::pos();
    const QPoint local = mapFromGlobal(cursor);
    constexpr int kEdgeWidth = 6;
    if (!readerDock_->isVisible()) {
        if (local.x() >= 0 && local.x() <= kEdgeWidth && local.y() >= 0 &&
            local.y() < height()) {
            if (QWidget* at = QApplication::widgetAt(cursor);
                !at || window()->isAncestorOf(at) || at == window()) {
                readerDock_->show();
                if (readerTabs_) {
                    for (int i = 0; i < readerTabs_->count(); ++i) {
                        if (readerTabs_->tabText(i) == "Outline") {
                            readerTabs_->setCurrentIndex(i);
                            break;
                        }
                    }
                }
                edgeRevealActive_ = true;
            }
        }
        return;
    }
    if (!edgeRevealActive_) return;
    if (readerDock_->underMouse()) return;
    if (QWidget* focus = QApplication::focusWidget();
        focus && readerDock_->isAncestorOf(focus))
        return;
    const int hideBeyond = readerDock_->width() + 40;
    if (local.x() > hideBeyond || local.x() < 0 || local.y() < 0 || local.y() >= height()) {
        readerDock_->hide();
        edgeRevealActive_ = false;
    }
}

void MainWindow::navigateTo(const reader::NavEntry& entry) {
    restoringHistory_ = true;
    pdf_->setZoom(entry.zoom);
    pdf_->goToPage(entry.page);
    pdf_->verticalScrollBar()->setValue(static_cast<int>(std::max(0.0, entry.scrollY)));
    if (entry.selection) pdf_->jumpToAnchor(*entry.selection, true);
    restoringHistory_ = false;
}

void MainWindow::navigateToAnchor(const reader::DocumentAnchor& anchor) {
    if (!pdf_) return;
    const auto state = pdf_->captureState();
    app_->state.history.visit({state.page, static_cast<double>(state.scrollY), state.zoom,
                               state.selection});
    pdf_->jumpToAnchor(anchor, true);
}

void MainWindow::openReaderTools() {
    if (!readerDock_) return;
    edgeRevealActive_ = false;
    readerDock_->setVisible(!readerDock_->isVisible());
    if (readerDock_->isVisible() && searchPanel_) searchPanel_->focusQuery();
}

void MainWindow::openLibrary() {
    if (!app_ || !app_->documents) return;
    QDialog dialog(this);
    dialog.setWindowTitle("Recent library");
    dialog.resize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    list->setObjectName("recentLibraryList");
    for (const auto& document : app_->documents->recentDocuments(30)) {
        const QString title = document.title.empty()
                                  ? QFileInfo(QString::fromStdString(document.filePath)).fileName()
                                  : QString::fromStdString(document.title);
        auto* item = new QListWidgetItem(
            QString("%1\n%2").arg(title, QString::fromStdString(document.filePath)), list);
        item->setData(Qt::UserRole, QString::fromStdString(document.filePath));
    }
    auto* open = new QPushButton("Open selected", &dialog);
    open->setObjectName("openRecentButton");
    layout->addWidget(list);
    layout->addWidget(open);
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog](QListWidgetItem*) { dialog.accept(); });
    connect(open, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return;
    const QString path = list->currentItem()->data(Qt::UserRole).toString();
    if (!path.isEmpty() && QFileInfo::exists(path)) openFile(path);
}

void MainWindow::openAnnotationManager() {
    if (!app_ || !app_->annotations || app_->model.document.id.empty()) return;
    const auto document = app_->model.document.id;
    QDialog dialog(this);
    dialog.setWindowTitle("Annotations and notes");
    dialog.resize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    list->setObjectName("annotationList");
    const auto annotations = app_->annotations->annotationsFor(document);
    const auto notes = app_->annotations->notesFor(document);
    for (const auto& annotation : annotations) {
        auto* item = new QListWidgetItem(
            QString("%1 · page %2 · %3")
                .arg(QString::fromStdString(annotation.kind))
                .arg(annotation.anchor.page + 1)
                .arg(QString::fromStdString(annotation.anchor.anchorText).left(100)),
            list);
        item->setData(Qt::UserRole, "annotation");
        item->setData(Qt::UserRole + 1, QString::fromStdString(annotation.id));
    }
    for (const auto& note : notes) {
        auto* item = new QListWidgetItem(
            QString("note · page %1 · %2")
                .arg(note.anchor.page + 1)
                .arg(QString::fromStdString(note.text).left(100)),
            list);
        item->setData(Qt::UserRole, "note");
        item->setData(Qt::UserRole + 1, QString::fromStdString(note.id));
    }
    auto* edit = new QPushButton("Edit", &dialog);
    auto* remove = new QPushButton("Delete", &dialog);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(edit);
    buttons->addWidget(remove);
    layout->addWidget(list);
    layout->addLayout(buttons);
    connect(edit, &QPushButton::clicked, &dialog, [this, list, document] {
        auto* item = list->currentItem();
        if (!item) return;
        const std::string id = item->data(Qt::UserRole + 1).toString().toStdString();
        if (item->data(Qt::UserRole).toString() == "note") {
            for (auto note : app_->annotations->notesFor(document)) {
                if (note.id != id) continue;
                bool ok = false;
                const QString text = QInputDialog::getMultiLineText(
                    this, "Edit note", "Note text:", QString::fromStdString(note.text), &ok);
                if (ok && !text.trimmed().isEmpty()) {
                    note.text = text.toStdString();
                    note.updatedAt = reader::nowMs();
                    app_->annotations->saveNote(document, note);
                    pdf_->refreshUserOverlays();
                }
                break;
            }
        } else {
            for (auto annotation : app_->annotations->annotationsFor(document)) {
                if (annotation.id != id) continue;
                QStringList styles{"highlight", "underline", "strikethrough", "bookmark", "region"};
                bool ok = false;
                const QString kind = QInputDialog::getItem(
                    this, "Edit annotation", "Style:", styles,
                    styles.indexOf(QString::fromStdString(annotation.kind)), false, &ok);
                if (ok) {
                    annotation.kind = kind.toStdString();
                    app_->annotations->saveAnnotation(document, annotation);
                    pdf_->refreshUserOverlays();
                }
                break;
            }
        }
    });
    connect(remove, &QPushButton::clicked, &dialog, [this, list, document] {
        auto* item = list->currentItem();
        if (!item) return;
        const std::string id = item->data(Qt::UserRole + 1).toString().toStdString();
        if (item->data(Qt::UserRole).toString() == "note")
            app_->annotations->deleteNote(document, id);
        else
            app_->annotations->deleteAnnotation(document, id);
        delete list->takeItem(list->row(item));
        pdf_->refreshUserOverlays();
    });
    dialog.exec();
}

void MainWindow::openSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle("Reader settings");
    auto* layout = new QFormLayout(&dialog);
    auto* local = new QCheckBox("Parse and index locally (required)", &dialog);
    auto* embeddings = new QCheckBox("Store local embeddings", &dialog);
    auto* retrieved = new QCheckBox("Send only retrieved passages", &dialog);
    auto* complete = new QCheckBox("Allow complete paper upload", &dialog);
    auto* copy = new QCheckBox("Copy selections to clipboard on explicit copy", &dialog);
    auto* intensity = new QComboBox(&dialog);
    intensity->addItems({"off", "minimal", "normal", "extensive"});
    intensity->setCurrentText(QString::fromStdString(app_->state.settings.aiEmphasis));
    auto* important = new QCheckBox("Show important passages", &dialog);
    auto* definitions = new QCheckBox("Show definitions", &dialog);
    auto* results = new QCheckBox("Show results", &dialog);
    auto* limitations = new QCheckBox("Show limitations", &dialog);
    auto* methods = new QCheckBox("Show methods", &dialog);
    important->setChecked(app_->state.settings.showImportant);
    definitions->setChecked(app_->state.settings.showDefinitions);
    results->setChecked(app_->state.settings.showResults);
    limitations->setChecked(app_->state.settings.showLimitations);
    methods->setChecked(app_->state.settings.showMethods);
    local->setChecked(true);
    local->setEnabled(false);
    local->setToolTip("PDF parsing and the searchable text index stay on this device.");
    embeddings->setChecked(app_->state.settings.storeEmbeddingsLocally);
    retrieved->setChecked(app_->state.settings.sendOnlyRetrievedPassages);
    complete->setChecked(app_->state.settings.allowCompleteUpload);
    copy->setChecked(app_->state.settings.copySelectionToClipboard);
    layout->addRow(local);
    layout->addRow(embeddings);
    layout->addRow(retrieved);
    layout->addRow(complete);
    layout->addRow(copy);
    layout->addRow("AI emphasis", intensity);
    layout->addRow(important);
    layout->addRow(definitions);
    layout->addRow(results);
    layout->addRow(limitations);
    layout->addRow(methods);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    app_->state.settings.parseLocally = true;
    app_->state.settings.storeEmbeddingsLocally = embeddings->isChecked();
    app_->state.settings.sendOnlyRetrievedPassages = retrieved->isChecked();
    app_->state.settings.allowCompleteUpload = complete->isChecked();
    app_->state.settings.copySelectionToClipboard = copy->isChecked();
    app_->state.settings.aiEmphasis = intensity->currentText().toStdString();
    app_->state.settings.showImportant = important->isChecked();
    app_->state.settings.showDefinitions = definitions->isChecked();
    app_->state.settings.showResults = results->isChecked();
    app_->state.settings.showLimitations = limitations->isChecked();
    app_->state.settings.showMethods = methods->isChecked();
    pdf_->refreshAiOverlays();
    QSettings settings;
    settings.setValue("reader/parseLocally", true);
    settings.setValue("reader/storeEmbeddingsLocally", embeddings->isChecked());
    settings.setValue("reader/sendOnlyRetrievedPassages", retrieved->isChecked());
    settings.setValue("reader/allowCompleteUpload", complete->isChecked());
    settings.setValue("reader/copySelectionToClipboard", copy->isChecked());
    settings.setValue("reader/aiEmphasis", intensity->currentText());
    settings.setValue("reader/showImportant", important->isChecked());
    settings.setValue("reader/showDefinitions", definitions->isChecked());
    settings.setValue("reader/showResults", results->isChecked());
    settings.setValue("reader/showLimitations", limitations->isChecked());
    settings.setValue("reader/showMethods", methods->isChecked());
}

void MainWindow::executeCommand(const QString& command) {
    const QString normalized = command.trimmed().toLower();
    if (normalized == "toggle ai pane") return toggleAiPane();
    if (normalized == "pin toolbar") return setToolbarPinned(true);
    if (normalized == "unpin toolbar") return setToolbarPinned(false);
    if (normalized == "toggle toolbar") return setToolbarPinned(!toolbarPinned_);
    if (normalized == "toggle concept map") {
        tabs_->setCurrentWidget(map_);
        return;
    }
    if (normalized == "new chat") {
        tabs_->setCurrentWidget(chat_);
        QMetaObject::invokeMethod(chat_, "newConversation", Qt::DirectConnection);
        return;
    }
    if (normalized == "re-ingest paper") return runIngest(true);
    if (normalized == "pin current selection") {
        const auto context = app_->context.currentContext();
        if (context.temporary.empty()) {
            statusAi_->setText("Select a passage or paper object before pinning.");
            return;
        }
        app_->context.pinReference(context.temporary.front().id);
        chat_->refreshContextChips();
        statusAi_->setText("Reference pinned for comparison.");
        return;
    }
    if (normalized == "go to methods") {
        for (const auto& section : app_->model.sections)
            if (reader::toLower(section.title).find("method") != std::string::npos) {
                navigateToAnchor(reader::anchorForSection(app_->model, section));
                return;
            }
    }
    if (normalized == "explain current section") {
        const reader::Section* section = nullptr;
        if (app_->state.readerState.section)
            section = app_->model.findSection(*app_->state.readerState.section);
        if (!section) section = app_->model.sectionForPage(app_->state.page);
        if (!section) {
            statusAi_->setText("No section is available at the current page.");
            return;
        }
        reader::ContextReference reference;
        reference.type = reader::ReferenceType::Section;
        reference.anchor = reader::anchorForSection(app_->model, *section);
        reference.displayName = section->title;
        reference.extractedText = app_->model.sectionText(section->id);
        tabs_->setCurrentWidget(chat_);
        chat_->prepareQuestion(
            QString("Explain the current section, \"%1\", including its purpose and assumptions.")
                .arg(QString::fromStdString(section->title)),
            std::move(reference));
        return;
    }
    if (normalized == "summarize current page") {
        reader::ContextReference reference;
        reference.type = reader::ReferenceType::Page;
        reference.anchor.document = app_->model.document.id;
        reference.anchor.page = app_->state.page;
        reference.displayName = "Page " + std::to_string(app_->state.page + 1);
        bool haveBounds = false;
        QRectF pageBounds;
        for (const auto& block : app_->model.blocks) {
            if (block.page != app_->state.page) continue;
            reference.extractedText += block.text + "\n";
            const QRectF bounds(block.bounds.x, block.bounds.y, block.bounds.width,
                                block.bounds.height);
            pageBounds = haveBounds ? pageBounds.united(bounds) : bounds;
            haveBounds = true;
        }
        if (haveBounds)
            reference.anchor.bounds = {static_cast<float>(pageBounds.x()),
                                       static_cast<float>(pageBounds.y()),
                                       static_cast<float>(pageBounds.width()),
                                       static_cast<float>(pageBounds.height())};
        reference.anchor.anchorText = reference.extractedText.substr(0, 500);
        tabs_->setCurrentWidget(chat_);
        chat_->prepareQuestion(
            QString("Summarize page %1 and identify its key claim, evidence, and caveats.")
                .arg(app_->state.page + 1),
            std::move(reference));
        return;
    }
    for (const auto& section : app_->model.sections) {
        if (normalized == ("go to " + reader::toLower(section.title))) {
            navigateToAnchor(reader::anchorForSection(app_->model, section));
            return;
        }
    }
}

void MainWindow::setupShortcuts() {
    auto add = [this](const QKeySequence& key, auto fn) {
        auto* sc = new QShortcut(key, this);
        connect(sc, &QShortcut::activated, this, fn);
    };
    add(QKeySequence("Ctrl+Shift+A"), [this] { toggleAiPane(); });
    add(QKeySequence("Ctrl+Shift+T"), [this] { setToolbarPinned(!toolbarPinned_); });
    add(QKeySequence("Ctrl+K"), [this] {
        CommandPalette palette(app_, this);
        connect(&palette, &CommandPalette::commandChosen, this,
                &MainWindow::executeCommand);
        palette.exec();
    });
    add(QKeySequence("Ctrl+1"), [this] { tabs_->setCurrentIndex(0); });
    add(QKeySequence("Ctrl+2"), [this] { tabs_->setCurrentIndex(1); });
    add(QKeySequence("Ctrl+3"), [this] { tabs_->setCurrentIndex(2); });
    add(QKeySequence("Ctrl+4"), [this] {
        if (tabs_->count() > 3) tabs_->setCurrentIndex(3);
    });
    add(QKeySequence("Ctrl+5"), [this] {
        if (tabs_->count() > 4) tabs_->setCurrentIndex(4);
    });
    add(QKeySequence("Alt+Left"), [this] {
        auto e = app_->state.history.back();
        navigateTo(e);
    });
    add(QKeySequence("Alt+Right"), [this] {
        auto e = app_->state.history.forward();
        navigateTo(e);
    });
}
