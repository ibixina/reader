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
ctest --test-dir /tmp/reader-full3 -E '^(webengine|chat_rich)$' --output-on-failure

# Authorized local runtime: require loopback tests and run WebEngine coverage.
READER_REQUIRE_LOCAL_HTTP=1 ctest --test-dir /tmp/reader-full3 --output-on-failure

# Launch this build (pass a PDF path as an optional argument).
PAPER_READER_BUILD_DIR=/tmp/reader-full3 ./run.sh
```

Tests run with unique HOME and XDG configuration, cache, and data directories.
The WebEngine tests block external requests; on hosts that restrict Chromium's
sandbox, run them in the host's authorized test environment. Embedding and
compatible-chat tests use loopback-only fake HTTP servers; restricted containers
may skip the bind checks. `chat_rich` also uses WebEngine, so the native command
excludes it with `webengine`. Test wrappers use isolated HOME/XDG directories and
unset provider credentials.

A Qt-free build is supported with `-DBUILD_UI=OFF -DBUILD_TESTS=ON`; it builds
the core library plus the core and chat-lifecycle tests using only C++20,
threads, and SQLite.

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
