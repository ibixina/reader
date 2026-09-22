#pragma once
#include "ai/References.h"
#include "core/Types.h"
#include <optional>
#include <vector>

namespace reader {

// Navigation history (§4.2): page + scroll offset + zoom (+ selection)
// restored on Back/Forward, so citation jumps are lossless.
struct NavEntry {
    int page = 0;
    double scrollY = 0;
    double zoom = 1.25;
    std::optional<DocumentAnchor> selection;
};

class NavigationHistory {
public:
    void clear() { back_.clear(); forward_.clear(); }
    void visit(NavEntry entry);
    bool canBack() const { return back_.size() > 1; }
    bool canForward() const { return !forward_.empty(); }
    NavEntry back();
    NavEntry forward();
    NavEntry current() const { return back_.empty() ? NavEntry{} : back_.back(); }

private:
    std::vector<NavEntry> back_;
    std::vector<NavEntry> forward_;
};

struct AppSettings {
    bool parseLocally = true;
    bool storeEmbeddingsLocally = true;
    bool sendOnlyRetrievedPassages = true;
    bool allowCompleteUpload = false; // §59 default off
    bool copySelectionToClipboard = false; // §13 default off
    std::string aiEmphasis = "normal"; // off|minimal|normal|extensive
    bool showImportant = true, showDefinitions = true, showResults = true,
         showLimitations = true, showSectionSummaries = true, showMethods = false;
    std::string sectionSummaryLevel = "short"; // off|one_sentence|short|detailed
};

class ApplicationState {
public:
    DocumentId openDocument;
    int page = 0;
    double scrollY = 0;
    double zoom = 1.25;
    bool aiPaneVisible = true;
    bool aiPaneCollapsed = false;
    ReaderState readerState;
    AppSettings settings;
    NavigationHistory history;

    void goTo(NavEntry entry);
};

} // namespace reader
