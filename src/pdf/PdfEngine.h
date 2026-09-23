#pragma once
#include "core/Types.h"
#include <memory>
#include <string>
#include <vector>

namespace reader {

// Raw per-glyph-run text with geometry from the PDF engine (§54).
// The Qt backend (QPdfDocument, PDFium-based) implements IPdfEngine;
// MuPDF/Poppler backends can be dropped in without touching callers.
struct TextSpan {
    std::string text;
    Rect bounds;
    float fontSize = 0;
    bool bold = false;
    int page = 0;
};

struct PdfLink {
    Rect bounds;
    int page = 0;
    int targetPage = -1;
    std::string targetUri;
};

struct PdfOutlineEntry {
    std::string title;
    int page = 0;
    int level = 0;
};

class IPdfEngine {
public:
    virtual ~IPdfEngine() = default;
    virtual bool open(const std::string& path) = 0;
    virtual int pageCount() const = 0;
    virtual std::vector<TextSpan> extractSpans(int page) = 0;
    virtual std::vector<PdfOutlineEntry> outline() = 0;
    virtual std::string name() const = 0;
};

// Deterministic fallback used by tests and when no PDF backend exists.
// Real rendering always goes through the Qt PdfRenderer (§6).
class NullPdfEngine : public IPdfEngine {
public:
    bool open(const std::string&) override { return true; }
    int pageCount() const override { return 0; }
    std::vector<TextSpan> extractSpans(int) override { return {}; }
    std::vector<PdfOutlineEntry> outline() override { return {}; }
    std::string name() const override { return "null"; }
};

} // namespace reader
