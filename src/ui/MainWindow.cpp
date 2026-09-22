#include "ui/MainWindow.h"
#include "analysis/StructureDetector.h"
#include "app/Application.h"
#include "core/Types.h"
#include "pdf/HighlightExport.h"
#include "pdf/PopplerBridge.h"
#include "pdf/QtPdfEngine.h"
#include "pdf/TextExtractor.h"
#include "ui/OutlinePanel.h"
#include "ui/PdfView.h"
#include "ui/SearchPanel.h"
#include "ui/WebPanel.h"
#include <QFileDialog>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QDir>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QMetaObject>
#include <QPdfDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

MainWindow::MainWindow(reader::Application* app, QWidget* parent)
    : QMainWindow(parent), app_(app) {
    QSettings settings;
    // Local parsing is the privacy boundary and a prerequisite for
    // selection/search. It is an invariant, not a toggle.
    app_->state.settings.parseLocally = true;
    app_->state.settings.copySelectionToClipboard =
        settings.value("reader/copySelectionToClipboard", false).toBool();
    app_->state.aiPaneVisible = settings.value("reader/aiPaneVisible", true).toBool();
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
        QSettings settings;
        const QString startDir =
            settings.value("reader/lastOpenDir",
                           QStandardPaths::writableLocation(
                               QStandardPaths::DocumentsLocation))
                .toString();
        QString path =
            QFileDialog::getOpenFileName(this, "Open paper", startDir, "PDF (*.pdf)");
        if (path.isEmpty()) return;
        settings.setValue("reader/lastOpenDir", QFileInfo(path).absolutePath());
        openFile(path);
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
    addAction("Toggle AI", [this] { toggleAiPane(); });
    addAction("Reader tools", [this] { openReaderTools(); });
    QAction* saveAct = addAction("Save", [this] { saveHighlightsToPdf(); });
    saveAct->setToolTip("Save highlights into this PDF (Ctrl+S)");
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
    web_ = new WebPanel(app_, splitter_);
    web_->setObjectName("browserChat");
    splitter_->addWidget(pdf_);
    splitter_->addWidget(web_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    setCentralWidget(splitter_);
    if (!app_->state.aiPaneVisible) web_->hide();

    // Reader-owned navigation/search tools stay separate from the chat and
    // remain usable while the browser works. The overlay hovers over the
    // document without pushing it.
    readerOverlay_ = new QFrame(this);
    readerOverlay_->setObjectName("readerToolsOverlay");
    readerOverlay_->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    readerOverlay_->setLineWidth(1);
    readerOverlay_->hide();
    auto* overlayLayout = new QVBoxLayout(readerOverlay_);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    readerTabs_ = new QTabWidget(readerOverlay_);
    searchPanel_ = new SearchPanel(app_, pdf_, readerTabs_);
    outlinePanel_ = new OutlinePanel(app_, readerTabs_);
    readerTabs_->addTab(searchPanel_, "Search");
    readerTabs_->addTab(outlinePanel_, "Outline");
    overlayLayout->addWidget(readerTabs_);
    // Hidden by default: the left edge hover reveals it, moving away
    // hides it again. The Outline tab is pre-selected for reveals.
    selectReaderTab("Outline");

    statusPage_ = new QLabel("No document", this);
    statusHint_ = new QLabel("Ready", this);
    statusBar()->addWidget(statusPage_, 1);
    statusBar()->addWidget(statusHint_);

    connect(pdf_, &PdfView::selectionChanged, this, [this](const reader::DocumentAnchor& anchor) {
        if (web_) web_->refreshContext();
        if (app_->state.settings.copySelectionToClipboard && !anchor.anchorText.empty())
            QGuiApplication::clipboard()->setText(QString::fromStdString(anchor.anchorText));
        showSelectionHint();
    });
    connect(pdf_, &PdfView::pageChanged, this, [this](int page) {
        app_->state.page = page;
        app_->state.readerState.page = page;
        if (const reader::Section* s = app_->model.sectionForPage(page))
            app_->state.readerState.section = s->id;
        if (const reader::TextBlock* b = app_->model.blockAtPage(page, 400))
            app_->state.readerState.paragraphBlock = b->id;
        pdf_->schedulePrefetchAround(page);
        if (web_) web_->refreshContext();
        statusPage_->setText(
            QString("p.%1 / %2").arg(page + 1).arg(app_->model.document.pageCount));
        showSelectionHint();
        if (outlinePanel_) outlinePanel_->followPage(page);
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
                // Clicking a figure/equation/section makes it the live
                // chat context; the browser prompt picks it up as text.
                if (auto built = reader::contextReferenceForObject(app_->model, a)) {
                    app_->context.setCurrentSelection(std::move(*built));
                    if (web_) web_->refreshContext();
                    showSelectionHint();
                }
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
            [this](const reader::DocumentAnchor& a, const QImage&) {
                // Persist the region as an annotation; no chat image payload.
                if (app_->annotations && !app_->model.document.id.empty()) {
                    reader::UserAnnotation region;
                    region.id =
                        QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                    region.anchor = a;
                    region.kind = "region";
                    region.color = "#4f8cff";
                    app_->annotations->saveAnnotation(app_->model.document.id, region);
                    pdf_->refreshUserOverlays();
                }
            });
    connect(searchPanel_, &SearchPanel::anchorActivated, this,
            [this](const reader::DocumentAnchor& a) {
                navigateToAnchor(a);
                edgeRevealActive_ = false;
                readerOverlay_->hide();
            });
    connect(outlinePanel_, &OutlinePanel::anchorActivated, this,
            [this](const reader::DocumentAnchor& a) { navigateToAnchor(a); });
    connect(pdf_, &PdfView::quickAskRequested, this,
            [this](const QString& seed) { focusBrowserQuestionWithSeed(seed); });
    connect(pdf_, &PdfView::findRequested, this, [this] {
        showReaderTools();
        edgeRevealActive_ = false;
        if (searchPanel_) searchPanel_->focusQuery();
    });
    connect(pdf_, &PdfView::sidecarToggleRequested, this, &MainWindow::toggleAiPane);
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
    // Sane initial divider: 60% reader, 40% chat, with a usable minimum so
    // the PDF pane can never collapse into an unreadable sliver.
    pdf_->setMinimumWidth(320);
    QTimer::singleShot(0, this, [this] {
        int w = width() > 0 ? width() : 1280;
        const auto saved = QSettings().value("reader/splitterSizes").value<QByteArray>();
        if (!saved.isEmpty()) {
            QList<int> sizes;
            for (const auto& value : saved.split(',')) sizes.push_back(value.toInt());
            // A zero-width pane is never intentional (dragged past the edge
            // or a stale layout): fall back to the default split instead of
            // stranding the chat invisible. An explicit hide via Toggle AI
            // is still honored through aiPaneVisible.
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
    });
}

MainWindow::~MainWindow() {
    persistWindowLayout();
    saveReadingState();
    documentToken_.cancel();
    if (web_) web_->cancelPending();
    if (app_) app_->shutdown();
}

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
    // Stage 1: page 1 visible immediately; never wait for analysis.
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
        if (web_) web_->refreshContext();
    }
    documentToken_.cancel();
    documentToken_ = reader::CancellationToken{};
    const unsigned long generation = ++openGeneration_;
    if (web_) web_->cancelPending();
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
    outlinePanel_->rebuild();
    app_->state.page = 0;
    app_->state.scrollY = 0;
    app_->state.history.clear();
    app_->state.history.visit({0, 0, app_->state.zoom, std::nullopt});

    statusPage_->setText(QString("p.%1 / %2 · identifying…")
                             .arg(app_->state.page + 1)
                             .arg(model.document.pageCount));

    // Stages 2-3 on background lanes: extract -> structure. Text is cached
    // by document hash so reopening is instant and stays offline.
    // Threading: the worker builds a LOCAL model; publishing to app_->model
    // happens in one queued UI-thread step. The UI never reads a model
    // that a worker is still mutating.
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
                    // Serial doc lane: the PDF engine is used from one
                    // background thread at a time.
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
                            QMetaObject::invokeMethod(
                                this,
                                [this, local = std::move(local), loadedModel,
                                 generation]() mutable {
                                    if (generation != openGeneration_) return;
                                    app_->model = std::move(local);
                                    pdf_->distributeLines();
                                    pdf_->schedulePrefetchAround(app_->state.page);
                                    outlinePanel_->rebuild();
                                    // In-place save reopened the same file: jump back to
                                    // where the reader was instead of page 1.
                                    if (pendingPosition_) {
                                        restoringHistory_ = true;
                                        pdf_->setZoom(pendingPosition_->zoom);
                                        pdf_->goToPage(pendingPosition_->page);
                                        pdf_->verticalScrollBar()->setValue(static_cast<int>(
                                            std::max(0.0, pendingPosition_->scrollY)));
                                        restoringHistory_ = false;
                                        pendingPosition_.reset();
                                    }
                                    statusPage_->setText(
                                        QString("p.%1 / %2 · %3 blocks")
                                            .arg(app_->state.page + 1)
                                            .arg(app_->model.document.pageCount)
                                            .arg(app_->model.blocks.size()));
                                    statusHint_->setText(
                                        loadedModel ? "Loaded cached text"
                                                    : "Text ready — select a passage to ask");
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

void MainWindow::toggleAiPane() {
    const bool visible = !web_->isVisible();
    web_->setVisible(visible);
    app_->state.aiPaneVisible = visible;
    persistWindowLayout();
}

void MainWindow::persistWindowLayout() {
    if (!splitter_ || !web_) return;
    QSettings settings;
    settings.setValue("reader/aiPaneVisible", web_->isVisible());
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
    toolbarRevealTimer_->setInterval(250);
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
    edgeRevealTimer_->setInterval(300);
    connect(edgeRevealTimer_, &QTimer::timeout, this, &MainWindow::updateEdgeReveal);
    edgeRevealTimer_->start();
}

void MainWindow::selectReaderTab(const QString& name) {
    if (!readerTabs_) return;
    for (int i = 0; i < readerTabs_->count(); ++i)
        if (readerTabs_->tabText(i) == name) {
            readerTabs_->setCurrentIndex(i);
            return;
        }
}

// The status bar is the shortcut cheat-sheet: it always reflects what the
// live selection accepts next.
void MainWindow::showSelectionHint() {
    if (!statusHint_) return;
    if (pdf_ && pdf_->hasHighlightForCurrentSelection())
        statusHint_->setText("highlighted · h removes · space asks in chat");
    else if (pdf_ && pdf_->hasLiveSelection())
        statusHint_->setText("selection → chat context · h highlight · n note · a ask");
    else
        statusHint_->setText("select text to ask · Ctrl+F search · Alt+Left back");
}

void MainWindow::updateEdgeReveal() {
    if (!readerOverlay_) return;
    const QPoint cursor = QCursor::pos();
    const QPoint local = mapFromGlobal(cursor);
    constexpr int kEdgeWidth = 6;
    if (!readerOverlay_->isVisible()) {
        if (local.x() >= 0 && local.x() <= kEdgeWidth && local.y() >= 0 &&
            local.y() < height()) {
            if (QWidget* at = QApplication::widgetAt(cursor);
                !at || window()->isAncestorOf(at) || at == window()) {
                showReaderTools();
                selectReaderTab("Outline");
                edgeRevealActive_ = true;
            }
        }
        return;
    }
    if (!edgeRevealActive_) return;
    if (readerOverlay_->underMouse()) return;
    if (QWidget* focus = QApplication::focusWidget();
        focus && readerOverlay_->isAncestorOf(focus))
        return;
    const int hideBeyond = readerOverlay_->width() + 40;
    if (local.x() > hideBeyond || local.x() < 0 || local.y() < 0 || local.y() >= height()) {
        readerOverlay_->hide();
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
    if (!readerOverlay_) return;
    edgeRevealActive_ = false;
    if (readerOverlay_->isVisible()) {
        readerOverlay_->hide();
        return;
    }
    showReaderTools();
    if (searchPanel_) searchPanel_->focusQuery();
}

void MainWindow::showReaderTools() {
    if (!readerOverlay_) return;
    placeReaderOverlay();
    readerOverlay_->raise();
    if (!readerOverlay_->isVisible()) readerOverlay_->show();
}

void MainWindow::placeReaderOverlay() {
    if (!readerOverlay_) return;
    constexpr int kOverlayWidth = 320;
    const int top = toolbar_ && toolbar_->isVisible() ? toolbar_->y() + toolbar_->height() : 0;
    const int bottom = height() - (statusBar() ? statusBar()->height() : 0);
    readerOverlay_->setGeometry(0, top, kOverlayWidth, std::max(300, bottom - top));
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (readerOverlay_ && readerOverlay_->isVisible()) placeReaderOverlay();
}

void MainWindow::setupShortcuts() {
    auto add = [this](const QKeySequence& key, auto fn) {
        auto* sc = new QShortcut(key, this);
        connect(sc, &QShortcut::activated, this, fn);
    };
    add(QKeySequence("Ctrl+Shift+A"), [this] { toggleAiPane(); });
    add(QKeySequence("Alt+Left"), [this] {
        auto e = app_->state.history.back();
        navigateTo(e);
    });
    add(QKeySequence("Alt+Right"), [this] {
        auto e = app_->state.history.forward();
        navigateTo(e);
    });
    add(QKeySequence("Ctrl+S"), [this] { saveHighlightsToPdf(); });
    // h/n/a act on the live PDF selection from anywhere in the window.
    // PdfView rarely owns keyboard focus (mouse selection does not move
    // it), so window shortcuts — not viewport keys — own them. Typing
    // inside any text input, the browser chat, or a modal dialog is never
    // hijacked.
    auto typingElsewhere = [this] {
        if (QApplication::activeModalWidget()) return true;
        if (chatHasFocus()) return true;
        if (QWidget* focus = QApplication::focusWidget();
            focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) ||
                      qobject_cast<QPlainTextEdit*>(focus)))
            return true;
        return false;
    };
    auto* highlightShortcut = new QShortcut(QKeySequence(Qt::Key_H), this);
    connect(highlightShortcut, &QShortcut::activated, this, [this, typingElsewhere] {
        if (typingElsewhere() || !pdf_) return;
        toggleHighlight();
    });
    auto* highlightGlobal = new QShortcut(QKeySequence("Ctrl+H"), this);
    connect(highlightGlobal, &QShortcut::activated, this, [this] {
        if (QApplication::activeModalWidget() || chatHasFocus() || !pdf_) return;
        toggleHighlight();
    });
    auto* noteShortcut = new QShortcut(QKeySequence(Qt::Key_N), this);
    connect(noteShortcut, &QShortcut::activated, this, [this, typingElsewhere] {
        if (typingElsewhere() || !pdf_) return;
        pdf_->promptNoteForCurrentSelection();
    });
    auto* noteGlobal = new QShortcut(QKeySequence("Ctrl+N"), this);
    connect(noteGlobal, &QShortcut::activated, this, [this] {
        if (QApplication::activeModalWidget() || chatHasFocus() || !pdf_) return;
        pdf_->promptNoteForCurrentSelection();
    });
    auto* askShortcut = new QShortcut(QKeySequence(Qt::Key_A), this);
    connect(askShortcut, &QShortcut::activated, this, [this, typingElsewhere] {
        if (typingElsewhere() || !pdf_) return;
        pdf_->askAboutCurrentSelection();
    });
    QApplication::instance()->installEventFilter(this);
}

void MainWindow::focusBrowserQuestion() {
    if (web_) {
        if (!web_->isVisible()) toggleAiPane();
        web_->focusQuestion();
    }
}

void MainWindow::focusBrowserQuestionWithSeed(const QString& seed) {
    if (web_) {
        if (!web_->isVisible()) toggleAiPane();
        web_->focusQuestionWithSeed(seed);
    }
}

void MainWindow::toggleHighlight() {
    if (!pdf_) return;
    if (pdf_->hasHighlightForCurrentSelection())
        pdf_->removeHighlightForCurrentSelection();
    else
        pdf_->highlightCurrentSelection();
    showSelectionHint();
}

bool MainWindow::chatHasFocus() const {
    const QWidget* focus = QApplication::focusWidget();
    return focus && web_ && web_->isAncestorOf(focus);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Space starts typing in the ask box from anywhere: highlighting and
    // selecting never steal focus, so space is the explicit "take me to
    // the composer" key. Text inputs, the browser chat, and modal dialogs
    // keep their space.
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<const QKeyEvent*>(event);
        const bool modified =
            (key->modifiers() &
             (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) != Qt::NoModifier;
        if (!modified && key->key() == Qt::Key_Space) {
            if (QApplication::activeModalWidget()) return false;
            if (chatHasFocus()) return false;
            if (QWidget* focus = QApplication::focusWidget();
                focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) ||
                          qobject_cast<QPlainTextEdit*>(focus)))
                return false;
            focusBrowserQuestion();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::saveHighlightsToPdf() {
    if (exportRunning_) return;
    const std::string docId = app_->model.document.id;
    const QString src = QString::fromStdString(app_->model.document.filePath);
    if (docId.empty() || src.isEmpty() || !QFile::exists(src)) {
        statusHint_->setText("Open a paper first.");
        return;
    }
    std::vector<reader::HighlightRow> rows;
    if (app_->annotations)
        for (const auto& ann : app_->annotations->annotationsFor(docId)) {
            if (ann.kind != "highlight" || !ann.anchor.bounds.valid()) continue;
            const auto& b = ann.anchor.bounds;
            rows.push_back({ann.anchor.page, QRectF(b.x, b.y, b.width, b.height)});
        }
    if (rows.empty()) {
        statusHint_->setText("No highlights to save — select text and press h first.");
        return;
    }
    if (QStandardPaths::findExecutable("qpdf").isEmpty() ||
        QStandardPaths::findExecutable("pdfinfo").isEmpty()) {
        statusHint_->setText("Saving needs qpdf + pdfinfo installed.");
        return;
    }
    // In-place save: merge to a temp sibling, verify, atomically replace
    // the original — no dialog. The file hash changes, so the paper is
    // reopened afterwards with the reading position restored.
    reader::HighlightExportJob job{src, src, pdf_->pageSizes(), std::move(rows)};
    if (job.pageSizes.empty()) {
        statusHint_->setText("No pages — open a paper first.");
        return;
    }
    const auto state = pdf_->captureState();
    exportRunning_ = true;
    statusHint_->setText("Saving highlights…");
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, src, state] {
                const QString error = watcher->result();
                watcher->deleteLater();
                exportRunning_ = false;
                if (error.isEmpty()) {
                    statusHint_->setText("Saved ✓");
                    pendingPosition_ = {state.page, static_cast<double>(state.scrollY),
                                        state.zoom, state.selection};
                    openFile(src);
                } else {
                    statusHint_->setText("Save failed: " + error);
                }
            });
    watcher->setFuture(QtConcurrent::run(reader::embedHighlights, job));
}
