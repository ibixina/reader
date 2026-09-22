#include "analysis/PaperIngestor.h"
#include "core/Json.h"
#include "core/Types.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <unistd.h>

namespace reader {

std::string cacheDirFor(const std::string& fileHash) {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : "/tmp";
    return base + "/.local/share/paper-reader/papers/" + fileHash;
}

namespace {

bool writeAtomically(const std::filesystem::path& target, const std::string& text) {
    std::string tempPattern = target.string() + ".XXXXXX";
    std::vector<char> tempName(tempPattern.begin(), tempPattern.end());
    tempName.push_back('\0');
    const int fd = mkstemp(tempName.data());
    if (fd < 0) return false;
    close(fd);
    const std::filesystem::path temp(tempName.data());
    std::error_code ec;
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    f.flush();
    if (!f) {
        f.close();
        std::filesystem::remove(temp, ec);
        return false;
    }
    f.close();
    if (!f) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

struct FileSnapshot {
    bool existed = false;
    std::string contents;
};

FileSnapshot snapshotFile(const std::filesystem::path& path) {
    FileSnapshot snapshot;
    std::ifstream input(path, std::ios::binary);
    if (!input) return snapshot;
    snapshot.existed = true;
    snapshot.contents.assign(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>());
    return snapshot;
}

bool restoreFile(const std::filesystem::path& path, const FileSnapshot& snapshot) {
    std::error_code ec;
    if (!snapshot.existed) {
        std::filesystem::remove(path, ec);
        return !ec;
    }
    return writeAtomically(path, snapshot.contents);
}

void normalizeForModel(PaperAnalysis& analysis, const DocumentModel& model) {
    std::unordered_set<BlockId> blocks;
    for (const auto& block : model.blocks) blocks.insert(block.id);
    std::unordered_set<SectionId> sections;
    for (const auto& section : model.sections) sections.insert(section.id);
    std::unordered_set<FigureId> figures;
    for (const auto& figure : model.figures) figures.insert(figure.id);
    std::unordered_set<EquationId> equations;
    for (const auto& equation : model.equations) equations.insert(equation.id);

    const auto retainBlocks = [&](std::vector<BlockId>& ids) {
        ids.erase(std::remove_if(ids.begin(), ids.end(),
                                 [&](const BlockId& id) { return !blocks.contains(id); }),
                  ids.end());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    };
    retainBlocks(analysis.overview.sources);
    if (analysis.overview.sources.empty()) analysis.overview = {};

    analysis.sections.erase(
        std::remove_if(analysis.sections.begin(), analysis.sections.end(),
                       [&](const SectionSummary& summary) {
                           return !sections.contains(summary.sectionId);
                       }),
        analysis.sections.end());
    for (auto& summary : analysis.sections) retainBlocks(summary.sources);
    analysis.sections.erase(
        std::remove_if(analysis.sections.begin(), analysis.sections.end(),
                       [](const SectionSummary& summary) { return summary.sources.empty(); }),
        analysis.sections.end());
    for (auto it = analysis.annotations.begin(); it != analysis.annotations.end();) {
        const auto block = model.findBlock(it->blockId);
        if (!block) {
            it = analysis.annotations.erase(it);
            continue;
        }
        it->start = std::min(it->start, block->text.size());
        it->end = std::min(it->end, block->text.size());
        if (it->end < it->start) std::swap(it->start, it->end);
        ++it;
    }
    for (auto& item : analysis.concepts) {
        item.sources.erase(
            std::remove_if(item.sources.begin(), item.sources.end(),
                           [&](const BlockId& id) { return !blocks.contains(id); }),
            item.sources.end());
    }
    analysis.concepts.erase(
        std::remove_if(analysis.concepts.begin(), analysis.concepts.end(),
                       [](const Concept& item) { return item.sources.empty(); }),
        analysis.concepts.end());
    std::unordered_set<ConceptId> concepts;
    for (const auto& item : analysis.concepts) concepts.insert(item.id);
    analysis.relationships.erase(
        std::remove_if(analysis.relationships.begin(), analysis.relationships.end(),
                       [&](const Relationship& relation) {
                           return !concepts.contains(relation.source) ||
                                  !concepts.contains(relation.target);
                       }),
        analysis.relationships.end());
    for (auto& relation : analysis.relationships) retainBlocks(relation.sources);
    analysis.relationships.erase(
        std::remove_if(analysis.relationships.begin(), analysis.relationships.end(),
                       [](const Relationship& relation) { return relation.sources.empty(); }),
        analysis.relationships.end());
    analysis.figures.erase(
        std::remove_if(analysis.figures.begin(), analysis.figures.end(),
                       [&](const FigureInfo& figure) { return !figures.contains(figure.figureId); }),
        analysis.figures.end());
    analysis.equations.erase(
        std::remove_if(analysis.equations.begin(), analysis.equations.end(),
                       [&](const EquationInfo& equation) {
                           return !equations.contains(equation.equationId);
                       }),
        analysis.equations.end());
}

std::vector<std::string> sentences(const std::string& text) {
    static const std::regex re(R"([^.!?]+[.!?])");
    std::vector<std::string> out;
    auto b = text.cbegin(), e = text.cend();
    std::smatch m;
    while (std::regex_search(b, e, m, re)) {
        std::string s = trim(m[0].str());
        if (s.size() > 20) out.push_back(s);
        b = m.suffix().first;
    }
    const std::string tail = trim(std::string(b, e));
    if (tail.size() > 20) out.push_back(tail);
    return out;
}

std::map<std::string, int> termFreq(const std::string& text) {
    std::map<std::string, int> tf;
    std::string cur;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!cur.empty()) {
            if (cur.size() > 3) tf[cur]++;
            cur.clear();
        }
    }
    if (cur.size() > 3) tf[cur]++;
    return tf;
}

double sentenceScore(const std::string& s, const std::map<std::string, int>& docTf) {
    static const char* cues[] = {"result", "show", "demonstrate", "propose", "define",
                                 "theorem", "proof", "conclud", "significan", "novel",
                                 "achieve", "outperform", "limitation", "assum"};
    double score = 0;
    std::string lower = toLower(s);
    for (auto c : cues)
        if (lower.find(c) != std::string::npos) score += 1.0;
    std::string cur;
    int words = 0;
    for (char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        else if (!cur.empty()) {
            auto it = docTf.find(cur);
            if (it != docTf.end() && it->second >= 3) score += 0.3;
            ++words;
            cur.clear();
        }
    }
    if (words > 8 && words < 45) score += 0.5;
    return score;
}
} // namespace

PaperAnalysis PaperIngestor::analyzeLocal(const DocumentModel& model, CancellationToken token) {
    PaperAnalysis a;
    a.meta.provider = "local-extractive";
    a.meta.model = "deterministic-extractive-v1";
    a.meta.generatedAt = nowMs();

    const auto isMetadataCandidate = [&](const TextBlock& block) {
        const std::string text = trim(block.text);
        if (!model.document.title.empty() && text == trim(model.document.title)) return true;
        for (const auto& author : model.document.authors)
            if (!author.empty() && text.find(author) != std::string::npos) return true;
        for (const auto& section : model.sections)
            if (text == trim(section.title)) return true;
        return false;
    };
    const bool hasBodyBlock = std::any_of(
        model.blocks.begin(), model.blocks.end(), [&](const TextBlock& block) {
            return !trim(block.text).empty() && !isMetadataCandidate(block);
        });
    const auto isMetadataBlock = [&](const TextBlock& block) {
        // Some engines return a whole one-page document as one block and the
        // structure pass necessarily uses that block as the title. In that
        // case it is still the only body evidence and must remain available.
        return hasBodyBlock && isMetadataCandidate(block);
    };

    std::string all;
    for (const auto& b : model.blocks) {
        if (token.cancelled()) break;
        if (isMetadataBlock(b)) continue;
        all += b.text + "\n";
    }
    auto docTf = termFreq(all);

    // Section summaries: top sentences per section.
    for (const auto& s : model.sections) {
        if (token.cancelled()) break;
        std::string text;
        for (const auto& bid : s.blocks)
            if (const TextBlock* b = model.findBlock(bid); b && !isMetadataBlock(*b))
                text += b->text + " ";
        auto sents = sentences(text);
        std::sort(sents.begin(), sents.end(), [&](const std::string& x, const std::string& y) {
            return sentenceScore(x, docTf) > sentenceScore(y, docTf);
        });
        SectionSummary ss;
        ss.sectionId = s.id;
        ss.summaryShort = sents.empty() ? "" : sents.front();
        for (std::size_t i = 0; i < std::min<std::size_t>(3, sents.size()); ++i) {
            if (i) ss.summaryDetailed += " ";
            ss.summaryDetailed += sents[i];
        }
        ss.importance = std::min(1.0, 0.35 + 0.1 * static_cast<double>(s.blocks.size()));
        for (const auto& blockId : s.blocks)
            if (const TextBlock* source = model.findBlock(blockId);
                source && !isMetadataBlock(*source))
                ss.sources.push_back(blockId);
        if (!ss.summaryShort.empty()) a.sections.push_back(std::move(ss));
    }

    // Important passages: top-scoring sentences mapped back to blocks.
    struct Cand {
        double score;
        std::size_t blockIdx;
        std::size_t start, end;
        std::string text;
    };
    std::vector<Cand> cands;
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        if (token.cancelled()) break;
        const auto& b = model.blocks[i];
        if (isMetadataBlock(b)) continue;
        for (const auto& s : sentences(b.text)) {
            double sc = sentenceScore(s, docTf);
            if (sc < 1.5) continue;
            std::size_t pos = b.text.find(s.substr(0, std::min<std::size_t>(24, s.size())));
            if (pos == std::string::npos) continue;
            cands.emplace_back(sc, i, pos, pos + s.size(), s);
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score > y.score; });
    std::size_t keep = std::min<std::size_t>(40, cands.size());
    auto typeFor = [](const std::string& s) -> const char* {
        std::string l = toLower(s);
        if (l.find("defin") != std::string::npos) return "definition";
        if (l.find("assum") != std::string::npos) return "assumption";
        if (l.find("limitation") != std::string::npos) return "limitation";
        if (l.find("result") != std::string::npos || l.find("achiev") != std::string::npos) return "result";
        if (l.find("method") != std::string::npos || l.find("algorithm") != std::string::npos) return "method";
        if (l.find("claim") != std::string::npos || l.find("theorem") != std::string::npos) return "claim";
        return "key_idea";
    };
    for (std::size_t i = 0; i < keep; ++i) {
        const auto& c = cands[i];
        Annotation an;
        an.blockId = model.blocks[c.blockIdx].id;
        an.start = c.start;
        an.end = std::min(model.blocks[c.blockIdx].text.size(), c.start + c.text.size());
        an.type = typeFor(c.text);
        an.importance = std::min(1.0, 0.5 + c.score * 0.1);
        an.reason = "High-salience sentence (local heuristic).";
        a.annotations.push_back(std::move(an));
    }

    // Concepts: frequent multi-occurrence terms with source blocks.
    std::vector<std::pair<std::string, int>> freq(docTf.begin(), docTf.end());
    std::sort(freq.begin(), freq.end(), [](auto& x, auto& y) { return x.second > y.second; });
    static const char* stop[] = {"this", "that", "with", "from", "have", "will", "they", "their",
                                 "which", "such", "more", "than", "also", "using", "used", "between"};
    int made = 0;
    for (auto [term, count] : freq) {
        if (made >= 24 || count < 4) break;
        bool isStop = false;
        for (auto s : stop)
            if (term == s) isStop = true;
        if (isStop) continue;
        Concept c;
        c.id = "concept_" + std::to_string(made + 1);
        c.name = term;
        c.type = "concept";
        for (const auto& b : model.blocks) {
            if (isMetadataBlock(b)) continue;
            if (toLower(b.text).find(term) != std::string::npos) {
                c.sources.push_back(b.id);
                if (c.sources.size() >= 4) break;
            }
        }
        if (c.sources.size() >= 2) {
            c.description = "Recurring term (" + std::to_string(count) + " mentions).";
            a.concepts.push_back(std::move(c));
            ++made;
        }
    }
    // Relationships are emitted only when one source sentence names both
    // concepts and contains a grounded relation cue. Co-occurrence alone is
    // insufficient evidence for a semantic edge.
    static const std::pair<const char*, const char*> relationCues[] = {
        {" uses ", "uses"},       {" produces ", "produces"},
        {" updates ", "updates"}, {" improves ", "improves"},
        {" depends ", "depends"}, {" combines ", "combines"}};
    for (std::size_t i = 0; i < a.concepts.size() && i < 12; ++i) {
        for (std::size_t j = i + 1; j < a.concepts.size() && j < 12; ++j) {
            bool found = false;
            std::string relation;
            BlockId evidence;
            for (const auto& block : model.blocks) {
                const auto lower = toLower(block.text);
                if (lower.find(a.concepts[i].name) == std::string::npos ||
                    lower.find(a.concepts[j].name) == std::string::npos)
                    continue;
                for (const auto& cue : relationCues)
                    if (lower.find(cue.first) != std::string::npos) {
                        relation = cue.second;
                        evidence = block.id;
                        found = true;
                        break;
                    }
                if (found) break;
            }
            if (found)
                a.relationships.push_back(
                    {a.concepts[i].id, a.concepts[j].id, relation, {evidence}});
        }
    }

    for (const auto& f : model.figures) {
        FigureInfo fi;
        fi.figureId = f.id;
        fi.summary = f.caption.substr(0, 200);
        fi.importance = 0.6;
        a.figures.push_back(std::move(fi));
    }
    for (const auto& e : model.equations) {
        EquationInfo ei;
        ei.equationId = e.id;
        ei.purpose = e.label.empty() ? "Core relation." : "Relation " + e.label + ".";
        ei.importance = 0.6;
        a.equations.push_back(std::move(ei));
    }

    struct ExtractedOverview {
        std::string text;
        BlockId block;
    };
    const auto findOverview = [&](std::initializer_list<const char*> cues,
                                  const std::string& sectionCue = std::string{}) {
        ExtractedOverview found;
        for (const auto& block : model.blocks) {
            if (isMetadataBlock(block)) continue;
            if (!sectionCue.empty()) {
                const Section* section = model.sectionForBlock(block.id);
                if (!section || toLower(section->title).find(sectionCue) == std::string::npos)
                    continue;
            }
            for (const auto& sentence : sentences(block.text)) {
                const std::string lower = toLower(sentence);
                for (const char* cue : cues)
                    if (lower.find(cue) != std::string::npos)
                        return ExtractedOverview{sentence, block.id};
            }
        }
        return found;
    };
    const auto research = findOverview(
        {"we study", "we investigate", "we examine", "our objective", "research question"});
    const auto contribution = findOverview(
        {"we propose", "we introduce", "our contribution", "we present"});
    auto method = findOverview({"we use", "we render", "our method", "algorithm"}, "method");
    if (method.text.empty())
        method = findOverview({"we use", "we render", "our method", "algorithm"});
    const auto architecture = findOverview({"architecture", "pipeline", "encoder", "decoder"});
    const auto setup = findOverview({"benchmark", "dataset", "corpus", "experimental setup"});
    const auto limitation = findOverview({"limitation"});
    const auto result = findOverview({"result", "show", "achiev", "outperform", "latency"},
                                     "result");
    ExtractedOverview mainIdea;
    for (const auto& block : model.blocks) {
        if (isMetadataBlock(block)) continue;
        const auto blockSentences = sentences(block.text);
        if (!blockSentences.empty()) {
            mainIdea = {blockSentences.front(), block.id};
            break;
        }
    }
    a.overview.researchQuestion = research.text;
    a.overview.mainIdea = mainIdea.text;
    a.overview.mainContribution = contribution.text;
    a.overview.method = method.text;
    a.overview.architecture = architecture.text;
    a.overview.setup = setup.text;
    if (!result.text.empty()) {
        a.overview.mainResults.push_back(result.text);
        a.overview.takeaway = result.text;
    }
    if (!limitation.text.empty()) a.overview.limitations.push_back(limitation.text);
    const auto addOverviewSource = [&](const ExtractedOverview& extracted) {
        if (!extracted.block.empty() &&
            std::find(a.overview.sources.begin(), a.overview.sources.end(), extracted.block) ==
                a.overview.sources.end())
            a.overview.sources.push_back(extracted.block);
    };
    addOverviewSource(research);
    addOverviewSource(mainIdea);
    addOverviewSource(contribution);
    addOverviewSource(method);
    addOverviewSource(architecture);
    addOverviewSource(setup);
    addOverviewSource(limitation);
    addOverviewSource(result);
    // Fallbacks: even a single-paragraph document must yield a usable
    // manifest so Ingest never reports failure on valid input.
    if (a.sections.empty() && !model.blocks.empty()) {
        const TextBlock* fallback = nullptr;
        for (const auto& block : model.blocks)
            if (!isMetadataBlock(block) && !trim(block.text).empty()) {
                fallback = &block;
                break;
            }
        SectionSummary ss;
        ss.sectionId = model.sections.empty() ? "sec_1" : model.sections.front().id;
        ss.summaryShort = fallback ? trim(fallback->text).substr(0, 300) : "";
        ss.summaryDetailed = ss.summaryShort;
        ss.importance = 0.5;
        if (fallback) ss.sources.push_back(fallback->id);
        if (!ss.summaryShort.empty()) a.sections.push_back(std::move(ss));
    }
    if (a.overview.mainIdea.empty()) {
        for (const auto& block : model.blocks)
            if (!isMetadataBlock(block) && !trim(block.text).empty()) {
                a.overview.mainIdea = trim(block.text).substr(0, 300);
                break;
            }
    }
    if (a.annotations.empty() && !model.blocks.empty()) {
        const TextBlock* fallback = nullptr;
        for (const auto& block : model.blocks)
            if (!isMetadataBlock(block) && !trim(block.text).empty()) {
                fallback = &block;
                break;
            }
        if (!fallback) return a;
        Annotation an;
        an.blockId = fallback->id;
        an.start = 0;
        an.end = std::min<std::size_t>(200, fallback->text.size());
        an.type = "key_idea";
        an.importance = 0.5;
        an.reason = "Opening passage (local heuristic fallback).";
        a.annotations.push_back(std::move(an));
    }
    return a;
}

std::string PaperIngestor::buildWholePaperPrompt(const DocumentModel& model) const {
    std::ostringstream os;
    os << "Analyze this research paper and return ONLY a JSON PaperAnalysis manifest "
          "(schema version 1). Use the given stable block/section/equation/figure/table "
          "and citation IDs verbatim; never invent coordinates. The JSON must contain "
          "overview {research_question, main_idea, main_contribution, method, architecture, "
          "setup, takeaway, main_results, limitations, sources:[block_id]}, sections "
          "[{section_id, summary_short, summary_detailed, importance, sources:[block_id]}], "
          "annotations [{block_id, start, end, type, "
          "importance, reason}], concepts [{id, name, description, type, sources}], "
          "relationships [{source, target, relation, sources:[block_id]}], figures "
          "[{figure_id, summary, "
          "importance}], equations [{equation_id, purpose, importance}], and meta "
          "{analysis_schema_version, prompt_version, provider, model, generated_at}.\n\n";
    os << "Title: " << model.document.title << "\n";
    for (const auto& s : model.sections) {
        os << "\n## [" << s.id << "] " << s.title << "\n";
        for (const auto& bid : s.blocks)
            if (const TextBlock* b = model.findBlock(bid)) os << "[" << b->id << "] " << b->text << "\n";
    }
    for (const auto& e : model.equations) os << "\n[" << e.id << "] EQ " << e.extractedText << "\n";
    for (const auto& f : model.figures) os << "\n[" << f.id << "] FIG " << f.caption << "\n";
    for (const auto& t : model.tables) os << "\n[" << t.id << "] TABLE " << t.caption << "\n";
    for (const auto& c : model.citations)
        os << "\n[" << c.id << "] CITATION " << c.raw << "\n";
    return os.str();
}

std::string PaperIngestor::buildGroundedSkeletonPrompt(const DocumentModel& model) const {
    const auto locator = [](const std::string& text, std::size_t max = 150) {
        std::string flat;
        flat.reserve(std::min(max, text.size()));
        bool space = true;
        for (char c : text) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                if (!space) flat.push_back(' ');
                space = true;
            } else {
                flat.push_back(c);
                space = false;
            }
            if (flat.size() >= max) break;
        }
        while (!flat.empty() && flat.back() == ' ') flat.pop_back();
        return flat;
    };
    std::ostringstream os;
    os << "The full paper PDF is attached: read all content, figures, tables and equations "
          "from it. Return ONLY a JSON PaperAnalysis manifest (schema version 1) with overview "
          "{research_question, main_idea, main_contribution, method, architecture, setup, "
          "takeaway, main_results, limitations, sources:[block_id]}, sections [{section_id, "
          "summary_short, summary_detailed, importance, sources:[block_id]}], annotations "
          "[{block_id, start, end, type, importance, reason}], concepts [{id, name, description, "
          "type, sources}], relationships [{source, target, relation, sources:[block_id]}], "
          "figures [{figure_id, summary, importance}], equations [{equation_id, purpose, "
          "importance}], and meta {analysis_schema_version, prompt_version, provider, model, "
          "generated_at}. Cite content ONLY with the stable IDs below, verbatim; never invent "
          "IDs or coordinates. For each annotation, match the block by its locator excerpt, "
          "find the passage in the attached PDF text, and give the character start/end offsets "
          "of that passage within the cited block's text. Cover the whole paper, not just the "
          "introduction: every section below needs annotations.\n\n";
    os << "Title: " << model.document.title << "\n";
    for (const auto& s : model.sections) {
        os << "\n## [" << s.id << "] " << s.title << "\n";
        for (const auto& bid : s.blocks)
            if (const TextBlock* b = model.findBlock(bid))
                os << "[" << b->id << "] " << locator(b->text) << "\n";
    }
    for (const auto& e : model.equations)
        os << "\n[" << e.id << "] EQ " << locator(e.extractedText, 200) << "\n";
    for (const auto& f : model.figures)
        os << "\n[" << f.id << "] FIG " << locator(f.caption, 200) << "\n";
    for (const auto& t : model.tables)
        os << "\n[" << t.id << "] TABLE " << locator(t.caption, 200) << "\n";
    for (const auto& c : model.citations)
        os << "\n[" << c.id << "] CITATION " << locator(c.raw, 200) << "\n";
    return os.str();
}

std::optional<PaperAnalysis> PaperIngestor::ingest(const DocumentModel& model,
                                                   FetchAnalysis remote,
                                                   ProgressCallback progress,
                                                   CancellationToken token) {
    auto report = [&](IngestState st, float p, const std::string& t) {
        if (progress) progress({st, p, t});
    };
    auto reportCancelled = [&] {
        report(IngestState::NotIngested, 0.0f, "Ingest cancelled");
    };
    report(IngestState::Preparing, 0.1f, "Preparing document");
    if (token.cancelled()) {
        reportCancelled();
        return std::nullopt;
    }

    PaperAnalysis local = analyzeLocal(model, token);
    if (token.cancelled()) {
        reportCancelled();
        return std::nullopt;
    }

    std::optional<PaperAnalysis> finalAnalysis;
    std::string rawResponse;
    const bool remoteRequested = static_cast<bool>(remote);
    if (remote && !token.cancelled()) {
        report(IngestState::Uploading, 0.4f, "Sending paper");
        std::string prompt = buildWholePaperPrompt(model);
        report(IngestState::Analyzing, 0.6f, "Analyzing paper");
        std::string raw;
        try {
            raw = remote(prompt);
        } catch (const std::exception& error) {
            report(IngestState::Failed, 1.0f,
                   std::string("Remote analysis failed: ") + error.what());
            return std::nullopt;
        } catch (...) {
            report(IngestState::Failed, 1.0f, "Remote analysis failed unexpectedly");
            return std::nullopt;
        }
        if (token.cancelled()) {
            reportCancelled();
            return std::nullopt;
        }
        if (!token.cancelled() && !raw.empty()) {
            std::string error;
            if (auto parsed = PaperAnalysis::parseLenient(raw, error)) {
                if (normalizeAnalysis(*parsed, model, &error)) {
                    finalAnalysis = *parsed;
                    rawResponse = std::move(raw);
                }
            }
        }
    }
    if (!finalAnalysis) {
        if (remoteRequested) {
            report(IngestState::Failed, 1.0f,
                   "Remote analysis failed or returned an invalid manifest");
            return std::nullopt;
        }
        finalAnalysis = local;
    }

    report(IngestState::Applying, 0.9f, "Validating analysis");
    if (token.cancelled()) {
        reportCancelled();
        return std::nullopt;
    }
    if (!finalAnalysis->usable()) {
        report(IngestState::Failed, 1.0f, "Analysis contained no usable grounded content");
        return std::nullopt;
    }

    const std::filesystem::path cacheDirectory = cacheDirFor(model.document.fileHash);
    const std::filesystem::path analysisPath = cacheDirectory / "analysis.json";
    const std::filesystem::path rawPath = cacheDirectory / "response.txt";
    const FileSnapshot oldAnalysis = snapshotFile(analysisPath);
    const FileSnapshot oldRaw = snapshotFile(rawPath);

    // For a remote result, make the raw response durable first, then publish
    // its validated manifest. Roll back either file on failure so a previous
    // active analysis is never paired with a different raw response.
    if (!rawResponse.empty() && !saveRawResponse(model.document.fileHash, rawResponse)) {
        report(IngestState::Failed, 1.0f, "Unable to save raw analysis response");
        return std::nullopt;
    }
    if (!saveCache(model.document.fileHash, *finalAnalysis)) {
        if (!rawResponse.empty()) restoreFile(rawPath, oldRaw);
        report(IngestState::Failed, 1.0f, "Unable to save analysis cache");
        return std::nullopt;
    }
    if (rawResponse.empty()) {
        std::error_code removeError;
        std::filesystem::remove(rawPath, removeError);
        if (removeError) {
            restoreFile(analysisPath, oldAnalysis);
            restoreFile(rawPath, oldRaw);
            report(IngestState::Failed, 1.0f, "Unable to replace the analysis cache");
            return std::nullopt;
        }
    }
    report(IngestState::Ingested, 1.0f, "Done");
    return finalAnalysis;
}

std::optional<PaperAnalysis> PaperIngestor::cachedAnalysis(const DocumentId&,
                                                           const std::string& fileHash) const {
    if (fileHash.empty()) return std::nullopt;
    std::ifstream f(cacheDirFor(fileHash) + "/analysis.json");
    if (!f) return std::nullopt;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string error;
    return PaperAnalysis::parse(raw, error);
}

void PaperIngestor::clearAnalysis(const std::string& fileHash) const {
    if (fileHash.empty()) return;
    std::error_code ec;
    std::filesystem::remove(cacheDirFor(fileHash) + "/analysis.json", ec);
    clearRawResponse(fileHash);
}

bool PaperIngestor::saveCache(const std::string& fileHash, const PaperAnalysis& analysis) const {
    if (fileHash.empty()) return false;
    std::error_code ec;
    const std::filesystem::path dir = cacheDirFor(fileHash);
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    return writeAtomically(dir / "analysis.json", json::serialize(analysis.toJson()));
}

bool PaperIngestor::saveAnalysis(const std::string& fileHash,
                                 const PaperAnalysis& analysis) const {
    if (fileHash.empty() || !analysis.usable()) return false;
    return saveCache(fileHash, analysis);
}

bool PaperIngestor::normalizeAnalysis(PaperAnalysis& analysis, const DocumentModel& model,
                                      std::string* error) const {
    normalizeForModel(analysis, model);
    if (analysis.usable()) return true;
    if (error) *error = "analysis has no grounded content for this document";
    return false;
}

bool PaperIngestor::saveRawResponse(const std::string& fileHash, const std::string& text) const {
    if (fileHash.empty() || text.empty()) return false;
    std::error_code ec;
    const std::filesystem::path dir = cacheDirFor(fileHash);
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    return writeAtomically(dir / "response.txt", text);
}

std::string PaperIngestor::loadRawResponse(const std::string& fileHash) const {
    if (fileHash.empty()) return {};
    std::ifstream f(cacheDirFor(fileHash) + "/response.txt");
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void PaperIngestor::clearRawResponse(const std::string& fileHash) const {
    if (fileHash.empty()) return;
    std::error_code ec;
    std::filesystem::remove(cacheDirFor(fileHash) + "/response.txt", ec);
}

} // namespace reader
