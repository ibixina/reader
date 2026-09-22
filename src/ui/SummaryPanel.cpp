#include "ui/SummaryPanel.h"
#include "analysis/PaperAnalysis.h"
#include "app/Application.h"
#include <QComboBox>
#include <QHash>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace {

std::string firstBlockForType(const reader::Application* app, const std::string& type) {
    if (!app || !app->analysis) return {};
    for (const auto& annotation : app->analysis->annotations)
        if (annotation.type == type && app->model.findBlock(annotation.blockId))
            return annotation.blockId;
    return app->model.blocks.empty() ? std::string{} : app->model.blocks.front().id;
}

QString sourceSuffix(const std::string& blockId) {
    if (blockId.empty()) return {};
    return QString(" <a href=\"block:%1\">[source]</a>")
        .arg(QString::fromStdString(blockId).toHtmlEscaped());
}

} // namespace

SummaryPanel::SummaryPanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    depth_ = new QComboBox(this);
    depth_->setObjectName("summaryDepth");
    depth_->addItems({"Quick", "Detailed", "Technical"});
    sectionDepth_ = new QComboBox(this);
    sectionDepth_->setObjectName("sectionSummaryDepth");
    sectionDepth_->addItems({"Sections off", "One sentence", "Short", "Detailed"});
    const auto level = QString::fromStdString(app_->state.settings.sectionSummaryLevel);
    const QHash<QString, QString> labels{{"off", "Sections off"},
                                         {"one_sentence", "One sentence"},
                                         {"short", "Short"},
                                         {"detailed", "Detailed"}};
    int levelIndex = sectionDepth_->findText(labels.value(level, "Short"), Qt::MatchFixedString);
    if (levelIndex >= 0) sectionDepth_->setCurrentIndex(levelIndex);
    view_ = new QTextBrowser(this);
    view_->setObjectName("summaryView");
    layout->addWidget(depth_);
    layout->addWidget(sectionDepth_);
    layout->addWidget(view_, 1);
    connect(depth_, &QComboBox::currentTextChanged, this, [this] { rebuild(); });
    connect(sectionDepth_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        if (text == "Sections off") app_->state.settings.sectionSummaryLevel = "off";
        else if (text == "One sentence") app_->state.settings.sectionSummaryLevel = "one_sentence";
        else if (text == "Detailed") app_->state.settings.sectionSummaryLevel = "detailed";
        else app_->state.settings.sectionSummaryLevel = "short";
        rebuild();
    });
    connect(view_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        const QString target = url.toString();
        if (target.startsWith("toggle:")) {
            const std::string id = target.mid(7).toStdString();
            if (!expandedSections_.insert(id).second) expandedSections_.erase(id);
            rebuild();
        } else if (target.startsWith("section:")) {
            const auto* section = app_->model.findSection(target.mid(8).toStdString());
            if (section) emit sourceClicked(reader::anchorForSection(app_->model, *section));
        } else if (target.startsWith("block:")) {
            const auto* block = app_->model.findBlock(target.mid(6).toStdString());
            if (block) emit sourceClicked(reader::anchorForBlock(app_->model, *block));
        } else if (target.startsWith("equation:")) {
            const std::string id = target.mid(9).toStdString();
            for (const auto& equation : app_->model.equations)
                if (equation.id == id) {
                    emit sourceClicked(reader::anchorForEquation(app_->model, equation));
                    break;
                }
        }
    });
}

void SummaryPanel::rebuild() {
    if (!app_->analysis) {
        view_->setText("Ingest the paper to generate a structured summary.");
        return;
    }
    const auto& ov = app_->analysis->overview;
    QString depth = depth_->currentText();
    QString html;
    if (app_->analysis->meta.provider == "local-extractive")
        html += "<p><b>Local extractive analysis</b><br>Built on this device from grounded paper passages.</p><hr>";
    auto section = [&](const QString& title, const QString& body,
                       const std::string& sourceBlock = std::string{}) {
        if (body.trimmed().isEmpty()) return;
        html += "<h3>" + title + "</h3><p>" + body.toHtmlEscaped() +
                sourceSuffix(sourceBlock) + "</p>";
    };
    section("Research Question", QString::fromStdString(ov.researchQuestion),
            firstBlockForType(app_, "claim"));
    section("Core Idea", QString::fromStdString(ov.mainIdea),
            firstBlockForType(app_, "key_idea"));
    section("Main Contribution", QString::fromStdString(ov.mainContribution),
            firstBlockForType(app_, "claim"));
    if (depth != "Quick" && app_->state.settings.showMethods) {
        const auto methodSource = firstBlockForType(app_, "method");
        section("Method", QString::fromStdString(ov.method), methodSource);
        section("Architecture", QString::fromStdString(ov.architecture), methodSource);
        section("Experimental Setup", QString::fromStdString(ov.setup), methodSource);
    }
    if (depth != "Quick" && app_->state.settings.showResults) {
        const auto resultSource = firstBlockForType(app_, "result");
        QString results = "<h3>Main Results</h3><ul>";
        for (const auto& result : ov.mainResults)
            results += "<li>" + QString::fromStdString(result).toHtmlEscaped() +
                       sourceSuffix(resultSource) + "</li>";
        results += "</ul>";
        if (!ov.mainResults.empty()) html += results;
    }
    if (depth == "Technical") {
        QString equations = "<h3>Key Equations</h3><ul>";
        for (const auto& equation : app_->analysis->equations) {
            const QString id = QString::fromStdString(equation.equationId);
            equations += "<li>" +
                         QString::fromStdString(equation.equationId + ": " + equation.purpose)
                             .toHtmlEscaped() +
                         " <a href=\"equation:" + id.toHtmlEscaped() + "\">[source]</a></li>";
        }
        equations += "</ul>";
        if (!app_->analysis->equations.empty()) html += equations;

        QString assumptions = "<h3>Assumptions and Parameters</h3><ul>";
        bool hasAssumptions = false;
        for (const auto& annotation : app_->analysis->annotations) {
            if (annotation.type != "assumption" && annotation.type != "method") continue;
            const auto* block = app_->model.findBlock(annotation.blockId);
            if (!block) continue;
            assumptions += "<li>" + QString::fromStdString(block->text).left(500).toHtmlEscaped() +
                           sourceSuffix(block->id) + "</li>";
            hasAssumptions = true;
        }
        assumptions += "</ul>";
        if (hasAssumptions) html += assumptions;

        if (!ov.mainResults.empty())
            section("Metrics and Experimental Details", QString::fromStdString(ov.setup),
                    firstBlockForType(app_, "result"));
    }
    section("Takeaway", QString::fromStdString(ov.takeaway),
            firstBlockForType(app_, "result"));
    if (app_->state.settings.showLimitations && !ov.limitations.empty()) {
        const auto source = firstBlockForType(app_, "limitation");
        html += "<h3>Limitations</h3><ul>";
        for (const auto& limitation : ov.limitations)
            html += "<li>" + QString::fromStdString(limitation).toHtmlEscaped() +
                    sourceSuffix(source) + "</li>";
        html += "</ul>";
    }
    if (depth != "Quick" && app_->state.settings.sectionSummaryLevel != "off") {
        for (auto& s : app_->analysis->sections) {
            const reader::Section* sec = app_->model.findSection(s.sectionId);
            QString title = sec ? QString::fromStdString(sec->title) : QString::fromStdString(s.sectionId);
            const bool detailed = app_->state.settings.sectionSummaryLevel == "detailed" ||
                                  depth == "Technical";
            QString body = detailed ? QString::fromStdString(s.summaryDetailed)
                                     : QString::fromStdString(s.summaryShort);
            if (app_->state.settings.sectionSummaryLevel == "one_sentence")
                body = body.section(QRegularExpression("[.!?]"), 0).trimmed();
            if (body.trimmed().isEmpty()) continue;
            const QString id = QString::fromStdString(s.sectionId);
            const bool expanded = expandedSections_.contains(s.sectionId);
            html += "<h3><a href=\"toggle:" + id.toHtmlEscaped() + "\">" +
                    (expanded ? "[-]" : "[+]") + "</a> <a href=\"section:" +
                    id.toHtmlEscaped() + "\">" + title.toHtmlEscaped() + "</a></h3>";
            if (expanded)
                html += "<p>" + body.toHtmlEscaped() +
                        (sec ? sourceSuffix(sec->blocks.empty() ? std::string{}
                                                               : sec->blocks.front())
                             : QString{}) +
                        "</p>";
        }
    }
    if (html.isEmpty()) {
        // Never show a blank page after ingest: fall back to the top
        // annotated passages with their source blocks.
        QString passages;
        int shown = 0;
        for (auto& a : app_->analysis->annotations) {
            if (shown >= 12) break;
            const reader::TextBlock* b = app_->model.findBlock(a.blockId);
            if (!b) continue;
            QString snippet = QString::fromStdString(b->text).left(280);
            if (snippet.trimmed().isEmpty()) continue;
            passages += "<p><b>[" + QString::fromStdString(a.type) + "]</b> " +
                        snippet.toHtmlEscaped() + "<br><i>" +
                        QString::fromStdString(a.reason).toHtmlEscaped() + "</i>" +
                        sourceSuffix(b->id) + "</p>";
            ++shown;
        }
        if (!passages.isEmpty()) html = "<h3>Key Passages</h3>" + passages;
    }
    if (html.isEmpty())
        html = "<p>Ingest produced no readable summary for this document.</p>";
    view_->setHtml(html);
}
