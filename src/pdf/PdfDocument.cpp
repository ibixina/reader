#include "pdf/PdfDocument.h"
#include "pdf/TextExtractor.h"
#include <filesystem>
#include <filesystem>

namespace reader {

PdfDocument::PdfDocument(std::unique_ptr<IPdfEngine> engine) : engine_(std::move(engine)) {}

bool PdfDocument::open(const std::string& path) {
    if (!engine_->open(path)) return false;
    DocumentModel m;
    m.document.filePath = path;
    m.document.fileHash = sha256File(path);
    m.document.id = m.document.fileHash.empty() ? path : m.document.fileHash;
    m.document.title = std::filesystem::path(path).stem().string();
    m.document.pageCount = engine_->pageCount();
    // Stage 1: geometry + identity available immediately; text follows async.
    model_ = std::move(m);
    setStage(Stage::RenderReady);
    return true;
}

void PdfDocument::close(CancellationToken token) {
    token.cancel();
    model_ = DocumentModel{};
    setStage(Stage::Closed);
}

} // namespace reader
