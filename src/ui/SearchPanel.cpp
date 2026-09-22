#include "ui/SearchPanel.h"
#include "app/Application.h"
#include "ui/PdfView.h"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QAbstractItemView>
#include <QVBoxLayout>

SearchPanel::SearchPanel(reader::Application* app, PdfView* view, QWidget* parent)
    : QWidget(parent), app_(app), view_(view) {
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout();
    mode_ = new QComboBox(this);
    mode_->setObjectName("searchMode");
    mode_->addItems({"Literal", "Semantic (configured)"});
    query_ = new QLineEdit(this);
    query_->setObjectName("searchQuery");
    query_->setPlaceholderText("Search this paper…");
    search_ = new QPushButton("Search", this);
    search_->setObjectName("searchButton");
    row->addWidget(mode_);
    row->addWidget(query_, 1);
    row->addWidget(search_);
    layout->addLayout(row);
    auto* actions = new QHBoxLayout();
    configure_ = new QPushButton("Embedding settings", this);
    build_ = new QPushButton("Build semantic index", this);
    status_ = new QLabel("Literal search is available offline.", this);
    status_->setWordWrap(true);
    actions->addWidget(configure_);
    actions->addWidget(build_);
    layout->addLayout(actions);
    layout->addWidget(status_);
    results_ = new QListWidget(this);
    results_->setObjectName("searchResults");
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(results_, 1);
    // Semantic queries can call a remote embedding endpoint. They only run
    // after an explicit Search/Enter action, never for each keystroke.
    connect(query_, &QLineEdit::returnPressed, this,
            [this] { search(query_->text()); });
    connect(search_, &QPushButton::clicked, this,
            [this] { search(query_->text()); });
    connect(mode_, &QComboBox::currentTextChanged, this,
            [this] {
                ++queryGeneration_;
                results_->clear();
                anchors_.clear();
                status_->setText(mode_->currentText() == "Literal"
                                     ? "Literal search is available offline. Press Enter to search."
                                     : "Semantic search uses the configured embedding provider. Press Enter to search.");
            });
    connect(configure_, &QPushButton::clicked, this, [this] {
        QSettings settings;
        bool ok = false;
        const QString base = QInputDialog::getText(
            this, "Embedding endpoint", "OpenAI-compatible base URL:", QLineEdit::Normal,
            settings.value("reader/embeddingBaseUrl").toString(), &ok);
        if (!ok) return;
        const QString model = QInputDialog::getText(
            this, "Embedding model", "Model name:", QLineEdit::Normal,
            settings.value("reader/embeddingModel").toString(), &ok);
        if (!ok) return;
        const int dimension = QInputDialog::getInt(
            this, "Embedding dimension", "Vector dimension:",
            settings.value("reader/embeddingDimension", 0).toInt(), 1, 1000000, 1, &ok);
        if (!ok) return;
        const QString key = QInputDialog::getText(
            this, "Embedding API key", "API key (optional):", QLineEdit::Password,
            settings.value("reader/embeddingApiKey").toString(), &ok);
        if (!ok) return;
        settings.setValue("reader/embeddingBaseUrl", base);
        settings.setValue("reader/embeddingModel", model);
        settings.setValue("reader/embeddingDimension", dimension);
        settings.setValue("reader/embeddingApiKey", key);
        reader::EmbeddingProviderQtConfig config{base.toStdString(), key.toStdString(),
                                                 model.toStdString(),
                                                 static_cast<std::size_t>(dimension), 120000};
        view_->configureSemantic(config);
        status_->setText("Embedding settings saved. Build the semantic index explicitly.");
    });
    connect(build_, &QPushButton::clicked, this, [this] {
        QSettings settings;
        reader::EmbeddingProviderQtConfig config{
            settings.value("reader/embeddingBaseUrl").toString().toStdString(),
            settings.value("reader/embeddingApiKey").toString().toStdString(),
            settings.value("reader/embeddingModel").toString().toStdString(),
            static_cast<std::size_t>(settings.value("reader/embeddingDimension", 0).toInt()),
            120000};
        view_->configureSemantic(config);
        build_->setEnabled(false);
        status_->setText(app_->state.settings.storeEmbeddingsLocally
                             ? "Building semantic index… it will be cached locally."
                             : "Building an in-memory semantic index… it will not be stored.");
        QPointer<SearchPanel> guard(this);
        view_->buildSemanticIndexAsync(10000, [guard](bool ok, const QString& error) {
            if (!guard) return;
            guard->build_->setEnabled(true);
            guard->status_->setText(ok ? "Semantic index ready." : error);
            if (!guard->query_->text().trimmed().isEmpty())
                guard->search(guard->query_->text());
        });
    });
    connect(results_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const int index = item->data(Qt::UserRole).toInt();
        if (index >= 0 && index < static_cast<int>(anchors_.size()))
            emit anchorActivated(anchors_[index]);
    });
}

void SearchPanel::focusQuery() {
    query_->setFocus(Qt::OtherFocusReason);
    query_->selectAll();
}

void SearchPanel::search(const QString& query) {
    ++queryGeneration_;
    rebuildResults(query.trimmed());
}

void SearchPanel::rebuildResults(const QString& query) {
    results_->clear();
    anchors_.clear();
    if (query.isEmpty() || !app_ || !view_) return;
    if (mode_->currentText() == "Literal") {
        for (const auto& hit : view_->searchLiteral(query.toStdString(), 100)) {
            auto* item = new QListWidgetItem(
                QString("p.%1  %2").arg(hit.anchor.page + 1)
                    .arg(QString::fromStdString(hit.snippet).trimmed()),
                results_);
            anchors_.push_back(hit.anchor);
            item->setData(Qt::UserRole, static_cast<int>(anchors_.size() - 1));
        }
    } else {
        if (!view_->semanticReady()) {
            status_->setText("Configure and build the semantic index before using semantic search.");
            return;
        }
        const auto generation = queryGeneration_;
        QPointer<SearchPanel> guard(this);
        view_->searchSemanticAsync(
            query.toStdString(), 100,
            [guard, generation](std::vector<reader::VectorIndex::Hit> hits, const QString& error) {
                if (!guard || generation != guard->queryGeneration_) return;
                if (!error.isEmpty()) {
                    guard->status_->setText(error);
                    return;
                }
                guard->status_->setText("Semantic results");
                for (const auto& hit : hits) {
            auto* item = new QListWidgetItem(
                QString("p.%1  [%2] %3")
                    .arg(hit.anchor.page + 1)
                    .arg(hit.score, 0, 'f', 3)
                    .arg(QString::fromStdString(hit.snippet).trimmed()),
                guard->results_);
            guard->anchors_.push_back(hit.anchor);
            item->setData(Qt::UserRole, static_cast<int>(guard->anchors_.size() - 1));
                }
            });
    }
}
