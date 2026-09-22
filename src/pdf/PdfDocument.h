#pragma once
#include "core/CancellationToken.h"
#include "document/DocumentModel.h"
#include "pdf/PdfEngine.h"
#include <functional>
#include <memory>
#include <string>

namespace reader {

// Owns the engine + staged pipeline (§44). Stage 1 renders immediately;
// stages 2-4 run on workers and never block the UI thread (§2.1, §7).
class PdfDocument {
public:
    enum class Stage { Closed, RenderReady, TextReady, StructureReady, Indexed };

    explicit PdfDocument(std::unique_ptr<IPdfEngine> engine = std::make_unique<NullPdfEngine>());

    bool open(const std::string& path);
    void close(CancellationToken token = {});

    const DocumentModel& model() const { return model_; }
    DocumentModel& model() { return model_; }
    Stage stage() const { return stage_; }
    IPdfEngine& engine() { return *engine_; }

    using StageCallback = std::function<void(Stage)>;
    void setStageCallback(StageCallback cb) { onStage_ = std::move(cb); }

protected:
    void setStage(Stage s) {
        stage_ = s;
        if (onStage_) onStage_(s);
    }
    std::unique_ptr<IPdfEngine> engine_;
    DocumentModel model_;
    Stage stage_ = Stage::Closed;
    StageCallback onStage_;
};

} // namespace reader
