# AI-Assisted Research Paper Reader
## Detailed Product and Technical Specification

**Working concept:** A fast, native research-paper/PDF reader with a built-in ChatGPT-style AI interface that is continuously aware of the paper, current reading location, selections, figures, equations, and references.

**Primary implementation language:** C++20/23  
**Suggested GUI framework:** Qt 6  
**Suggested PDF engine:** MuPDF or PDFium  
**Suggested storage:** SQLite  
**Primary design goal:** The PDF reader must remain extremely fast and responsive even when AI features are unavailable, slow, or still processing the document.

---

# 1. Product Vision

The application is a **fast academic PDF reader with an AI system embedded directly into the reading workflow**.

The paper is always the primary object. AI should support reading rather than replace it.

The experience should feel like:

- a lightweight native PDF reader,
- combined with a ChatGPT-style conversation pane,
- with first-class references to exact locations and objects in the paper,
- plus optional AI-generated summaries, concept maps, explanations, and emphasis.

The defining interaction is:

1. The user reads normally.
2. The user selects text, an equation, figure, table, or other document object.
3. The selected object automatically becomes AI context.
4. The user asks a question naturally.
5. The AI answers with clickable references back into the paper.
6. Clicking a reference jumps to and highlights the source passage.

No copy/paste workflow should be required.

---

# 2. Core Design Principles

## 2.1 The PDF reader comes first

Basic reading must never depend on AI.

The following must work offline and independently:

- opening PDFs,
- rendering,
- scrolling,
- zooming,
- search,
- navigation,
- text selection,
- annotations,
- notes,
- bookmarks,
- reading position persistence.

AI processing must never block rendering or input.

---

## 2.2 AI should feel native to the paper

The user should be able to ask:

> Why do they need this?

rather than:

> In section 3.2 of the paper, the authors say X. Why do they need X?

The application should already know:

- which paper is open,
- current page,
- current section,
- current selection,
- nearby text,
- pinned references,
- current conversation.

---

## 2.3 References are first-class objects

The AI interface should not merely paste PDF text into messages.

The application should represent references explicitly:

- selected paragraph,
- page,
- section,
- equation,
- figure,
- table,
- citation,
- result,
- concept.

These references should remain clickable and resolvable.

---

## 2.4 Minimal friction

Common actions should require almost no setup.

Examples:

- select text → type question,
- click equation → ask for derivation,
- click figure → ask what matters,
- click AI source → jump to paper,
- use `@` to reference another paper object,
- pin multiple references for comparison.

---

## 2.5 Progressive enhancement

The PDF should open immediately.

Additional intelligence can appear gradually:

1. render document,
2. extract text,
3. detect structure,
4. build search index,
5. compute embeddings,
6. generate summaries,
7. generate concept graph,
8. identify important passages.

The user should never wait for the full pipeline before reading.

---

# 3. Main Interface

## 3.1 Default layout

The main application consists of two synchronized panes.

```text
┌─────────────────────────────────────────────────────────────────────┐
│ File / Title / Search / Navigation / Zoom                          │
├──────────────────────────────────────────┬──────────────────────────┤
│                                          │ Chat | Map | Summary     │
│                                          ├──────────────────────────┤
│                                          │                          │
│               PDF READER                 │      AI INTERFACE        │
│                                          │                          │
│                                          │                          │
│                                          │                          │
├──────────────────────────────────────────┴──────────────────────────┤
│ Status: page / section / indexing / AI state                       │
└─────────────────────────────────────────────────────────────────────┘
```

The divider between panes must be draggable.

The AI pane should support:

- visible,
- hidden,
- collapsed,
- resized,
- optionally detached into a separate window,
- optionally moved to the left.

Suggested shortcut:

```text
Ctrl+Shift+A
```

Toggle AI pane.

---

# 4. PDF Reader Requirements

## 4.1 Core functionality

Required:

- continuous scrolling,
- page-by-page mode,
- zoom,
- fit width,
- fit page,
- rotate,
- text selection,
- text search,
- clickable PDF links,
- internal PDF links,
- table-of-contents navigation,
- page thumbnails,
- back/forward navigation,
- fullscreen mode,
- annotations,
- bookmarks.

Optional later:

- two-page view,
- presentation mode,
- color inversion,
- custom themes,
- crop margins,
- page recoloring.

---

## 4.2 Navigation history

Academic reading frequently involves jumping to:

- citations,
- appendices,
- equations,
- references,
- figures.

The application should maintain navigation history.

Example:

```text
Page 4
  ↓
click citation [27]
  ↓
Page 14
  ↓
Alt+Left
  ↓
return to exact prior position on Page 4
```

Back/forward should restore:

- page,
- scroll offset,
- zoom,
- optionally active selection.

---


# 5. Ingest Button

The application should expose an explicit **Ingest** action for AI analysis.

The user should be able to open and read any PDF without triggering AI processing. AI-generated annotations, summaries, concept maps, and related analysis should only be created when the user chooses to ingest the paper.

A simple toolbar action is sufficient:

```text
┌─────────────────────────────────────────────────────────────┐
│ JADAI.pdf        Search      6 / 14      125%     [Ingest] │
└─────────────────────────────────────────────────────────────┘
```

The initial state is:

```text
[Ingest]
```

Clicking it starts the whole-paper AI analysis pipeline.

---

## 5.1 Ingest Behavior

Clicking **Ingest** should:

1. Extract the paper into the application's structured document representation.
2. Assign stable IDs to sections, paragraphs, figures, equations, tables, and other relevant blocks.
3. Send the paper and its structured identifiers to the configured AI provider.
4. Ask the model to analyze the paper as a whole.
5. Receive a structured `PaperAnalysis` manifest.
6. Validate and normalize the returned data.
7. Save the analysis locally.
8. Inject the resulting annotations and metadata into the reader interface.

Conceptually:

```text
User clicks Ingest
        │
        ▼
Structured document extraction
        │
        ▼
Whole-paper AI request
        │
        ▼
PaperAnalysis manifest
        │
        ▼
Validate + cache
        │
        ├─────────────┬─────────────┬─────────────┐
        ▼             ▼             ▼             ▼
   Highlights     Summaries      Concept Map   Metadata
        │             │             │             │
        └─────────────┴─────────────┴─────────────┘
                              │
                              ▼
                     Inject into reader UI
```

---

## 5.2 Ingest Button States

The button should communicate the paper's analysis state clearly.

### Not ingested

```text
[Ingest]
```

No AI-generated document annotations exist yet.

The paper remains fully readable.

### Ingesting

```text
[Ingesting…]
```

or:

```text
[Ingesting 42%]
```

The button can show a spinner or progress indicator.

The user must still be able to read and interact with the PDF while ingestion runs.

### Ingested

```text
[Ingested ✓]
```

or a compact status icon:

```text
[✓ Ingested]
```

At this point the cached `PaperAnalysis` is active.

### Ingest failed

```text
[Retry Ingest]
```

The PDF reader remains unaffected.

Hovering or opening the status can show the error.

---

## 5.3 Ingest Menu

Once a paper has been ingested, clicking the control or an adjacent dropdown can expose:

```text
✓ Ingested
──────────────
Re-ingest
Clear AI analysis
View analysis details
AI annotation settings
```

`Re-ingest` should intentionally rerun the whole-paper analysis.

This is useful if:

- the user changes AI models,
- the analysis schema changes,
- the prompt improves,
- the existing analysis is poor.

---

## 5.4 Ingestion Is Explicit, Not Automatic

Opening a paper should **not** automatically send it to an AI provider.

The default flow should be:

```text
Open PDF
   ↓
Read normally
   ↓
User decides AI analysis would be useful
   ↓
Click Ingest
```

This has several advantages:

- no unexpected API cost,
- no unexpected document upload,
- better privacy,
- faster first-open behavior,
- users can use the application as a normal PDF reader,
- users decide which papers deserve deeper AI processing.

---

## 5.5 What Ingest Produces

A successful ingest should generate one coordinated analysis containing data such as:

```text
Paper overview
Section summaries
Important passages
Key definitions
Claims
Methods
Assumptions
Results
Limitations
Important equations
Figure explanations
Concept nodes
Concept relationships
Section importance
Paper structure metadata
```

These should all originate from the same whole-paper analysis rather than from many unrelated UI requests.

---

## 5.6 PaperAnalysis Manifest

The ingest response should be a structured object rather than natural-language prose.

Example shape:

```json
{
  "overview": {
    "research_question": "...",
    "main_idea": "...",
    "main_contribution": "...",
    "method": "...",
    "main_results": ["..."],
    "limitations": ["..."]
  },

  "sections": [
    {
      "section_id": "sec_4",
      "summary_short": "...",
      "summary_detailed": "...",
      "importance": 0.91
    }
  ],

  "annotations": [
    {
      "block_id": "block_143",
      "start": 12,
      "end": 84,
      "type": "key_idea",
      "importance": 0.96,
      "reason": "Defines the central objective."
    }
  ],

  "concepts": [
    {
      "id": "concept_12",
      "name": "Expected Information Gain",
      "description": "...",
      "sources": ["block_81", "block_143"]
    }
  ],

  "relationships": [
    {
      "source": "concept_12",
      "target": "concept_19",
      "relation": "optimizes"
    }
  ],

  "figures": [
    {
      "figure_id": "fig_2",
      "summary": "...",
      "importance": 0.82
    }
  ],

  "equations": [
    {
      "equation_id": "eq_7",
      "purpose": "...",
      "importance": 0.89
    }
  ]
}
```

---

## 5.7 Stable Document IDs

The AI should not be responsible for locating text visually.

Before ingestion, the C++ application should create stable IDs:

```text
sec_1
sec_2
block_001
block_002
eq_1
fig_1
table_1
```

The AI returns references to these IDs.

Example:

```json
{
  "block_id": "block_143",
  "start": 20,
  "end": 91,
  "type": "definition"
}
```

The application then maps that character range back to PDF coordinates.

This gives a clean separation:

```text
AI:
"What is important and what does it mean?"

Application:
"Where is it and how should it be displayed?"
```

The model should not be asked to invent page coordinates or bounding boxes.

---

## 5.8 Injecting Analysis Into the Reader

After ingest completes, the application should render the returned information as overlays and auxiliary UI.

Possible injected features:

### Section summary

```text
3.2 Posterior Estimation

[AI Summary]
The authors train a reusable posterior estimator...
```

### Important passage

```text
The neural posterior estimator is trained across simulated datasets...
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
AI key idea
```

### Definition

A subtle underline or margin marker can indicate a definition.

### Figure annotation

```text
Figure 2                              [AI explanation]
```

### Concept-map population

The Map tab becomes available immediately from the cached manifest.

---

## 5.9 Overlay Architecture

The original PDF should remain unchanged.

Render conceptually as:

```text
PDF page
   +
AI annotation overlay
   +
User annotation overlay
   =
Displayed page
```

Therefore AI-generated annotations can be turned off instantly.

Suggested controls:

```text
AI annotations
[✓] Important passages
[✓] Definitions
[✓] Results
[✓] Limitations
[✓] Section summaries
[ ] Methods
```

---

## 5.10 Ingest Cache

The result should be persisted by document hash.

Example:

```text
~/.local/share/paper-reader/
    papers/
        <sha256>/
            document.json
            analysis.json
            embeddings.db
            chats.db
```

Metadata should include:

```json
{
  "analysis_schema_version": 1,
  "prompt_version": 1,
  "provider": "openai",
  "model": "...",
  "generated_at": "..."
}
```

Opening the same paper later should load the cached analysis immediately.

It should **not** require re-ingestion unless requested.

---

## 5.11 Ingest and Chat Are Separate

The application uses AI in two different modes.

### Ingest

A deliberate, mostly one-time whole-paper operation:

```text
Paper
  ↓
AI
  ↓
PaperAnalysis
```

Used for:

- highlights,
- section summaries,
- concepts,
- important sections,
- methods,
- results,
- limitations,
- figure/equation metadata.

### Chat

Interactive and continuous:

```text
Question
+ selection
+ pinned references
+ current location
+ relevant PaperAnalysis
+ retrieved source passages
      ↓
AI
```

The user should be able to use Chat even before ingestion if desired.

Ingestion simply gives Chat richer paper-level context.

---

## 5.12 Suggested C++ API

```cpp
enum class IngestState {
    NotIngested,
    Preparing,
    Uploading,
    Analyzing,
    Applying,
    Ingested,
    Failed
};
```

```cpp
struct IngestProgress {
    IngestState state;
    float progress;
    std::string statusText;
};
```

```cpp
class PaperIngestor {
public:
    void ingest(DocumentId document);

    void cancel();

    void reingest(DocumentId document);

    std::optional<PaperAnalysis> cachedAnalysis(
        DocumentId document
    ) const;
};
```

Possible progress flow:

```text
Preparing document
      ↓
Sending paper
      ↓
Analyzing paper
      ↓
Validating analysis
      ↓
Applying annotations
      ↓
Done
```

---

## 5.13 Recommended UX

The ideal experience is:

```text
Open JADAI.pdf
      │
      ▼
Paper appears immediately
      │
      ▼
User reads normally
      │
      ▼
Clicks [Ingest]
      │
      ▼
Continues reading while analysis runs
      │
      ▼
[Ingested ✓]
      │
      ├── summaries appear
      ├── important passages are marked
      ├── Map is populated
      └── richer paper-aware chat becomes available
```

The important design choice is that **Ingest is an explicit user action that converts a plain PDF into an AI-augmented paper inside the application**.


# 6. PDF Rendering Architecture

Rendering should be independent of document analysis.

Suggested logical pipeline:

```text
Viewport changes
      │
      ▼
Determine visible pages / tiles
      │
      ▼
Check render cache
   ┌──┴──┐
 cached  missing
   │       │
display    background render
           │
           ▼
         cache
```

Recommended cache key:

```cpp
struct RenderKey {
    DocumentId document;
    int page;
    int zoomBucket;
    int tileX;
    int tileY;
};
```

Use an LRU cache for rendered page tiles.

AI or indexing threads must never share the critical rendering path.

---

# 7. Threading Model

At minimum, logically separate:

```text
Main/UI Thread
    │
    ├── Render Worker Pool
    │
    ├── Text Extraction Workers
    │
    ├── Analysis Workers
    │
    ├── Search / Embedding Workers
    │
    └── Network / AI Streaming Workers
```

Rules:

- the UI thread must not perform expensive parsing,
- rendering should not wait on AI,
- AI should not wait on whole-paper indexing when local context is sufficient,
- background processing should be cancellable,
- closing a document should cancel or safely detach pending jobs.

---

# 8. Internal Document Model

Do not expose raw PDF-engine objects across the whole application.

Build an application-level document model.

## 7.1 Core types

```cpp
struct Document {
    DocumentId id;
    std::string filePath;
    std::string title;
    std::vector<std::string> authors;
    int pageCount;
};
```

```cpp
struct Rect {
    float x;
    float y;
    float width;
    float height;
};
```

```cpp
struct TextBlock {
    BlockId id;
    int page;
    Rect bounds;
    std::string text;
};
```

```cpp
struct Section {
    SectionId id;
    std::string title;
    int level;
    int startPage;
    int endPage;
    std::vector<BlockId> blocks;
};
```

```cpp
struct Equation {
    EquationId id;
    int page;
    Rect bounds;
    std::string extractedText;
    std::string label;
};
```

```cpp
struct Figure {
    FigureId id;
    int page;
    Rect bounds;
    std::string caption;
};
```

```cpp
struct Table {
    TableId id;
    int page;
    Rect bounds;
    std::string caption;
};
```

---

# 9. DocumentAnchor

`DocumentAnchor` should be one of the most important application types.

```cpp
struct DocumentAnchor {
    DocumentId document;
    int page;
    Rect bounds;
    std::string anchorText;

    std::optional<SectionId> section;
    std::optional<BlockId> block;
};
```

It provides a common representation for:

- highlights,
- notes,
- AI sources,
- semantic search results,
- concept-map nodes,
- references in chat,
- bookmarks,
- citations,
- navigation history.

Every feature that points into the paper should resolve to a `DocumentAnchor`.

---

# 10. AI Interface

The AI interface should behave like a native ChatGPT tab built into the reader.

The primary AI tabs are:

```text
Chat | Map | Summary
```

Chat is the central interaction mode.

Map and Summary are alternative views over the same internal paper model.

---

# 11. Chat Pane

## 10.1 Layout

```text
┌────────────────────────────────────────────┐
│ Chat | Map | Summary                       │
├────────────────────────────────────────────┤
│                                            │
│ You                                        │
│ Why do they need this encoder?             │
│                                            │
│ AI                                         │
│ The encoder compresses the previous        │
│ experiment history into a fixed-size       │
│ state used by the policy...                │
│                                            │
│ [§3.2 · p.6] [Figure 2 · p.5]             │
│                                            │
│                                            │
├────────────────────────────────────────────┤
│ JADAI.pdf · §3.2 · p.6                     │
│ Context                                    │
│ [Selected paragraph · p.6] [📌 Eq. 4]     │
│                                            │
│ Ask about the paper...                 ↑   │
└────────────────────────────────────────────┘
```

The chat should support:

- streaming responses,
- Markdown,
- LaTeX,
- code blocks,
- copy,
- stop generation,
- retry/regenerate,
- edit and resend,
- conversation history,
- model selection if supported.

---

# 12. Automatic Selection-to-Context

This is a core feature.

When the user selects text in the PDF:

```text
"We use a recurrent history encoder..."
```

the application immediately adds an internal context reference to the chat composer.

Visible UI:

```text
Context

[ "We use a recurrent history encoder..." · p.6 · §3.2   × ]
```

The system should **not** automatically send a request.

The user can continue reading or type:

```text
why is this necessary?
```

The application then sends the selection as context.

---

# 13. Do Not Overwrite the OS Clipboard by Default

“Auto-copy” should mean:

> Automatically copy the selected content into the application's AI context buffer.

It should not normally overwrite the system clipboard.

Reasons:

- users may already have something copied,
- selections happen frequently,
- automatic clipboard replacement would be intrusive.

An optional preference can expose:

```text
[ ] Also copy PDF selections to system clipboard
```

---

# 14. Temporary and Pinned Context

The chat composer should support two kinds of context.

## 13.1 Temporary context

Normally contains the current PDF selection.

Selecting something else replaces it.

```text
[Selected paragraph · p.6]
```

---

## 13.2 Pinned context

Pinned references survive new selections.

```text
[📌 Equation 4]
[📌 Figure 2]
[Selected paragraph · p.8]
```

This allows questions such as:

```text
How does Equation 4 explain the result shown in Figure 2?
```

Each context card should support:

- pin/unpin,
- remove,
- hover preview,
- click to jump to source.

---

# 15. ContextReference

Represent all explicit chat references using one common type.

```cpp
enum class ReferenceType {
    TextSelection,
    Paragraph,
    Section,
    Page,
    Equation,
    Figure,
    Table,
    Citation,
    Concept
};
```

```cpp
struct ContextReference {
    ReferenceId id;
    ReferenceType type;

    DocumentAnchor anchor;

    std::string displayName;
    std::string extractedText;

    bool pinned = false;
};
```

The chat composer holds live context:

```cpp
struct ComposerContext {
    std::vector<ContextReference> temporary;
    std::vector<ContextReference> pinned;
};
```

---

# 16. Message Context Must Be Snapshotted

When a message is sent, the references used for that message must become immutable.

```cpp
struct ChatMessage {
    MessageId id;
    std::string text;

    std::vector<ContextReference> references;
};
```

Example:

1. user selects paragraph A,
2. asks question 1,
3. user selects paragraph B,
4. asks question 2.

Question 1 must still refer to A even though the live composer now contains B.

---

# 17. Implicit Reading Context

The AI should also receive low-friction implicit context.

Possible implicit context:

- current document,
- current page,
- current section,
- current paragraph under viewport center.

This should be shown minimally:

```text
JADAI.pdf · §3.2 · p.6
```

The user can then ask:

```text
what are they trying to establish here?
```

without selecting anything.

Priority order:

```text
Pinned explicit context
    ↓
Current selection
    ↓
Current paragraph
    ↓
Current section
    ↓
Retrieved relevant passages
    ↓
Whole-paper metadata
```

---

# 18. Input Focus Behavior

Selection should be seamless while typing.

Example:

1. user starts typing in chat,
2. user selects a sentence in the PDF,
3. context card updates,
4. text cursor remains in the chat input.

Selecting in the document should not unnecessarily steal keyboard focus from the composer.

This behavior is important for a fluid workflow.

---

# 19. Manual References With `@`

The chat composer should support explicit references.

Typing:

```text
@
```

opens a searchable selector.

Example:

```text
Reference something

Current selection
Current page
Current section
Equation 4
Equation 5
Figure 1
Figure 2
Table 1
Methods
Results
```

Example composed query:

```text
How does [Equation 4] relate to [Figure 2]?
```

Internally these remain structured references rather than pasted strings.

---

# 20. Supported Reference Shorthands

Potential commands:

```text
@selection
@page
@section
@eq4
@fig2
@table1
@methods
@results
```

The parser can resolve typed aliases to `ContextReference` objects.

---

# 21. AI Answer References

Every paper-specific factual answer should provide source links.

Example:

```text
The history encoder is used because the policy must act on a
variable-length experimental history. The encoder maps that history
to a fixed-dimensional representation that the policy network can use.

[§3.2 · p.6] [Figure 2 · p.5]
```

These are UI elements, not ordinary plain text.

---

# 22. Clicking AI References

Clicking:

```text
[§3.2 · p.6]
```

should:

1. navigate to page 6,
2. scroll to the exact relevant location,
3. briefly highlight the referenced passage,
4. preserve navigation history.

Hovering can show a preview:

```text
┌──────────────────────────────────┐
│ §3.2 · p.6                       │
│                                  │
│ "The history encoder maps the..."│
│                                  │
│                         Go there │
└──────────────────────────────────┘
```

---

# 23. Bidirectional Referencing

The system should support both:

```text
PDF → Chat
```

and:

```text
Chat → PDF
```

Examples:

### PDF → Chat

Select text:

```text
"We jointly amortize..."
```

Ask:

```text
what exactly is jointly amortized here?
```

### Chat → PDF

AI:

```text
The objective is defined earlier in §2.1. [p.3]
```

Click `[p.3]`.

The paper jumps directly there.

---

# 24. Source / Evidence Inspector

Every AI message should expose its evidence.

Example:

```text
Sources 3
```

Click:

```text
Sources used

1. §3.1 · p.5
   "The inference network receives..."

2. §3.2 · p.6
   "The history encoder..."

3. §4 · p.8
   "During training..."
```

Interactions:

- hover → temporarily highlight source in PDF,
- click → jump to source,
- pin → add source to current composer context.

This provides transparent retrieval.

---

# 25. Avoid Duplicating Reference Text in Chat

Do not display long quoted selections directly inside every message.

Instead:

```text
You                                           [1 reference]
Why is this necessary?
```

Click `[1 reference]` to inspect:

```text
Referenced from §3.2 · p.6

"We use a recurrent history encoder..."
```

This keeps conversations compact.

---

# 26. Figures as Chat Context

A figure should be referenceable like text.

Clicking or selecting a figure adds:

```text
[Figure 3 · p.8]
```

The AI context should contain:

- image crop,
- caption,
- nearby text,
- any extracted labels,
- passages elsewhere that reference the figure.

User questions:

```text
what should I notice here?
```

```text
why does the curve flatten?
```

```text
how does this support their main result?
```

---

# 27. Equations as Chat Context

Clicking Equation 7 adds:

```text
[Equation 7 · p.9]
```

Potential questions:

```text
derive this
```

```text
what does each term mean?
```

```text
why is the proportional sign used?
```

```text
derive this from Equation 6
```

The system should preserve LaTeX where possible.

---

# 28. Tables as Chat Context

A table reference should include:

- table image or structured extraction,
- caption,
- row/column labels,
- nearby discussion,
- references elsewhere.

Questions:

```text
what is the strongest result here?
```

```text
compare columns 2 and 4
```

```text
which result is statistically meaningful according to the authors?
```

---

# 29. Citation Objects

Clicking a citation such as:

```text
[17]
```

should optionally open a preview:

```text
Smith et al. 2024
Neural Posterior Estimation...

Cited here because:
The current method builds on their estimator.

[Go to bibliography]
[Open DOI]
[Ask about relationship]
```

Later versions may support:

- opening referenced paper,
- fetching metadata,
- building citation graph,
- cross-paper conversations.

---

# 30. Chat History

Each paper should persist multiple conversations.

Example:

```text
JADAI.pdf

Chat 1 — Understanding amortization
Chat 2 — Experimental design objective
Chat 3 — Figure 4 interpretation
```

The chat pane should support:

- new conversation,
- rename,
- delete,
- reopen,
- search.

The current paper context should automatically apply to new conversations.

---

# 31. Summary Tab

The Summary tab should provide structured, generated understanding of the paper.

Suggested structure:

```text
Research Question
─────────────────
...

Core Idea
─────────
...

Method
──────
...

Model Architecture
──────────────────
...

Experimental Setup
──────────────────
...

Main Results
────────────
...

Limitations
───────────
...

Key Takeaway
────────────
...
```

---

# 32. Summary Depth

Support at least:

```text
Quick
Detailed
Technical
```

## Quick

A short orientation.

## Detailed

Section-by-section explanation.

## Technical

Preserve:

- equations,
- architecture,
- parameters,
- assumptions,
- metrics,
- experimental details.

---

# 33. Section Summaries

Each detected section may have an expandable inline summary.

Example:

```text
▸ Summary
```

Expanded:

```text
▾ Summary

This section introduces the recurrent history encoder and explains
how it compresses the sequence of previous experimental decisions
and observations into a fixed-dimensional state.
```

Settings:

```text
Section summaries:
Off | One sentence | Short | Detailed
```

---

# 34. Concept Map

The Map tab should represent meaningful relationships between paper concepts.

Not just extracted keywords.

Example:

```text
Adaptive Design
      │ chooses
      ▼
Experiment
      │ produces
      ▼
Observation
      │ updates
      ▼
Posterior
```

---

# 35. Concept Graph Types

Suggested node types:

```cpp
enum class ConceptType {
    Concept,
    Method,
    Model,
    Variable,
    Equation,
    Experiment,
    Claim,
    Result,
    Dataset,
    PriorWork
};
```

```cpp
struct ConceptNode {
    ConceptId id;
    ConceptType type;
    std::string name;
    std::string description;

    std::vector<DocumentAnchor> sources;
};
```

```cpp
struct ConceptEdge {
    ConceptId source;
    ConceptId target;
    std::string relation;
};
```

Possible relationship labels:

```text
uses
depends on
estimates
approximates
extends
contrasts with
evaluated on
produces
updates
summarizes
```

---

# 36. Map Interaction

Click node:

```text
Expected Information Gain
```

Show:

```text
Definition:
Expected reduction in uncertainty produced by an experiment.

Appears in:
- §2.2
- Equation 5
- Experiment 1

[Jump to source]
[Ask AI]
```

`Ask AI` should switch to the Chat tab and insert the concept as a context reference.

---

# 37. AI-Generated Emphasis

Optional AI-generated highlighting may identify:

- definitions,
- main claims,
- important assumptions,
- methods,
- important equations,
- results,
- limitations,
- terminology.

AI emphasis must be visually distinct from user highlights.

Example setting:

```text
AI emphasis:
Off | Minimal | Normal | Extensive
```

AI highlighting should be subtle by default.

---

# 38. User Annotations

Support:

- highlight,
- underline,
- strikethrough,
- note,
- margin note,
- bookmark,
- region/image selection.

A floating toolbar may appear after text selection:

```text
Highlight | Note | Ask AI | Copy
```

Selecting `Ask AI` should focus the chat composer without requiring another attach step.

---

# 39. Notes Model

```cpp
struct Note {
    NoteId id;
    DocumentAnchor anchor;
    std::string text;
    Timestamp createdAt;
    Timestamp updatedAt;
};
```

A note should be attached both geometrically and textually to improve resilience.

---

# 40. Search

Support two distinct search modes.

## 39.1 Literal search

```text
Ctrl+F
posterior
```

Fast local text search.

---

## 39.2 Semantic search

Example:

```text
Where do they explain how the policy chooses experiments?
```

Results:

```text
1. §3.1 · p.5
   "...the policy network receives..."

2. §3.4 · p.8
   "...the next experimental design..."
```

Clicking a result jumps to the relevant anchor.

---

# 41. Command Palette

Suggested:

```text
Ctrl+K
```

Example commands:

```text
> explain current section
> summarize current page
> go to methods
> show equation 8
> search posterior
> toggle concept map
> export annotations
> new chat
> pin current selection
```

The command palette can combine:

- application commands,
- document navigation,
- semantic search.

---

# 42. Keyboard-First Interaction

Suggested defaults:

```text
j / k           Scroll
g g             First page
G               Last page
Ctrl+F          Text search
Ctrl+K          Command palette
Ctrl+Shift+A    Toggle AI pane
Ctrl+1          Chat
Ctrl+2          Map
Ctrl+3          Summary
h               Highlight selection
n               Add note
a               Ask about selection
Alt+Left        Back
Alt+Right       Forward
```

Optional Vim-style mode can expose richer bindings.

---

# 43. Document Structure Extraction

Asynchronously detect:

- title,
- authors,
- abstract,
- sections,
- subsections,
- paragraphs,
- equations,
- figures,
- tables,
- references,
- footnotes,
- citations.

Suggested hierarchy:

```text
Document
 ├── Section
 │    ├── Subsection
 │    │    ├── Paragraph
 │    │    ├── Equation
 │    │    ├── Figure
 │    │    └── Table
 │    └── ...
 └── References
```

---

# 44. Ingestion Pipeline

## Stage 1 — Immediate

Target: first page visible as quickly as possible.

- open PDF,
- render visible page,
- restore saved position.

## Stage 2 — Fast extraction

- extract text,
- obtain page geometry,
- load PDF-native outline if present.

## Stage 3 — Structural analysis

- identify headings,
- sections,
- paragraphs,
- equations,
- figures,
- tables.

## Stage 4 — Search indexing

- local text index,
- optional semantic embeddings.

## Stage 5 — AI enrichment

- section summaries,
- full summary,
- concept graph,
- important passages.

All later stages are asynchronous.

---

# 45. Context Retrieval Strategy

Do not send the entire paper to the language model for every question.

Use a hierarchy:

```text
Explicit pinned references
        ↓
Current selection
        ↓
Current paragraph
        ↓
Current section
        ↓
Semantic retrieval
        ↓
Whole-paper metadata
```

Only include as much context as needed.

---

# 46. AI Request Model

Example:

```cpp
struct ChatRequest {
    std::string question;

    PaperMetadata paper;
    ReaderState readerState;

    std::vector<ContextReference> explicitReferences;
    std::vector<RetrievedPassage> retrievedPassages;

    std::vector<ChatMessage> recentConversation;
};
```

---

# 47. Context Manager

Suggested class responsibilities:

```cpp
class ContextManager {
public:
    void setCurrentSelection(ContextReference ref);
    void clearCurrentSelection();

    void pinReference(ReferenceId id);
    void unpinReference(ReferenceId id);

    ComposerContext currentContext() const;

    ChatRequest buildRequest(
        const std::string& question,
        const ReaderState& readerState
    );
};
```

The context manager should remain deterministic whenever possible.

Do not require another LLM just to decide what current selection means.

---

# 48. LLM Provider Abstraction

```cpp
class LlmProvider {
public:
    virtual ~LlmProvider() = default;

    virtual void streamChat(
        const ChatRequest& request,
        StreamCallbacks callbacks
    ) = 0;
};
```

Possible implementations:

```text
OpenAIProvider
AnthropicProvider
GeminiProvider
OpenAICompatibleProvider
LocalProvider
```

The UI should not depend directly on any specific provider.

---

# 49. Streaming

AI responses should stream incrementally.

Requirements:

- first token as quickly as possible,
- user can stop generation,
- incomplete messages clearly indicated,
- citations can resolve after or during generation,
- network errors should not affect the PDF reader.

---

# 50. Source Grounding

Paper-specific answers should distinguish among:

- direct statements from the paper,
- AI interpretation,
- external knowledge.

Example:

```text
Paper:
The authors report that model A achieved the highest likelihood.

Interpretation:
This suggests that their flexible recurrent model better captures
the observed behavior under the evaluated metric.
```

The AI should avoid presenting its own interpretation as an author claim.

---

# 51. Hallucination Behavior

If the paper does not support an answer:

```text
I don't see this addressed explicitly in the paper.
```

is preferable to fabricating evidence.

Every strong paper-specific factual claim should ideally resolve to one or more source anchors.

---

# 52. Suggested C++ / Qt Architecture

Recommended high-level source tree:

```text
src/
    app/
        Application.cpp
        ApplicationState.cpp

    ui/
        MainWindow.cpp
        PdfView.cpp
        ChatPanel.cpp
        MapPanel.cpp
        SummaryPanel.cpp
        CommandPalette.cpp

    pdf/
        PdfDocument.cpp
        PdfRenderer.cpp
        PdfSelection.cpp
        TextExtractor.cpp

    document/
        DocumentModel.cpp
        DocumentAnchor.cpp
        Section.cpp
        Equation.cpp
        Figure.cpp
        Table.cpp

    ai/
        ContextManager.cpp
        ChatManager.cpp
        LlmProvider.cpp
        OpenAIProvider.cpp
        RetrievalEngine.cpp

    analysis/
        StructureDetector.cpp
        SummaryGenerator.cpp
        ConceptExtractor.cpp
        ImportantPassageDetector.cpp

    search/
        TextIndex.cpp
        VectorIndex.cpp

    storage/
        Database.cpp
        DocumentRepository.cpp
        ChatRepository.cpp
        AnnotationRepository.cpp

    core/
        ThreadPool.cpp
        Cache.cpp
        CancellationToken.cpp
        Types.cpp
```

---

# 53. Qt Recommendation

Recommended:

- **Qt 6**
- **QML / Qt Quick** for flexible UI
- **C++ backend** for document logic, rendering, storage, indexing, and AI integration

QML can handle:

- resizing,
- animations,
- pane layout,
- chat bubbles,
- context chips,
- map interactions.

C++ should own:

- PDF state,
- rendering,
- references,
- document model,
- context manager,
- search,
- persistence,
- AI providers.

---

# 54. PDF Engine Evaluation

Strong candidates:

- MuPDF,
- PDFium.

Prototype both against:

1. fast academic-paper rendering,
2. text extraction with coordinates,
3. multi-column selection,
4. hyperlink extraction,
5. page geometry,
6. figure / image access.

Text selection quality matters at least as much as raw rendering performance.

---

# 55. Selection Challenges

Academic PDFs commonly contain:

- two-column layouts,
- equations,
- superscripts,
- subscripts,
- hyphenation,
- ligatures,
- footnotes,
- captions,
- mixed reading order.

The selection system should map:

```text
visual drag
    ↓
logical text span
    ↓
correct reading order
    ↓
DocumentAnchor
```

This is a critical subsystem.

---

# 56. Storage

SQLite is suitable for:

- document metadata,
- reading state,
- annotations,
- notes,
- chat history,
- AI-generated summaries,
- concept graph,
- cached extraction,
- retrieval metadata.

Suggested tables:

```text
documents
reading_state
sections
blocks
anchors
annotations
notes
conversations
messages
message_references
concept_nodes
concept_edges
summaries
```

---

# 57. Cache Strategy

Use a stable file hash, e.g. SHA-256, as document identity.

Cache:

```text
PDF hash
 ├── metadata
 ├── extracted text
 ├── sections
 ├── render cache
 ├── embeddings
 ├── summaries
 ├── concept graph
 └── AI metadata
```

Reopening the same document should avoid expensive reprocessing.

---

# 58. Offline Behavior

Without network access, the following must still work:

- reading,
- rendering,
- search,
- notes,
- highlights,
- bookmarks,
- cached summaries,
- cached concept maps,
- cached chat history.

The AI pane can display a small status:

```text
AI offline
```

The rest of the application should remain fully usable.

---

# 59. Privacy

Suggested settings:

```text
Privacy

[✓] Parse documents locally
[✓] Store embeddings locally
[✓] Send only relevant retrieved passages
[ ] Allow complete document upload
```

For unpublished papers or manuscripts, users should be able to understand exactly what is transmitted.

---

# 60. Performance Targets

Approximate targets:

```text
Open first page                 < 500 ms where practical
Scroll                          60+ FPS
Page jump                       < 100 ms cached
Literal search                  near-instant
Selection → context chip        immediate
Open cached AI summary          immediate
Open cached concept map         < 200 ms
AI first token                  ideally ~1–2 s
```

AI latency should not affect interaction latency.

---

# 61. MVP Roadmap

## MVP 1 — Native reader

Implement:

- open PDF,
- smooth rendering,
- scrolling,
- zoom,
- text search,
- selection,
- navigation,
- reading-state persistence.

Success criterion:

> The app already feels like a high-quality standalone PDF reader.

---

## MVP 2 — AI sidecar

Add:

- right-side Chat tab,
- automatic selection-to-context,
- streaming chat,
- source links,
- click source → jump to PDF.

Success criterion:

> Select → ask → answer → verify source feels nearly frictionless.

---

## MVP 3 — Document intelligence

Add:

- section extraction,
- current-section awareness,
- semantic retrieval,
- section summaries,
- full-paper structured summary.

---

## MVP 4 — Rich paper objects

Add:

- equations,
- figures,
- tables,
- citation references,
- `@` reference selector,
- multiple pinned references.

---

## MVP 5 — Concept map

Add:

- concept graph extraction,
- interactive map,
- node → source navigation,
- node → Chat context.

---

## MVP 6 — Library / multi-paper workflows

Add:

- paper library,
- multi-paper search,
- cross-paper context,
- linked notes,
- citation relationships.

---

# 62. Features to Avoid Early

Do not initially turn the project into:

- a Zotero replacement,
- a word processor,
- a cloud document platform,
- a social reading tool,
- a collaborative editor,
- a generic AI workspace,
- a flashcard ecosystem.

The central use case should remain:

> I have a paper open. Help me understand it as efficiently as possible.

---

# 63. Core User Flows

## Flow A — Explain a selected passage

```text
Open paper
   ↓
Read normally
   ↓
Select sentence
   ↓
Context chip appears
   ↓
Type: "what does this mean?"
   ↓
AI answers
   ↓
Click citation
   ↓
Source passage highlighted
```

---

## Flow B — Compare equation and figure

```text
Click Equation 4
   ↓
Pin
   ↓
Click Figure 2
   ↓
Type:
"How does Eq. 4 explain this result?"
   ↓
AI receives both references
```

---

## Flow C — Ask about current section

```text
Scroll to Methods
   ↓
No explicit selection
   ↓
Type:
"what are they doing here?"
   ↓
Current section used implicitly
```

---

## Flow D — Verify AI answer

```text
AI answer
   ↓
Sources 3
   ↓
Hover source 2
   ↓
PDF highlights source temporarily
   ↓
Click
   ↓
Jump to exact passage
```

---

# 64. Defining Product Principle

The application is not:

> ChatGPT next to a PDF.

It is:

> A PDF reader whose AI interface has native, structured awareness of the document and the reader's current context.

The strongest implementation should make the following interactions feel natural:

```text
select → ask
click equation → derive
click figure → interpret
click citation → inspect
AI source → jump back
pin objects → compare
Map node → ask
Summary statement → show source
```

The reader should almost never need to manually copy text, describe where they are in the paper, or search for the source of an AI answer.

---

# 65. Central Technical Invariant

The central architectural rule should be:

> Every meaningful piece of information shown by the AI should be representable as a reference back to one or more concrete document anchors.

That invariant enables:

- trustworthy answers,
- source inspection,
- bidirectional navigation,
- reusable context,
- concept maps,
- linked summaries,
- persistent conversations,
- rich cross-paper workflows later.

---

# 66. Recommended First Prototype

Build only this:

```text
1. Open PDF
2. Render smoothly
3. Select text reliably
4. Selection appears automatically in Chat context
5. Type a question
6. Stream an AI response
7. Response contains source reference
8. Click source reference
9. Jump to and highlight exact passage
```

If this interaction feels excellent, the most important part of the product already works.

Everything else can then be layered on top.
