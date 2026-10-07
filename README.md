# Paper Reader

This is a local Qt 6 paper reader with SQLite persistence and an offline
provider by default. Opening a paper does not contact a model or embedding
service. Network access occurs only after the user configures and explicitly
uses a compatible provider.

## Build and test

The core build needs C++20 and SQLite development files. The full application
also needs Qt 6 Widgets, Network, Pdf, Sql, Concurrent, and Poppler Qt 6.
WebEngine and the browser transcript are optional when those Qt components are
available.

```sh
CONDA_PREFIX=/home/nao/miniforge3 cmake -S . -B /tmp/reader-full3 \
  -DCMAKE_PREFIX_PATH=/home/nao/qtsysroot/usr \
  -DBUILD_UI=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
flock /tmp/reader-full3-build.lock cmake --build /tmp/reader-full3 --parallel 1
ctest --test-dir /tmp/reader-full3 -E '^webengine$' --output-on-failure

# The embedding and compatible-chat tests bind loopback-only fake servers
# unconditionally; they skip themselves if the bind is unavailable.
ctest --test-dir /tmp/reader-full3 --output-on-failure

# Launch this build (pass a PDF path as an optional argument).
PAPER_READER_BUILD_DIR=/tmp/reader-full3 ./run.sh
```

Tests run with unique HOME and XDG configuration, cache, and data directories.
The WebEngine tests block external requests; on hosts that restrict Chromium's
sandbox, run them in the host's authorized test environment. Embedding and
compatible-chat tests use loopback-only fake HTTP servers; restricted containers
may skip the bind checks. Test wrappers use isolated HOME/XDG directories and
unset provider credentials. The browser chat is the only AI chat and requires
Qt WebEngine at build time.

A Qt-free build is supported with `-DBUILD_UI=OFF -DBUILD_TESTS=ON`; it builds
the core library plus the core and chat-lifecycle tests using only C++20,
threads, and SQLite.

The app starts on **Your documents**. Use **Choose folder…** to select a folder;
the shelf remembers it and shows all readable PDFs directly inside it (including
`.PDF` files), ordered by the latest open or creation time. On filesystems without
creation dates, modification time is used instead. The shelf refreshes as files
are added or removed. Papers appear in a responsive grid with first-page covers,
titles, and reading progress. Cover previews load as you scroll and are cached
locally; changing a PDF refreshes its preview. Click a document, or select it and
press Enter, to open it at its saved page, scroll position, zoom, rotation, and
page mode. The library search filters filenames as you type, ignoring case and
matching every word. Results keep their recent-activity order. Press Enter in
the search bar to open the selected match, or clear it to show the full shelf.
The query stays when you return from a paper and resets when you choose a new
folder or restart the app. Use **Shelf** in the reader toolbar
or **Ctrl+L** to return to the list. Reading position saves automatically and
when returning to the shelf, switching documents, or closing the app. Launching
with a PDF argument still opens that file directly.

**Open** in the toolbar has a search bar that filters PDF filenames in the
current folder as you type. Search ignores case and matches every word you enter.
Select a result and click **Open**, double-click it, or press Enter to open it.
Clear the search to see all PDFs and folders; use **Up** or **Choose folder…**
to browse elsewhere. The folder path is read-only and there is no filename field.

In the browser chat pane, Enter sends the current selection and question in
one step. **New chat** opens another conversation; each tab keeps its own
conversation and question draft while sharing your ChatGPT login. Double-click
a tab to name it, drag tabs to reorder them, or use its close button. With focus
in the AI pane, Ctrl+T opens a chat and Ctrl+W closes the current chat.

Move the cursor to the left edge to reveal the sidebar; moving back into the
document hides it. It overlays the document without changing its width and
stays open while an input in the sidebar has focus. The **Notes** tab lists
highlights, notes, and bookmarks in document order.
Click a passage to jump to it. Press **N** on selected text to open its quoted
passage and note editor; notes save automatically as you type and when you
close the sidebar or change documents. You can search, edit, and delete notes
there, or remove an entire multiline highlight with one click. **Ctrl+Left**
and **Ctrl+Right** in the PDF pane return to previous and next reading positions,
including the exact scroll offset before an internal document link. Internal
links preserve your current zoom and fit mode.

## Providers and cache

`EchoProvider` is the default offline provider. `EmbeddingProviderQt` is an
explicit OpenAI-compatible `/embeddings` adapter with batch requests,
cancellation, finite-value and dimension checks, and a caller-supplied model,
endpoint, and dimension. `VectorIndex::buildSemantic` must be invoked by an
explicit indexing action. With storage enabled it persists vectors below
`$HOME/.local/share/paper-reader/embeddings`, keyed by document hash, block
content, model identity, and dimension; otherwise the index remains memory-only.
Reader and chat share an immutable semantic snapshot queried on a worker lane,
with lexical fallback on cancellation, stale data, or provider error.

Analysis and document caches are stored below the application data directory;
failed writes are reported and incomplete extraction rows are cache misses.
Do not copy a cache between papers or change the source file without allowing
the hash and model identity checks to invalidate it.

Local Ingest is deterministic extractive analysis and is labeled that way in
the Summary tab. It does not call a model, invent relationships, or treat the
paper title/authors as findings. Remote whole-paper ingest is explicit and its
grounded manifest is validated before it replaces an existing cache.

Current acceptance evidence and the user-deferred MVP 6 multi-paper scope are
recorded in [`docs/spec-acceptance.md`](docs/spec-acceptance.md).
# reader
