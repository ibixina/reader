#pragma once
#include "core/Json.h"
#include "core/Types.h"
#include <optional>
#include <string>
#include <vector>

namespace reader {

// Structured whole-paper analysis returned by Ingest (§5.6). Always schema
// validated + normalized before it touches the UI or cache (§5.1 step 6).
inline constexpr int kAnalysisSchemaVersion = 1;

struct Overview {
    std::string researchQuestion;
    std::string mainIdea;
    std::string mainContribution;
    std::string method;
    // Optional structured fields used by richer summary views. Older
    // manifests omit them and continue to parse with empty defaults.
    std::string architecture;
    std::string setup;
    std::string takeaway;
    std::vector<std::string> mainResults;
    std::vector<std::string> limitations;
    std::vector<BlockId> sources;
};

struct SectionSummary {
    SectionId sectionId;
    std::string summaryShort;
    std::string summaryDetailed;
    double importance = 0;
    std::vector<BlockId> sources;
};

struct Annotation {
    BlockId blockId;
    std::size_t start = 0, end = 0;
    std::string type; // key_idea|definition|claim|method|result|limitation|assumption
    double importance = 0;
    std::string reason;
};

struct Concept {
    ConceptId id;
    std::string name;
    std::string description;
    std::string type; // concept|method|model|variable|equation|experiment|claim|result|dataset|prior_work
    std::vector<BlockId> sources;
};

struct Relationship {
    ConceptId source;
    ConceptId target;
    std::string relation;
    std::vector<BlockId> sources;
};

struct FigureInfo {
    FigureId figureId;
    std::string summary;
    double importance = 0;
};

struct EquationInfo {
    EquationId equationId;
    std::string purpose;
    double importance = 0;
};

struct AnalysisMeta {
    int schemaVersion = kAnalysisSchemaVersion;
    int promptVersion = 1;
    std::string provider;
    std::string model;
    TimestampMs generatedAt = 0;
};

struct PaperAnalysis {
    Overview overview;
    std::vector<SectionSummary> sections;
    std::vector<Annotation> annotations;
    std::vector<Concept> concepts;
    std::vector<Relationship> relationships;
    std::vector<FigureInfo> figures;
    std::vector<EquationInfo> equations;
    AnalysisMeta meta;

    json::Value toJson() const;
    // Parses + validates + normalizes (clamps ranges, drops dangling IDs).
    // Returns nullopt with an error string on schema failure.
    static std::optional<PaperAnalysis> fromJson(const json::Value& v, std::string& error);
    static std::optional<PaperAnalysis> parse(const std::string& text, std::string& error);
    // parse() plus chat-model conveniences: strips ```json fences and
    // extracts the first {...} object before validating.
    static std::optional<PaperAnalysis> parseLenient(const std::string& text, std::string& error);
    // True when the manifest carries anything worth showing. Ingest must
    // never report success on an empty manifest (§5.2 honesty).
    bool usable() const {
        return !overview.mainIdea.empty() || !sections.empty() || !annotations.empty();
    }
};

} // namespace reader
