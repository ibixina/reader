#include "analysis/PaperAnalysis.h"
#include <cmath>
#include <limits>

namespace reader {

static double clamp01(double v) {
    if (!std::isfinite(v)) return 0;
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

static std::size_t nonnegativeOffset(const json::Value& v) {
    const double n = v.asNumber(0);
    if (!std::isfinite(n) || n <= 0) return 0;
    const double max = static_cast<double>(std::numeric_limits<std::size_t>::max());
    if (n >= max) return std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(n);
}

static std::vector<std::string> strVec(const json::Value& v) {
    std::vector<std::string> out;
    for (const auto& e : v.asArray())
        if (e.isString()) out.push_back(e.asString());
    return out;
}

json::Value PaperAnalysis::toJson() const {
    using namespace json;
    Value root{Object{}};
    Value ov{Object{}};
    ov["research_question"] = overview.researchQuestion;
    ov["main_idea"] = overview.mainIdea;
    ov["main_contribution"] = overview.mainContribution;
    ov["method"] = overview.method;
    ov["architecture"] = overview.architecture;
    ov["setup"] = overview.setup;
    ov["takeaway"] = overview.takeaway;
    Array res, lim;
    for (auto& s : overview.mainResults) res.emplace_back(s);
    for (auto& s : overview.limitations) lim.emplace_back(s);
    ov["main_results"] = res;
    ov["limitations"] = lim;
    Array overviewSources;
    for (const auto& source : overview.sources) overviewSources.emplace_back(source);
    ov["sources"] = overviewSources;
    root["overview"] = ov;

    Array secs;
    for (auto& s : sections) {
        Value o{Object{}};
        o["section_id"] = s.sectionId;
        o["summary_short"] = s.summaryShort;
        o["summary_detailed"] = s.summaryDetailed;
        o["importance"] = s.importance;
        Array sources;
        for (const auto& source : s.sources) sources.emplace_back(source);
        o["sources"] = sources;
        secs.push_back(o);
    }
    root["sections"] = secs;

    Array anns;
    for (auto& a : annotations) {
        Value o{Object{}};
        o["block_id"] = a.blockId;
        o["start"] = static_cast<double>(a.start);
        o["end"] = static_cast<double>(a.end);
        o["type"] = a.type;
        o["importance"] = a.importance;
        o["reason"] = a.reason;
        anns.push_back(o);
    }
    root["annotations"] = anns;

    Array cons;
    for (auto& c : concepts) {
        Value o{Object{}};
        o["id"] = c.id;
        o["name"] = c.name;
        o["description"] = c.description;
        o["type"] = c.type;
        Array src;
        for (auto& s : c.sources) src.emplace_back(s);
        o["sources"] = src;
        cons.push_back(o);
    }
    root["concepts"] = cons;

    Array rels;
    for (auto& r : relationships) {
        Value o{Object{}};
        o["source"] = r.source;
        o["target"] = r.target;
        o["relation"] = r.relation;
        Array sources;
        for (const auto& source : r.sources) sources.emplace_back(source);
        o["sources"] = sources;
        rels.push_back(o);
    }
    root["relationships"] = rels;

    Array figs;
    for (auto& f : figures) {
        Value o{Object{}};
        o["figure_id"] = f.figureId;
        o["summary"] = f.summary;
        o["importance"] = f.importance;
        figs.push_back(o);
    }
    root["figures"] = figs;

    Array eqs;
    for (auto& e : equations) {
        Value o{Object{}};
        o["equation_id"] = e.equationId;
        o["purpose"] = e.purpose;
        o["importance"] = e.importance;
        eqs.push_back(o);
    }
    root["equations"] = eqs;

    Value metaObj{Object{}};
    metaObj["analysis_schema_version"] = meta.schemaVersion;
    metaObj["prompt_version"] = meta.promptVersion;
    metaObj["provider"] = meta.provider;
    metaObj["model"] = meta.model;
    metaObj["generated_at"] = static_cast<double>(meta.generatedAt);
    root["meta"] = metaObj;
    return root;
}

std::optional<PaperAnalysis> PaperAnalysis::fromJson(const json::Value& v, std::string& error) {
    if (!v.isObject()) {
        error = "manifest must be a JSON object";
        return std::nullopt;
    }
    PaperAnalysis a;
    const auto& ov = v.at("overview");
    a.overview.researchQuestion = ov.at("research_question").asString();
    a.overview.mainIdea = ov.at("main_idea").asString();
    a.overview.mainContribution = ov.at("main_contribution").asString();
    a.overview.method = ov.at("method").asString();
    a.overview.architecture = ov.at("architecture").asString();
    a.overview.setup = ov.at("setup").asString();
    a.overview.takeaway = ov.at("takeaway").asString();
    a.overview.mainResults = strVec(ov.at("main_results"));
    a.overview.limitations = strVec(ov.at("limitations"));
    a.overview.sources = strVec(ov.at("sources"));

    for (const auto& s : v.at("sections").asArray()) {
        SectionSummary ss;
        ss.sectionId = s.at("section_id").asString();
        if (ss.sectionId.empty()) continue;
        ss.summaryShort = s.at("summary_short").asString();
        ss.summaryDetailed = s.at("summary_detailed").asString();
        ss.importance = clamp01(s.at("importance").asNumber());
        ss.sources = strVec(s.at("sources"));
        a.sections.push_back(std::move(ss));
    }
    static const char* kTypes[] = {"key_idea", "definition", "claim", "method",
                                   "result", "limitation", "assumption"};
    for (const auto& e : v.at("annotations").asArray()) {
        Annotation an;
        an.blockId = e.at("block_id").asString();
        if (an.blockId.empty()) continue;
        an.start = nonnegativeOffset(e.at("start"));
        an.end = nonnegativeOffset(e.at("end"));
        if (an.end < an.start) std::swap(an.start, an.end);
        an.type = e.at("type").asString("key_idea");
        bool known = false;
        for (auto t : kTypes)
            if (an.type == t) known = true;
        if (!known) an.type = "key_idea";
        an.importance = clamp01(e.at("importance").asNumber(0.5));
        an.reason = e.at("reason").asString();
        a.annotations.push_back(std::move(an));
    }
    for (const auto& e : v.at("concepts").asArray()) {
        Concept c;
        c.id = e.at("id").asString();
        if (c.id.empty()) continue;
        c.name = e.at("name").asString();
        c.description = e.at("description").asString();
        c.type = e.at("type").asString("concept");
        for (const auto& s : e.at("sources").asArray())
            if (s.isString()) c.sources.push_back(s.asString());
        a.concepts.push_back(std::move(c));
    }
    for (const auto& e : v.at("relationships").asArray()) {
        Relationship r;
        r.source = e.at("source").asString();
        r.target = e.at("target").asString();
        r.relation = e.at("relation").asString("relates to");
        r.sources = strVec(e.at("sources"));
        if (!r.source.empty() && !r.target.empty()) a.relationships.push_back(std::move(r));
    }
    for (const auto& e : v.at("figures").asArray()) {
        FigureInfo f;
        f.figureId = e.at("figure_id").asString();
        if (f.figureId.empty()) continue;
        f.summary = e.at("summary").asString();
        f.importance = clamp01(e.at("importance").asNumber(0.5));
        a.figures.push_back(std::move(f));
    }
    for (const auto& e : v.at("equations").asArray()) {
        EquationInfo q;
        q.equationId = e.at("equation_id").asString();
        if (q.equationId.empty()) continue;
        q.purpose = e.at("purpose").asString();
        q.importance = clamp01(e.at("importance").asNumber(0.5));
        a.equations.push_back(std::move(q));
    }
    const auto& meta = v.at("meta");
    const double schemaVersion = meta.at("analysis_schema_version").asNumber(1);
    if (!std::isfinite(schemaVersion) || std::floor(schemaVersion) != schemaVersion ||
        schemaVersion < std::numeric_limits<int>::min() ||
        schemaVersion > std::numeric_limits<int>::max()) {
        error = "invalid analysis_schema_version";
        return std::nullopt;
    }
    a.meta.schemaVersion = static_cast<int>(schemaVersion);
    const double promptVersion = meta.at("prompt_version").asNumber(1);
    if (std::isfinite(promptVersion) &&
        promptVersion >= std::numeric_limits<int>::min() &&
        promptVersion <= std::numeric_limits<int>::max())
        a.meta.promptVersion = static_cast<int>(promptVersion);
    else
        a.meta.promptVersion = 1;
    a.meta.provider = meta.at("provider").asString();
    a.meta.model = meta.at("model").asString();
    const double generatedAt = meta.at("generated_at").asNumber(0);
    const long double generatedAtWide = static_cast<long double>(generatedAt);
    const long double minTimestamp =
        static_cast<long double>(std::numeric_limits<TimestampMs>::min());
    const long double maxTimestampExclusive =
        static_cast<long double>(std::numeric_limits<TimestampMs>::max()) + 1.0L;
    if (std::isfinite(generatedAt) && generatedAtWide >= minTimestamp &&
        generatedAtWide < maxTimestampExclusive)
        a.meta.generatedAt = static_cast<TimestampMs>(generatedAt);
    else
        a.meta.generatedAt = 0;
    if (a.meta.schemaVersion != kAnalysisSchemaVersion) {
        error = "unsupported analysis_schema_version";
        return std::nullopt;
    }
    if (a.overview.mainIdea.empty() && a.sections.empty() && a.annotations.empty()) {
        error = "manifest has no usable content";
        return std::nullopt;
    }
    return a;
}

std::optional<PaperAnalysis> PaperAnalysis::parse(const std::string& text, std::string& error) {
    try {
        return fromJson(json::parse(text), error);
    } catch (const json::ParseError& e) {
        error = e.what();
        return std::nullopt;
    } catch (const std::exception& e) {
        error = std::string("invalid analysis: ") + e.what();
        return std::nullopt;
    }
}

std::optional<PaperAnalysis> PaperAnalysis::parseLenient(const std::string& text,
                                                         std::string& error) {
    // Strip markdown fences the chat model loves to add.
    std::string t = text;
    auto fence = t.find("```");
    while (fence != std::string::npos) {
        auto end = t.find("```", fence + 3);
        if (end == std::string::npos) {
            t.erase(fence, 3);
            break;
        }
        std::string inner = t.substr(fence + 3, end - fence - 3);
        // Drop a leading "json" language tag.
        std::size_t head = inner.find_first_not_of(" \t\r\n");
        if (head != std::string::npos && inner.compare(head, 4, "json") == 0) {
            std::size_t after = head + 4;
            if (after >= inner.size() || inner[after] == '\n' || inner[after] == '\r')
                inner.erase(head, 4);
        }
        t.replace(fence, end - fence + 3, inner);
        fence = t.find("```");
    }
    // Narrow to the first top-level {...} in case of prose around it.
    std::size_t begin = t.find('{');
    std::size_t finish = t.rfind('}');
    if (begin != std::string::npos && finish != std::string::npos && finish > begin)
        t = t.substr(begin, finish - begin + 1);
    return parse(t, error);
}

} // namespace reader
