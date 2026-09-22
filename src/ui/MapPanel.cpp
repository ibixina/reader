#include "ui/MapPanel.h"
#include "analysis/PaperAnalysis.h"
#include "app/Application.h"
#include <QGraphicsEllipseItem>
#include <QBrush>
#include <QColor>
#include <QGraphicsLineItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <cmath>
#include <numbers>
#include <unordered_map>

MapPanel::MapPanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    scene_ = new QGraphicsScene(this);
    view_ = new QGraphicsView(scene_, this);
    view_->setObjectName("conceptMapView");
    detail_ = new QLabel("Select a concept node.", this);
    detail_->setObjectName("conceptDetail");
    detail_->setWordWrap(true);
    detail_->setTextFormat(Qt::RichText);
    detail_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    detail_->setOpenExternalLinks(false);
    auto* actions = new QHBoxLayout();
    ask_ = new QPushButton("Ask AI", this);
    jump_ = new QPushButton("Jump to source", this);
    ask_->setEnabled(false);
    jump_->setEnabled(false);
    actions->addWidget(ask_);
    actions->addWidget(jump_);
    layout->addWidget(view_, 1);
    layout->addWidget(detail_);
    layout->addLayout(actions);
    connect(ask_, &QPushButton::clicked, this, [this] {
        if (!selectedConcept_.isEmpty()) emit askAbout(selectedConcept_);
    });
    connect(jump_, &QPushButton::clicked, this, [this] {
        if (!selectedConcept_.isEmpty()) emit jumpToSource(selectedConcept_);
    });
    connect(detail_, &QLabel::linkActivated, this, [this](const QString& href) {
        if (!href.startsWith("block:")) return;
        if (const auto* block = app_->model.findBlock(href.mid(6).toStdString()))
            emit sourceActivated(reader::anchorForBlock(app_->model, *block));
    });
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this] {
        selectedConcept_.clear();
        for (QGraphicsItem* item : scene_->selectedItems()) {
            const QString id = item->data(0).toString();
            if (id.isEmpty()) continue;
            selectedConcept_ = id;
            if (app_->analysis) {
                for (const auto& c : app_->analysis->concepts) {
                    if (c.id == id.toStdString()) {
                        QString html = "<b>" + QString::fromStdString(c.name).toHtmlEscaped() +
                                       "</b><br>" +
                                       QString::fromStdString(c.description).toHtmlEscaped();
                        if (!c.sources.empty()) html += "<br><br><b>Appears in:</b> ";
                        for (std::size_t i = 0; i < c.sources.size(); ++i) {
                            const auto* block = app_->model.findBlock(c.sources[i]);
                            if (!block) continue;
                            if (i) html += " · ";
                            html += QString("<a href=\"block:%1\">§%2 · p.%3</a>")
                                        .arg(QString::fromStdString(block->id).toHtmlEscaped())
                                        .arg(QString::fromStdString(
                                            app_->model.sectionForPage(block->page)
                                                ? app_->model.sectionForPage(block->page)->title
                                                : "Source").toHtmlEscaped())
                                        .arg(block->page + 1);
                        }
                        detail_->setText(html);
                        break;
                    }
                }
            }
            break;
        }
        const bool enabled = !selectedConcept_.isEmpty();
        ask_->setEnabled(enabled);
        jump_->setEnabled(enabled);
    });
}

MapPanel::~MapPanel() {
    // QGraphicsScene emits selectionChanged while selected items are removed
    // during child destruction. The handler queries the scene and touches
    // sibling widgets, which are already in QObject teardown at that point.
    // Disconnect before QWidget starts deleting children.
    if (scene_) disconnect(scene_, nullptr, this, nullptr);
}

void MapPanel::rebuild() {
    scene_->clear();
    selectedConcept_.clear();
    ask_->setEnabled(false);
    jump_->setEnabled(false);
    if (!app_->analysis) {
        scene_->addText("Ingest the paper to populate the concept map.");
        return;
    }
    const auto& concepts = app_->analysis->concepts;
    const auto& edges = app_->analysis->relationships;
    std::unordered_map<std::string, QPointF> pos;
    double cx = 0, cy = 0, r = 160 + 12 * concepts.size();
    for (std::size_t i = 0; i < concepts.size(); ++i) {
        double a = 2 * std::numbers::pi * i / std::max<std::size_t>(1, concepts.size());
        pos[concepts[i].id] = {cx + r * std::cos(a), cy + r * std::sin(a)};
    }
    for (const auto& e : edges) {
        auto it1 = pos.find(e.source), it2 = pos.find(e.target);
        if (it1 == pos.end() || it2 == pos.end()) continue;
        const QLineF edge(it1->second, it2->second);
        auto* line = scene_->addLine(edge, QPen(QColor(90, 100, 120), 1.5));
        line->setToolTip(QString::fromStdString(e.relation));
        const double angle = std::atan2(-edge.dy(), edge.dx());
        const QPointF tip = edge.pointAt(0.78);
        constexpr double arrow = 9.0;
        QPolygonF head;
        head << tip
             << tip - QPointF(std::cos(angle + std::numbers::pi / 6) * arrow,
                              -std::sin(angle + std::numbers::pi / 6) * arrow)
             << tip - QPointF(std::cos(angle - std::numbers::pi / 6) * arrow,
                              -std::sin(angle - std::numbers::pi / 6) * arrow);
        scene_->addPolygon(head, QPen(Qt::NoPen), QBrush(QColor(90, 100, 120)));
        if (!e.relation.empty()) {
            auto* label = scene_->addSimpleText(QString::fromStdString(e.relation));
            label->setBrush(QBrush(QColor(60, 70, 90)));
            const QRectF labelBounds = label->boundingRect();
            label->setPos(edge.center() - QPointF(labelBounds.width() / 2,
                                                   labelBounds.height() / 2));
        }
    }
    for (const auto& c : concepts) {
        QPointF p = pos[c.id];
        auto* node = scene_->addEllipse(p.x() - 46, p.y() - 22, 92, 44);
        node->setData(0, QString::fromStdString(c.id));
        node->setFlag(QGraphicsItem::ItemIsSelectable);
        auto* label = scene_->addSimpleText(QString::fromStdString(c.name).left(16));
        label->setPos(p.x() - 40, p.y() - 10);
    }
    fitGraph();
}

void MapPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fitGraph();
}

void MapPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    fitGraph();
}

void MapPanel::fitGraph() {
    if (!view_ || !scene_ || scene_->items().isEmpty()) return;
    view_->fitInView(scene_->itemsBoundingRect().adjusted(-12, -12, 12, 12),
                     Qt::KeepAspectRatio);
}
