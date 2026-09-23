#pragma once
#include "pdf/PdfEngine.h"
#include <memory>

class QPdfDocument;

// Qt (PDFium-based) IPdfEngine implementation (§54). All calls are
// synchronous and run on worker lanes; the UI thread only renders.
// The document is shared (deleteLater deleter) so background jobs can
// safely outlive a reopen.
class QtPdfEngine : public reader::IPdfEngine {
public:
    explicit QtPdfEngine(std::shared_ptr<QPdfDocument> doc = nullptr) : doc_(std::move(doc)) {}
    void attach(std::shared_ptr<QPdfDocument> doc) { doc_ = std::move(doc); }

    bool open(const std::string& path) override;
    // Human-readable reason for the last open() failure ("" once open).
    std::string lastError() const { return lastError_; }
    int pageCount() const override;
    std::vector<reader::TextSpan> extractSpans(int page) override;
    std::vector<reader::PdfOutlineEntry> outline() override;
    std::string name() const override { return "qtpdf"; }

private:
    std::shared_ptr<QPdfDocument> doc_;
    std::string lastError_;
};
