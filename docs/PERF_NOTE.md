# Reader Optimization Note (ongoing)

Goal: efficient, optimal, minimally resource-intensive app with high-quality,
blazingly fast, seamless features. No visible lag.
Keep: document viewer, browser-chat integration (only AI chat), keyboard
shortcuts, notes, highlights, selection→chat, outlines, page fitting,
minimal distraction-free look, crisp fast rendering.
Remove: all other bloat.

`docs/spec-acceptance.md` is the pre-slim historical record; this note is
authoritative for the current minimal scope.

## 1. Feature inventory

### KEEP (load-bearing, cheap)
- PdfView: continuous/page modes, zoom, fit width/page, rotate, selection,
  links, history back/forward, literal search, notes/highlights/bookmarks
  overlays, reading-position persistence — `src/ui/PdfView.*`
- PdfRenderer tile/page cache + preview + prefetch — `src/pdf/PdfRenderer.*`
- PopplerBridge raster (Antialiasing+TextAntialiasing+Hinting, exact-DPI,
  pad/crop instead of blur rescale) — `src/pdf/PopplerBridge.*`
- QtPdfEngine text/selection/outline source; TextExtractor + StructureDetector
- WebPanel browser chat (selection-aware Ask/Copy-prompt, persistent profile,
  lazy first load) — `src/ui/WebPanel.*` — the ONLY AI chat
- OutlinePanel; literal-only SearchPanel
- Notes/highlights (AnnotationRepository + h/n/a/space + status-bar hints)
- Shortcuts: Ctrl+Shift+A, Alt+Left/Right, h, Ctrl+H, n, Ctrl+N, a, Ctrl+F,
  space→ask, j/k/g/G scroll — window-level, never hijack typing
- Fit width / fit page, zoom, rotate, page mode, fullscreen, goToPage/jumpToAnchor
- Reading state + navigation history (SQLite, debounced 150 ms save)
- Distraction-free: auto-hide toolbar (250 ms poll) + left-edge reader
  overlay (300 ms poll) with Search/Outline, no document push

### REMOVED (2026-09-22 slim-down; deleted files, not just unwired)
- ChatPanel + ChatTranscript + KaTeX bundle + chat_resources.qrc (native chat)
- MapPanel, SummaryPanel, IngestRawPanel, ThumbnailPanel, CommandPalette
- Ingest button/menu, whole-paper ingest plumbing, AI emphasis overlays,
  analysis-cache load on open, retrieval-index build on open
- Semantic UI: embedding settings/index buttons, semantic search mode,
  PdfView semantic methods + per-view VectorIndex + provider state
- WebPanel ingest-via-chat auto-send chain (ingestViaChat, attachPdf,
  confirmSend, startPolling, pollChatResponse, poll timer, ingest signal)
- Library / annotation-manager / settings dialogs; Move-AI-left, Collapse,
  Detach, extra toolbar actions; Ctrl+1..5, Ctrl+K shortcuts
- Tests for removed features: test_chat_ui, test_chat_rich (sources deleted);
  test_reader_ui rewritten around the kept set (below)
- Qt WebEngine is now REQUIRED for BUILD_UI (browser chat is the only AI
  chat); the native-chat fallback is gone

### Deliberately KEPT in core (no UI runtime cost, covered by passing tests)
- ChatManager/OpenAIProvider/EmbeddingProviderQt/VectorIndex/RetrievalEngine/
  PaperIngestor/PaperAnalysis stay in `reader_core` (thread-free until used;
  ChatManager ctor spawns nothing). Removing them is Phase C follow-up.
- PdfView::restoreState/outlineEntries: tiny stable APIs, no runtime cost.

## 2. Benchmarks (Debug, /tmp/reader-full3, offscreen x86_64)

Canonical runnable build: in-repo `build-qt/` (Release), launched with
plain `./run.sh [paper.pdf]`. Verified 2026-09-22: fresh configure+build,
9/9 ctest pass there, offscreen launch stays in a healthy event loop.

| Metric | Before | After | Δ |
|---|---|---|---|
| paper-reader binary | 37,918,568 B | 26,458,536 B | **−30%** |
| paper-reader binary (Release, `build-qt`) | 3,116,456 B | 1,935,832 B | **−38%** |
| test_reader_ui binary | 37,028,896 B | 26,860,200 B | −27% |
| test_select binary | 21,555,744 B | 20,150,440 B | −6% |
| UI sources | 26 files + KaTeX (936K) | 10 files | −16 files, −936K bundle |
| MainWindow.cpp | 1587 lines | ~560 lines | −65% |
| PdfView.cpp | 1513 lines | ~1050 lines | −31% |
| ctest (9 tests) | 5.29 s* | 7.98 s (webengine 4.79 s) | reader_ui 2.82 s → **0.84 s** |
| reader_ui timings | first_paint — | first_paint=110 ms, selection_context=1 ms, literal_search=0 ms | warm offscreen |
| render_stress | 0.26 s / 119 MB RSS | 0.28 s / ~120 MB RSS | noise (tiny fixture) |
| test_core | 0.07 s / 7 MB RSS | unchanged | — |
| Qt-free core build | 2/2 pass | 2/2 pass (0.09 s) | — |

\* Baseline excluded webengine+chat_rich (8 tests in the first baseline run;
9 after chat_rich existed). Current suite is 9 tests, all passing.
- Tile cache worst case: 256×512²×4 B ≈ 256 MB → 64×512²×4 B ≈ **64 MB**.
  Page cache: 16 → 6 full pages (~3 MB each at default zoom).
- Thread lanes: 2+2+2+2+2+1 = 11 → **1+1+1+1+1+1 = 6** (ChatManager spawns none).
- Always-on timers: toolbar 120 ms → 250 ms, edge reveal 150 ms → 300 ms
  (~2× fewer wakeups); WebPanel chatgpt.com load deferred to first show.
- Rendering path untouched (Poppler exact-DPI + pad/crop): crispness claims
  in the old record still hold; render + render_stress tests pass unmodified.

## 3. Changes made (2026-09-22, all verified by build + ctest)

1. Phase A: renderer caches 256→64 tiles / 16→6 pages (`PdfRenderer.h/.cpp`);
   pools →1 thread each (`Application.h`); toolbar/edge timers →250/300 ms;
   WebPanel lazy first load on showEvent + ensureLoaded (`WebPanel.*`).
2. SearchPanel rewritten literal-only (mode combo, embedding buttons,
   semantic branch deleted).
3. MainWindow rewritten (~560 lines): splitter holds PdfView + WebPanel
   directly; minimal toolbar (Open/Back/Forward/Fit width/Fit page/Rotate/
   Page mode/Fullscreen/Toggle AI/Reader tools/Pin); overlay keeps
   Search+Outline; openFile pipeline is hash→extract→structure→publish
   (no ingest/analysis/semantic/retrieval/thumbnail/chat wiring).
4. PdfView strip: semantic methods, VectorIndex/provider state, thumbnail +
   anchor-image crops, AI overlay pipeline + paint, Ctrl+K/Ctrl+1..3 key
   emissions removed. Kept: selection, notes/highlights, literal search,
   fit/zoom/rotate/page-mode, history, links, object clicks→context.
5. WebPanel: removed ingest-via-chat chain; fixed copyPrompt to include the
   question box (its tooltip already promised "selection + question").
6. CMake: WebEngineWidgets required; chat_resources removed; test_chat_ui /
   test_chat_rich targets removed; test_reader_ui links WebEngine and gets
   the shared WebEngine test env (sandbox off + sysroot resource paths).
7. test_reader_ui rewritten (~390 lines): drag→context→browser-surface,
   copy-prompt (offline), literal search, outline activation, h+space→browser
   ask, history, rotation anchor retention, pin, equation/figure/table/
   citation clicks, AI toggle, cache-safe reopen, doc-switch isolation.
   (Content assertions use the actual resolved selection text: the synthetic
   drag geometry resolves to a neighboring line — pre-existing engine
   behavior the old test never checked.)
8. README test commands updated (no more chat_rich exclusion).
9. Selection highlight rework (2026-09-22): PageWidget painted one tight
   rect per whole word (gaps at every space, updates only at word
   boundaries). Now: character-precise endpoints (proportional char offset
   within boundary words, clamped past-the-text drags to document ends
   instead of dropping the selection) and one continuous rect per visual
   row, so inter-word spaces stay painted. `test_select` pins both: a
   partial-word drag yields a strict substring, and every background pixel
   in an inter-word gap carries the wash. Full suite 9/9 in both build
   dirs (`build-qt` Release + `/tmp/reader-full3` Debug).
10. Desktop entry + shutdown (2026-09-22): `packaging/paper-reader.desktop`
    + `packaging/paper-reader.svg`, installed to
    `~/.local/share/applications` + hicolor icons (validated with
    desktop-file-validate) — the app appears in the launcher and opens PDFs
    via `run.sh %f`. New `shutdown` test closes the window mid-background-
    work from inside the running loop: open→quit in ~105 ms, exit 0, no
    lingering QtWebEngineProcess. Audit: token cancels, QPointer guards,
    context-bound timers/invokes, pool joins bounded by in-flight jobs
    (ChatManager::stop is cancel-only).     Suite now 10/10 in both build dirs.
11. Multiline highlight hugs rows (2026-09-22): pressing `h` saved the
    selection's bounding box as ONE annotation, so a partial multiline drag
    painted the whole sentence block. Now the drag stashes its per-row
    rects (`selectionRows_`, PDF points, cleared with the selection and on
    document switch) and `h` saves one annotation per row; the overlay and
    the jump highlight paint rows, never the box. Geometry-based
    match/remove logic is unchanged (overlap ≥ 0.5), so toggle-off still
    clears the whole group at once. `test_select` pins it: a two-line drag
    saves exactly 2 annotations, the first row's unselected prefix stays
    clean, and removal restores the pixel state. Suite 10/10 in both dirs.
12. Row-aware endpoint snap (2026-09-22): press/release points falling in
    the inter-line gap used a global-nearest rule that grabbed a word on
    the wrong row (aiming at "makes" anchored "premises", so both the blue
    selection and the saved highlight started a line late). The rule is now
    `reader::snapWordIndex` (`src/pdf/WordBoxes.cpp`): exact hit, then the
    row the point visually sits on, then global-nearest fallback. Unit-
    pinned in `test_select` with tight-leading synthetic boxes — verified
    the test FAILS on the old rule and passes on the new one. Suite 10/10
    in both dirs; `./run.sh` binary rebuilt with the fix.
14. Batch 2026-09-22 — chat nav auto-hide, last-dir, in-PDF highlights:
    - Browser nav row (‹ › ⟳ chatgpt.com Copy prompt) auto-hides with a
      250 ms edge reveal like the main toolbar; the ask row never hides.
    - File→Open starts in `reader/lastOpenDir` (Documents folder default)
      and remembers the picked folder.
    - Fresh-start chat defaults ON (pinned by test); toggle-off persists
      per user choice. Reading position already restores per document hash
      (pinned by test: leave page 2, reopen, back on page 2) — a changed
      file is a new hash and starts at page 0 by cache-identity design.
    - Save highlights into PDF (toolbar + Ctrl+S): per-row SQLite geometry
      is baked into a Save-As copy via hand-written overlay PDF +
      `qpdf --overlay` (poppler-qt6 has no save API; needs qpdf+pdfinfo at
      runtime, else the action reports what is missing). Refuses rotated
      pages instead of misplacing; verifies by re-opening (page count)
      before delivering; the original is never overwritten (same-file
      choice is rejected). `test_export` proves the round trip down to
      pixels (builder, overlay pairing, `embedHighlights`, source
      byte-identical).     Suite 11/11 in both dirs.
15. In-place save (2026-09-22): Ctrl+S / Save writes highlights straight
    into the open PDF — no dialog. Merge stages temp siblings next to the
    file (same filesystem), verifies by re-opening, then atomically
    replaces the original. The changed hash reopens the paper with the
    reading position restored (`pendingPosition_`). `test_export` covers
    the same-file path: baked wash present, page count intact, no staging
    files left. Suite 11/11 in both dirs.
13. Chat keystrokes no longer stolen (2026-09-22): the app-wide space/h/n/a
    shortcuts only exempted Qt text inputs, so typing space (or h/n/a) in
    ChatGPT's composer jerked focus to the ask box / fired reader actions.
    New `MainWindow::chatHasFocus()` (focus anywhere inside the browser
    panel, incl. the web view's internal proxy) guards space, h/n/a and the
    Ctrl+H/Ctrl+N variants. Pinned in `test_reader_ui`: with web focus,
    space keeps focus in the page (verified the test FAILS with the guard
    disabled — focus lands on the ask box) and `n` opens no note editor.
    Suite 10/10 in both dirs.

## 4. Verification log
- Full `ctest`: **9/9 pass** (core, chat_lifecycle, qt, select,
  embedding_qt, reader_ui, render, render_stress, webengine).
- Qt-free `BUILD_UI=OFF` build: 2/2 pass.
- Zero compiler warnings in touched UI files; zero dangling references to
  removed symbols (grep-verified); paper-reader + test_reader_ui link clean.

## 5. Follow-ups (not yet done)
- Phase C: remove dead core (ChatManager/OpenAIProvider/EmbeddingProviderQt/
  VectorIndex-semantic/RetrievalEngine-semantic/PaperIngestor) + their tests,
  once a replacement decision for offline lexical retrieval in browser prompts
  is made. Currently zero runtime cost (nothing in the UI invokes them).
- Scroll FPS still unbenchmarked (was already true pre-slim); needs a
  frame-timing harness before any 60 FPS claim.
- PageWidget-per-page widget cost for 1000+ page docs is unchanged; virtualize
  if profiling shows it.
