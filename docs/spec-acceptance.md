# Specification acceptance record

This record maps `ai_paper_reader_spec.md` to the current local implementation. It records observable behavior and test evidence. The source tree is authoritative; generated files under old build directories are not.

## Shipped core behavior

| Spec area | Current behavior | Evidence |
|---|---|---|
| §1–4 native reader | The PDF remains primary. The Qt reader supports continuous and page modes, zoom/fit/rotation/fullscreen, literal search, selection, links, outline, thumbnails, history, bookmarks, highlights, notes, and persisted reading position. The AI pane can hide, collapse, resize, detach, or move. | `test_reader_ui`, `test_select`, `test_render`, `test_qt`; `tests/academic_fixture.h`. |
| §5 ingest | Opening performs no model request. Explicit ingest runs off the UI thread, reports progress/failure/cancellation, validates grounded JSON, and replaces the analysis/raw-response cache as one recoverable operation. Local ingest is labeled `local-extractive` and does not present titles/authors as synthesized findings. | `test_core` covers local/remote success, malformed data, thrown provider errors, cancellation, unwritable cache, rollback, schema/range validation, and cache reopen. |
| §5.5–5.9 analysis | The manifest retains overview, sections, annotations, concepts, grounded relationships, figures, equations, and metadata. Overview, section summaries, concepts, and relationships carry source block IDs; dangling or ungrounded items are removed before display/cache. Local relationships require a same-block relation cue. | `PaperAnalysis`, `PaperIngestor`, `SummaryPanel`, `MapPanel`; manifest round-trip and normalization in `test_core`. |
| §6–8 rendering and work lanes | Page raster uses bounded background tiles and cache keys. Extraction, analysis, search/embedding, network, and serialized PDF-document work use separate lanes with document generations and cancellation. | `test_render`, `test_reader_ui`, `Application` pools, and `PdfView` generation checks. |
| §9, §12–29 evidence | `DocumentAnchor` covers text, sections, equations, figures, tables, and citations. One shared object-reference builder preserves equation LaTeX, figure captions, table rows, citation metadata, image crops, nearby discussion, and referencing passages. Temporary/pinned context, `@` shorthands, immutable sent references, evidence hover/jump/pin, and exact citation IDs are implemented. | `test_core`, `test_chat_lifecycle`, `test_chat_ui`, `test_reader_ui`. |
| §10–11, §21–25, §30 chat | Per-paper conversations persist. Native chat streams, stops, retries, edits/resends, copies, renders Markdown/code and bundled KaTeX where WebEngine exists, and stores exact cited anchors. Provider and storage failures remain retryable. | `test_chat_lifecycle`, `test_chat_ui`, `test_chat_rich`; `/tmp/reader-chat-evidence.png` and `/tmp/reader-chat-katex.png`. |
| §17, §40, §45 retrieval | Explicit references are prioritized, then grounded paragraph/section, literal search, explicitly enabled semantic search, deterministic lexical fallback, and newest history. An immutable semantic snapshot is queried off the UI thread; stale/error/cancel falls back locally. Analysis overview text is not treated as evidence without anchors. | `test_core`, `test_embedding_qt`, SearchPanel, and ChatPanel integration. |
| §34–36 map | Nodes require valid source blocks. Local edges require a sentence containing both concepts and a semantic relation cue. Clicking nodes navigates or prepares grounded chat context. | `PaperIngestor`, `MapPanel`, `test_core`, `test_reader_ui`. |
| §38–39, §56–57 storage | Documents, extraction geometry, analysis, annotations, notes, bookmarks, conversations, rich reference payloads, and exact assistant source records persist through SQLite/cache with bound parameters and transactions. | `test_core`, `test_chat_lifecycle`. |
| §41–42 keyboard flow | Command palette, reader shortcuts, search, navigation, pane toggle, chat focus, copy, and object/context actions are wired. | `test_reader_ui`, `test_chat_ui`. |
| §48–51 providers and grounding | Offline Echo is the default. OpenAI and compatible chat endpoints are explicit. Prompt preparation bounds the combined system/user payload, preserves the full question or rejects it, truncates UTF-8 safely, and exposes only transmitted evidence to citation resolution. Cross-document evidence is rejected. | `test_core`, `test_chat_lifecycle`, loopback transport tests in `test_chat_ui`. |
| §58–59 offline/privacy | Open, extraction, lexical search, cache load, and local ingest require no key or network. Network chat and embeddings require explicit configuration/action. Embeddings can remain memory-only when local storage is disabled. Tests unset credentials and use isolated HOME/XDG directories. | `RunIsolatedTest.cmake`, `test_embedding_qt`, `test_chat_ui`, `test_reader_ui`. |

## Validation baseline

```sh
CONDA_PREFIX=/home/nao/miniforge3 cmake -S . -B /tmp/reader-full3 \
  -DCMAKE_PREFIX_PATH=/home/nao/qtsysroot/usr \
  -DBUILD_UI=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
flock /tmp/reader-full3-build.lock cmake --build /tmp/reader-full3 --parallel 2
ctest --test-dir /tmp/reader-full3 --output-on-failure
```

The final authorized local run passed all 10 tests in 8.86 seconds with
`READER_REQUIRE_LOCAL_HTTP=1`: core, chat lifecycle, Qt extraction, selection,
native chat, embedding adapter, integrated reader, render, WebEngine, and rich
chat. The embedding and compatible-chat transports used loopback fake servers;
WebEngine tests blocked external requests and used bundled resources. A separate
`BUILD_UI=OFF` build in `/tmp/reader-core-final` compiled without Qt and passed
its 2 core tests.

## Integrated flow and performance evidence

The deterministic four-page academic fixture exercises all §63 flows across the
integrated reader and core contracts: actual drag→ask→source click; pinned
equation plus clicked figure with renderer-produced PNG observed by the
provider; current-section context plus implicit retrieval; and transcript
viewport hover/click with exact preview and PDF highlight. It also covers exact
page/scroll/zoom history, citation/table clicks, two-document stale-result
cancellation, summary/map anchors, and layout controls.

Representative offscreen x86_64 fixture measurements were: cold first paint
453 ms; warm first paint 107–153 ms; selection→context and literal search below
the millisecond timer resolution; delayed-AI page round trip 9–10 ms; and first
token 266–267 ms from a fake provider containing an intentional 250 ms delay.
These measurements support the applicable approximate §60 latency targets.
Scroll frame rate was not benchmarked, so no 60 FPS claim is made.

There are no known remaining mandatory core-reader or chat functional gaps.
PDF object/structure recovery remains heuristic across arbitrary publisher
layouts, and image-only PDFs need OCR that is not present. The fixture validates
caption-to-object bounds when an in-object label exists; otherwise the app keeps
caption-only/no-row results and offers manual region capture rather than
inventing content. No known core path fabricates an answer, source,
relationship, table row, or object bound.

## Optional and environment-dependent items

- Builds without WebEngine use readable limited math rather than full TeX layout.
- Anthropic/Gemini-specific transports are absent; offline, OpenAI, and arbitrary OpenAI-compatible endpoints are supported.
- Citation preview cards and external DOI metadata enrichment are optional under §29. Extracted citation anchors and chat context work without network enrichment.
- Two-page view, presentation mode, recoloring, themes, margin crop, and similar §4.1 later options remain optional.
- The former MapPanel destruction crash was traced under gdb to a scene
  `selectionChanged` callback during teardown; disconnecting it in the
  destructor passes 20/20 allocator-stress runs.

## User-deferred roadmap

The user explicitly deferred MVP 6 multi-paper search, cross-paper contexts, linked notes, and related library-wide workflows. Request preparation rejects cross-document evidence until that mode has an explicit UI and grounding contract. No hosted repository, deployment, or ChatGPT-managed infrastructure is part of this project.
