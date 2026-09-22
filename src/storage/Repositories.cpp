#include "storage/Repositories.h"
#include "core/Json.h"
#include <algorithm>
#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <sqlite3.h>

namespace reader {
namespace {

bool text(sqlite3_stmt* st, int index, const std::string& value) {
    return sqlite3_bind_text(st, index, value.data(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool optionalText(sqlite3_stmt* st, int index, const std::optional<std::string>& value) {
    if (!value) return sqlite3_bind_null(st, index) == SQLITE_OK;
    return text(st, index, *value);
}

bool integer(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    return sqlite3_bind_int64(st, index, value) == SQLITE_OK;
}

bool real(sqlite3_stmt* st, int index, double value) {
    return sqlite3_bind_double(st, index, value) == SQLITE_OK;
}

bool blob(sqlite3_stmt* st, int index, const std::vector<std::uint8_t>& value) {
    return sqlite3_bind_blob(st, index, value.data(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string columnText(sqlite3_stmt* st, int index) {
    const auto* value = sqlite3_column_text(st, index);
    if (!value) return {};
    return {reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(sqlite3_column_bytes(st, index))};
}

std::optional<std::string> columnOptionalText(sqlite3_stmt* st, int index) {
    const auto* value = sqlite3_column_text(st, index);
    if (!value) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(sqlite3_column_bytes(st, index)));
}

std::vector<std::uint8_t> columnBlob(sqlite3_stmt* st, int index) {
    const auto* value = static_cast<const std::uint8_t*>(sqlite3_column_blob(st, index));
    const int size = sqlite3_column_bytes(st, index);
    if (!value || size <= 0) return {};
    return {value, value + size};
}

std::string tableRowsJson(const std::vector<std::vector<std::string>>& rows) {
    json::Array encoded;
    for (const auto& row : rows) {
        json::Array values;
        for (const auto& value : row) values.emplace_back(value);
        encoded.emplace_back(std::move(values));
    }
    return json::serialize(encoded);
}

std::vector<std::vector<std::string>> parseTableRows(const std::string& encoded) {
    if (encoded.empty()) return {};
    try {
        const auto value = json::parse(encoded);
        if (!value.isArray()) return {};
        std::vector<std::vector<std::string>> rows;
        for (const auto& row : value.asArray()) {
            if (!row.isArray()) continue;
            std::vector<std::string> values;
            for (const auto& cell : row.asArray()) values.push_back(cell.asString());
            rows.push_back(std::move(values));
        }
        return rows;
    } catch (const json::ParseError&) {
        return {};
    }
}

json::Value anchorJson(const DocumentAnchor& anchor) {
    return json::Object{{"document", anchor.document},
                        {"page", anchor.page},
                        {"x", anchor.bounds.x},
                        {"y", anchor.bounds.y},
                        {"w", anchor.bounds.width},
                        {"h", anchor.bounds.height},
                        {"text", anchor.anchorText},
                        {"section", anchor.section ? *anchor.section : ""},
                        {"block", anchor.block ? *anchor.block : ""},
                        {"objectType", anchor.objectType},
                        {"objectId", anchor.objectId}};
}

std::string relatedSourcesJson(const std::vector<DocumentAnchor>& anchors) {
    json::Array encoded;
    for (const auto& anchor : anchors) encoded.emplace_back(anchorJson(anchor));
    return json::serialize(encoded);
}

std::vector<DocumentAnchor> parseRelatedSources(const std::string& encoded) {
    if (encoded.empty()) return {};
    try {
        const auto value = json::parse(encoded);
        if (!value.isArray()) return {};
        std::vector<DocumentAnchor> anchors;
        for (const auto& item : value.asArray()) {
            if (!item.isObject()) continue;
            DocumentAnchor anchor;
            anchor.document = item.at("document").asString();
            anchor.page = static_cast<int>(item.at("page").asNumber());
            anchor.bounds = {static_cast<float>(item.at("x").asNumber()),
                             static_cast<float>(item.at("y").asNumber()),
                             static_cast<float>(item.at("w").asNumber()),
                             static_cast<float>(item.at("h").asNumber())};
            anchor.anchorText = item.at("text").asString();
            const auto section = item.at("section").asString();
            const auto block = item.at("block").asString();
            if (!section.empty()) anchor.section = section;
            if (!block.empty()) anchor.block = block;
            anchor.objectType = item.at("objectType").asString();
            anchor.objectId = item.at("objectId").asString();
            anchors.push_back(std::move(anchor));
        }
        return anchors;
    } catch (const json::ParseError&) {
        return {};
    }
}

std::string citationMetadataJson(const Citation& citation) {
    return json::serialize(json::Object{{"title", citation.title},
                                        {"doi", citation.doi},
                                        {"reason", citation.reason}});
}

std::string referenceTypeCode(ReferenceType type) {
    switch (type) {
        case ReferenceType::TextSelection: return "text_selection";
        case ReferenceType::Paragraph: return "paragraph";
        case ReferenceType::Section: return "section";
        case ReferenceType::Page: return "page";
        case ReferenceType::Equation: return "equation";
        case ReferenceType::Figure: return "figure";
        case ReferenceType::Table: return "table";
        case ReferenceType::Citation: return "citation";
        case ReferenceType::Concept: return "concept";
    }
    return "text_selection";
}

ReferenceType referenceTypeFromCode(const std::string& code) {
    if (code == "paragraph") return ReferenceType::Paragraph;
    if (code == "section") return ReferenceType::Section;
    if (code == "page") return ReferenceType::Page;
    if (code == "equation") return ReferenceType::Equation;
    if (code == "figure") return ReferenceType::Figure;
    if (code == "table") return ReferenceType::Table;
    if (code == "citation") return ReferenceType::Citation;
    if (code == "concept") return ReferenceType::Concept;
    return ReferenceType::TextSelection;
}

void fillAnchor(DocumentAnchor& anchor, sqlite3_stmt* st, int docIndex, int pageIndex,
                int xIndex, int yIndex, int wIndex, int hIndex, int textIndex,
                int sectionIndex, int blockIndex) {
    anchor.document = columnText(st, docIndex);
    anchor.page = sqlite3_column_int(st, pageIndex);
    anchor.bounds = {static_cast<float>(sqlite3_column_double(st, xIndex)),
                     static_cast<float>(sqlite3_column_double(st, yIndex)),
                     static_cast<float>(sqlite3_column_double(st, wIndex)),
                     static_cast<float>(sqlite3_column_double(st, hIndex))};
    anchor.anchorText = columnText(st, textIndex);
    anchor.section = columnOptionalText(st, sectionIndex);
    anchor.block = columnOptionalText(st, blockIndex);
}

std::string nextConversationId() {
    std::random_device random;
    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    std::ostringstream id;
    id << "conv_" << std::hex << std::setfill('0');
    for (const auto byte : bytes) id << std::setw(2) << static_cast<unsigned>(byte);
    return id.str();
}

std::string joinStrings(const std::vector<std::string>& values) {
    std::string joined;
    for (const auto& value : values) {
        if (!joined.empty()) joined += ';';
        joined += value;
    }
    return joined;
}

std::vector<std::string> splitStrings(const std::string& value) {
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(';', start);
        values.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (values.size() == 1 && values.front().empty()) values.clear();
    return values;
}

} // namespace

bool AnnotationRepository::saveNote(const DocumentId& doc, const Note& note) {
    const auto& a = note.anchor;
    return db_->execPrepared(
        "INSERT OR REPLACE INTO notes(id,document_id,page,x,y,w,h,anchor_text,text,created_at,"
        "updated_at,section_id,block_id,object_type,object_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        [&](sqlite3_stmt* st) {
            return text(st, 1, note.id) && text(st, 2, doc) && integer(st, 3, a.page) &&
                   real(st, 4, a.bounds.x) && real(st, 5, a.bounds.y) &&
                   real(st, 6, a.bounds.width) && real(st, 7, a.bounds.height) &&
                   text(st, 8, a.anchorText) && text(st, 9, note.text) &&
                   integer(st, 10, note.createdAt) && integer(st, 11, note.updatedAt) &&
                   optionalText(st, 12, a.section) && optionalText(st, 13, a.block) &&
                   text(st, 14, a.objectType) && text(st, 15, a.objectId);
        });
}

std::vector<Note> AnnotationRepository::notesFor(const DocumentId& doc) const {
    std::vector<Note> out;
    db_->queryPrepared(
        "SELECT id,document_id,page,x,y,w,h,anchor_text,text,created_at,updated_at,section_id,"
        "block_id,object_type,object_id FROM notes WHERE document_id=? ORDER BY created_at,id",
        [&](sqlite3_stmt* st) { return text(st, 1, doc); },
        [&](sqlite3_stmt* st) {
            Note n;
            n.id = columnText(st, 0);
            fillAnchor(n.anchor, st, 1, 2, 3, 4, 5, 6, 7, 11, 12);
            n.text = columnText(st, 8);
            n.createdAt = sqlite3_column_int64(st, 9);
            n.updatedAt = sqlite3_column_int64(st, 10);
            n.anchor.objectType = columnText(st, 13);
            n.anchor.objectId = columnText(st, 14);
            out.push_back(std::move(n));
        });
    return out;
}

bool AnnotationRepository::deleteNote(const DocumentId& doc, const NoteId& id) {
    return db_->execPrepared("DELETE FROM notes WHERE document_id=? AND id=?",
                             [&](sqlite3_stmt* st) {
                                 return text(st, 1, doc) && text(st, 2, id);
                             });
}

bool AnnotationRepository::saveAnnotation(const DocumentId& doc, const UserAnnotation& ann) {
    const auto& a = ann.anchor;
    return db_->execPrepared(
        "INSERT INTO annotations(id,document_id,page,x,y,w,h,anchor_text,kind,color,"
        "created_at,section_id,block_id,object_type,object_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET document_id=excluded.document_id,page=excluded.page,"
        "x=excluded.x,y=excluded.y,w=excluded.w,h=excluded.h,anchor_text=excluded.anchor_text,"
        "kind=excluded.kind,color=excluded.color,section_id=excluded.section_id,"
        "block_id=excluded.block_id,object_type=excluded.object_type,object_id=excluded.object_id",
        [&](sqlite3_stmt* st) {
            return text(st, 1, ann.id) && text(st, 2, doc) && integer(st, 3, a.page) &&
                   real(st, 4, a.bounds.x) && real(st, 5, a.bounds.y) &&
                   real(st, 6, a.bounds.width) && real(st, 7, a.bounds.height) &&
                   text(st, 8, a.anchorText) && text(st, 9, ann.kind) &&
                   text(st, 10, ann.color) && integer(st, 11, nowMs()) &&
                   optionalText(st, 12, a.section) && optionalText(st, 13, a.block) &&
                   text(st, 14, a.objectType) && text(st, 15, a.objectId);
        });
}

std::vector<UserAnnotation> AnnotationRepository::annotationsFor(const DocumentId& doc) const {
    std::vector<UserAnnotation> out;
    db_->queryPrepared(
        "SELECT id,document_id,page,x,y,w,h,anchor_text,kind,color,section_id,block_id,"
        "object_type,object_id "
        "FROM annotations WHERE document_id=? ORDER BY created_at,id",
        [&](sqlite3_stmt* st) { return text(st, 1, doc); },
        [&](sqlite3_stmt* st) {
            UserAnnotation a;
            a.id = columnText(st, 0);
            fillAnchor(a.anchor, st, 1, 2, 3, 4, 5, 6, 7, 10, 11);
            a.kind = columnText(st, 8);
            a.color = columnText(st, 9);
            a.anchor.objectType = columnText(st, 12);
            a.anchor.objectId = columnText(st, 13);
            out.push_back(std::move(a));
        });
    return out;
}

bool AnnotationRepository::deleteAnnotation(const DocumentId& doc, const AnnotationId& id) {
    return db_->execPrepared("DELETE FROM annotations WHERE document_id=? AND id=?",
                             [&](sqlite3_stmt* st) {
                                 return text(st, 1, doc) && text(st, 2, id);
                             });
}

bool DocumentRepository::saveDocument(const Document& doc) {
    return db_->execPrepared(
        "INSERT INTO documents(id,path,hash,title,authors,abstract_text,keywords,page_count,last_opened,"
        "extraction_version,extraction_complete) VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET path=excluded.path,"
        "hash=excluded.hash,title=excluded.title,authors=excluded.authors,"
        "abstract_text=excluded.abstract_text,keywords=excluded.keywords,"
        "page_count=excluded.page_count,last_opened=excluded.last_opened,"
        "extraction_version=excluded.extraction_version,extraction_complete=excluded.extraction_complete",
        [&](sqlite3_stmt* st) {
            return text(st, 1, doc.id) && text(st, 2, doc.filePath) &&
                   text(st, 3, doc.fileHash) && text(st, 4, doc.title) &&
                   text(st, 5, joinStrings(doc.authors)) && text(st, 6, doc.abstractText) &&
                   text(st, 7, joinStrings(doc.keywords)) && integer(st, 8, doc.pageCount) &&
                   integer(st, 9, nowMs()) && integer(st, 10, doc.extractionVersion) &&
                   integer(st, 11, doc.extractionComplete ? 1 : 0);
        });
}

std::vector<Document> DocumentRepository::recentDocuments(std::size_t limit) const {
    std::vector<Document> out;
    db_->queryPrepared(
        "SELECT id,path,hash,title,authors,abstract_text,keywords,page_count,"
        "extraction_version,extraction_complete FROM documents "
        "ORDER BY last_opened DESC,id LIMIT ?",
        [&](sqlite3_stmt* st) { return integer(st, 1, static_cast<sqlite3_int64>(limit)); },
        [&](sqlite3_stmt* st) {
            Document document;
            document.id = columnText(st, 0);
            document.filePath = columnText(st, 1);
            document.fileHash = columnText(st, 2);
            document.title = columnText(st, 3);
            document.authors = splitStrings(columnText(st, 4));
            document.abstractText = columnText(st, 5);
            document.keywords = splitStrings(columnText(st, 6));
            document.pageCount = sqlite3_column_int(st, 7);
            document.extractionVersion = sqlite3_column_int(st, 8);
            document.extractionComplete = sqlite3_column_int(st, 9) != 0;
            out.push_back(std::move(document));
        });
    return out;
}

bool DocumentRepository::saveModel(const DocumentModel& model) {
    return db_->transaction([&] {
        Document persisted = model.document;
        persisted.extractionVersion = kExtractionSchemaVersion;
        persisted.extractionComplete = true;
        bool ok = saveDocument(persisted);
        ok = ok && db_->execPrepared("DELETE FROM section_blocks WHERE document_id=?",
                                    [&](sqlite3_stmt* st) { return text(st, 1, model.document.id); });
        ok = ok && db_->execPrepared("DELETE FROM sections WHERE document_id=?",
                                     [&](sqlite3_stmt* st) { return text(st, 1, model.document.id); });
        ok = ok && db_->execPrepared("DELETE FROM blocks WHERE document_id=?",
                                     [&](sqlite3_stmt* st) { return text(st, 1, model.document.id); });
        ok = ok && db_->execPrepared("DELETE FROM document_objects WHERE document_id=?",
                                     [&](sqlite3_stmt* st) { return text(st, 1, model.document.id); });
        ok = ok && db_->execPrepared("DELETE FROM line_spans WHERE document_id=?",
                                     [&](sqlite3_stmt* st) { return text(st, 1, model.document.id); });
        for (const auto& block : model.blocks) {
            ok = ok && db_->execPrepared(
                           "INSERT INTO blocks(id,document_id,page,x,y,w,h,text) VALUES(?,?,?,?,?,?,?,?)",
                           [&](sqlite3_stmt* st) {
                               return text(st, 1, block.id) && text(st, 2, model.document.id) &&
                                      integer(st, 3, block.page) && real(st, 4, block.bounds.x) &&
                                      real(st, 5, block.bounds.y) && real(st, 6, block.bounds.width) &&
                                      real(st, 7, block.bounds.height) && text(st, 8, block.text);
                           });
        }
        for (std::size_t i = 0; i < model.lineSpans.size(); ++i) {
            const auto& span = model.lineSpans[i];
            ok = ok && db_->execPrepared(
                           "INSERT INTO line_spans(document_id,ordinal,page,x,y,w,h,text,font_size,bold) "
                           "VALUES(?,?,?,?,?,?,?,?,?,?)",
                           [&](sqlite3_stmt* st) {
                               return text(st, 1, model.document.id) && integer(st, 2, i) &&
                                      integer(st, 3, span.page) && real(st, 4, span.bounds.x) &&
                                      real(st, 5, span.bounds.y) && real(st, 6, span.bounds.width) &&
                                      real(st, 7, span.bounds.height) && text(st, 8, span.text) &&
                                      real(st, 9, span.fontSize) && integer(st, 10, span.bold ? 1 : 0);
                           });
        }
        for (const auto& section : model.sections) {
            ok = ok && db_->execPrepared(
                           "INSERT INTO sections(id,document_id,title,level,start_page,end_page) "
                           "VALUES(?,?,?,?,?,?)",
                           [&](sqlite3_stmt* st) {
                               return text(st, 1, section.id) && text(st, 2, model.document.id) &&
                                      text(st, 3, section.title) && integer(st, 4, section.level) &&
                                      integer(st, 5, section.startPage) && integer(st, 6, section.endPage);
                           });
            for (std::size_t i = 0; i < section.blocks.size(); ++i) {
                ok = ok && db_->execPrepared(
                               "INSERT INTO section_blocks(document_id,section_id,block_id,ordinal) "
                               "VALUES(?,?,?,?)",
                               [&](sqlite3_stmt* st) {
                                   return text(st, 1, model.document.id) && text(st, 2, section.id) &&
                                          text(st, 3, section.blocks[i]) && integer(st, 4, i);
                               });
            }
        }
        const auto saveObject = [&](const std::string& id, const std::string& type, int page,
                                    const Rect& bounds, const std::string& textValue,
                                    const std::string& caption, const std::string& metadata,
                                    const std::optional<BlockId>& block) {
            return db_->execPrepared(
                "INSERT INTO document_objects(document_id,object_id,object_type,page,x,y,w,h,text,"
                "caption,metadata,block_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                [&](sqlite3_stmt* st) {
                    return text(st, 1, model.document.id) && text(st, 2, id) && text(st, 3, type) &&
                           integer(st, 4, page) && real(st, 5, bounds.x) && real(st, 6, bounds.y) &&
                           real(st, 7, bounds.width) && real(st, 8, bounds.height) &&
                           text(st, 9, textValue) && text(st, 10, caption) && text(st, 11, metadata) &&
                           optionalText(st, 12, block);
                });
        };
        for (const auto& equation : model.equations)
            ok = ok && saveObject(equation.id, "equation", equation.page, equation.bounds,
                                  equation.extractedText, equation.label, equation.latex, std::nullopt);
        for (const auto& figure : model.figures)
            ok = ok && saveObject(figure.id, "figure", figure.page, figure.bounds, {}, figure.caption,
                                  {}, std::nullopt);
        for (const auto& table : model.tables)
            ok = ok && saveObject(table.id, "table", table.page, table.bounds, {}, table.caption,
                                  tableRowsJson(table.rows), std::nullopt);
        for (const auto& citation : model.citations)
            ok = ok && saveObject(citation.id, "citation", citation.page, {}, citation.raw,
                                  citation.label, citationMetadataJson(citation),
                                  citation.block);
        for (const auto& entry : model.bibliography)
            ok = ok && saveObject(entry.id, "bibliography", 0, {}, entry.text, entry.label, entry.doi,
                                  std::nullopt);
        for (const auto& footnote : model.footnotes)
            ok = ok && saveObject(footnote.id, "footnote", footnote.page, {}, footnote.text, {}, {},
                                  std::nullopt);
        return ok;
    });
}

bool DocumentRepository::loadModel(const DocumentId& doc, DocumentModel& model) const {
    return loadModel(doc, {}, model);
}

bool DocumentRepository::loadModel(const DocumentId& doc, const std::string& expectedFileHash,
                                   DocumentModel& model) const {
    DocumentModel loaded;
    return db_->readTransaction([&] {
        bool found = false;
        if (!db_->queryPrepared(
                "SELECT id,path,hash,title,authors,abstract_text,keywords,page_count,"
                "extraction_version,extraction_complete FROM documents WHERE id=?",
                [&](sqlite3_stmt* st) { return text(st, 1, doc); },
                [&](sqlite3_stmt* st) {
                    loaded.document.id = columnText(st, 0);
                    loaded.document.filePath = columnText(st, 1);
                    loaded.document.fileHash = columnText(st, 2);
                    loaded.document.title = columnText(st, 3);
                    loaded.document.authors = splitStrings(columnText(st, 4));
                    loaded.document.abstractText = columnText(st, 5);
                    loaded.document.keywords = splitStrings(columnText(st, 6));
                    loaded.document.pageCount = sqlite3_column_int(st, 7);
                    loaded.document.extractionVersion = sqlite3_column_int(st, 8);
                    loaded.document.extractionComplete = sqlite3_column_int(st, 9) != 0;
                    found = true;
                }) || !found)
            return false;
        if (!loaded.document.extractionComplete ||
            loaded.document.extractionVersion != kExtractionSchemaVersion ||
            (!expectedFileHash.empty() && loaded.document.fileHash != expectedFileHash))
            return false;
        if (!db_->queryPrepared(
                "SELECT id,page,x,y,w,h,text FROM blocks WHERE document_id=? ORDER BY rowid",
                [&](sqlite3_stmt* st) { return text(st, 1, doc); },
                [&](sqlite3_stmt* st) {
                    TextBlock block;
                    block.id = columnText(st, 0);
                    block.page = sqlite3_column_int(st, 1);
                    block.bounds = {static_cast<float>(sqlite3_column_double(st, 2)),
                                    static_cast<float>(sqlite3_column_double(st, 3)),
                                    static_cast<float>(sqlite3_column_double(st, 4)),
                                    static_cast<float>(sqlite3_column_double(st, 5))};
                    block.text = columnText(st, 6);
                    loaded.blocks.push_back(std::move(block));
                }))
            return false;
        if (!db_->queryPrepared(
                "SELECT ordinal,page,x,y,w,h,text,font_size,bold FROM line_spans "
                "WHERE document_id=? ORDER BY ordinal",
                [&](sqlite3_stmt* st) { return text(st, 1, doc); },
                [&](sqlite3_stmt* st) {
                    TextSpan span;
                    span.page = sqlite3_column_int(st, 1);
                    span.bounds = {static_cast<float>(sqlite3_column_double(st, 2)),
                                   static_cast<float>(sqlite3_column_double(st, 3)),
                                   static_cast<float>(sqlite3_column_double(st, 4)),
                                   static_cast<float>(sqlite3_column_double(st, 5))};
                    span.text = columnText(st, 6);
                    span.fontSize = static_cast<float>(sqlite3_column_double(st, 7));
                    span.bold = sqlite3_column_int(st, 8) != 0;
                    loaded.lineSpans.push_back(std::move(span));
                }))
            return false;
        if (!db_->queryPrepared(
                "SELECT id,title,level,start_page,end_page FROM sections WHERE document_id=? "
                "ORDER BY start_page,id",
                [&](sqlite3_stmt* st) { return text(st, 1, doc); },
                [&](sqlite3_stmt* st) {
                    Section section;
                    section.id = columnText(st, 0);
                    section.title = columnText(st, 1);
                    section.level = sqlite3_column_int(st, 2);
                    section.startPage = sqlite3_column_int(st, 3);
                    section.endPage = sqlite3_column_int(st, 4);
                    loaded.sections.push_back(std::move(section));
                }))
            return false;
        for (auto& section : loaded.sections) {
            if (!db_->queryPrepared(
                    "SELECT block_id FROM section_blocks WHERE document_id=? AND section_id=? "
                    "ORDER BY ordinal",
                    [&](sqlite3_stmt* st) {
                        return text(st, 1, doc) && text(st, 2, section.id);
                    },
                    [&](sqlite3_stmt* st) { section.blocks.push_back(columnText(st, 0)); }))
                return false;
        }
        if (!db_->queryPrepared(
                "SELECT object_id,object_type,page,x,y,w,h,text,caption,metadata,block_id "
                "FROM document_objects WHERE document_id=? ORDER BY rowid",
                [&](sqlite3_stmt* st) { return text(st, 1, doc); },
                [&](sqlite3_stmt* st) {
                    const std::string id = columnText(st, 0);
                    const std::string type = columnText(st, 1);
                    const int page = sqlite3_column_int(st, 2);
                    const Rect bounds{static_cast<float>(sqlite3_column_double(st, 3)),
                                      static_cast<float>(sqlite3_column_double(st, 4)),
                                      static_cast<float>(sqlite3_column_double(st, 5)),
                                      static_cast<float>(sqlite3_column_double(st, 6))};
                    const std::string textValue = columnText(st, 7);
                    const std::string caption = columnText(st, 8);
                    const std::string metadata = columnText(st, 9);
                    const auto block = columnOptionalText(st, 10);
                    if (type == "equation")
                        loaded.equations.push_back({id, page, bounds, textValue, metadata, caption});
                    else if (type == "figure")
                        loaded.figures.push_back({id, page, bounds, caption});
                else if (type == "table")
                    loaded.tables.push_back({id, page, bounds, caption, parseTableRows(metadata)});
                    else if (type == "citation") {
                        Citation citation{id, textValue, caption, {}, {}, {}, page, block};
                        try {
                            const auto citationMeta = json::parse(metadata);
                            citation.title = citationMeta.at("title").asString();
                            citation.doi = citationMeta.at("doi").asString();
                            citation.reason = citationMeta.at("reason").asString();
                        } catch (const json::ParseError&) {
                            // Older caches used a newline-delimited payload;
                            // retain the raw value rather than failing load.
                            const auto first = metadata.find('\n');
                            const auto second = first == std::string::npos
                                                    ? std::string::npos
                                                    : metadata.find('\n', first + 1);
                            citation.title = first == std::string::npos
                                                 ? metadata
                                                 : metadata.substr(0, first);
                            if (first != std::string::npos)
                                citation.doi = metadata.substr(first + 1,
                                                               second == std::string::npos
                                                                   ? second
                                                                   : second - first - 1);
                            if (second != std::string::npos)
                                citation.reason = metadata.substr(second + 1);
                        }
                        loaded.citations.push_back(std::move(citation));
                    } else if (type == "bibliography") {
                        loaded.bibliography.push_back({id, caption, textValue, metadata});
                    } else if (type == "footnote") {
                        loaded.footnotes.push_back({id, page, textValue});
                    }
                }))
            return false;
        loaded.rebuildIndex();
        model = std::move(loaded);
        return true;
    });
}

bool DocumentRepository::saveReadingState(const DocumentId& doc, int page, double scrollY,
                                          double zoom) {
    return db_->execPrepared(
        "INSERT OR REPLACE INTO reading_state(document_id,page,scroll_y,zoom,updated_at) "
        "VALUES(?,?,?,?,?)",
        [&](sqlite3_stmt* st) {
            return text(st, 1, doc) && integer(st, 2, page) && real(st, 3, scrollY) &&
                   real(st, 4, zoom) && integer(st, 5, nowMs());
        });
}

bool DocumentRepository::loadReadingState(const DocumentId& doc, int& page, double& scrollY,
                                          double& zoom) const {
    bool found = false;
    const bool queried = db_->queryPrepared(
        "SELECT page,scroll_y,zoom FROM reading_state WHERE document_id=?",
        [&](sqlite3_stmt* st) { return text(st, 1, doc); },
        [&](sqlite3_stmt* st) {
            page = sqlite3_column_int(st, 0);
            scrollY = sqlite3_column_double(st, 1);
            zoom = sqlite3_column_double(st, 2);
            found = true;
        });
    return queried && found;
}

bool DocumentRepository::saveAnalysisRefs(const DocumentId& doc, const PaperAnalysis& analysis) {
    return db_->transaction([&] {
        bool ok = db_->execPrepared("DELETE FROM concept_edges WHERE document_id=?",
                                    [&](sqlite3_stmt* st) { return text(st, 1, doc); });
        ok = ok && db_->execPrepared("DELETE FROM concept_nodes WHERE document_id=?",
                                     [&](sqlite3_stmt* st) { return text(st, 1, doc); });
        for (const auto& c : analysis.concepts) {
            ok = ok && db_->execPrepared(
                           "INSERT OR REPLACE INTO concept_nodes(id,document_id,type,name,description) "
                           "VALUES(?,?,?,?,?)",
                           [&](sqlite3_stmt* st) {
                               return text(st, 1, c.id) && text(st, 2, doc) &&
                                      text(st, 3, c.type) && text(st, 4, c.name) &&
                                      text(st, 5, c.description);
                           });
        }
        for (const auto& r : analysis.relationships) {
            ok = ok && db_->execPrepared(
                           "INSERT INTO concept_edges(document_id,source_id,target_id,relation) "
                           "VALUES(?,?,?,?)",
                           [&](sqlite3_stmt* st) {
                               return text(st, 1, doc) && text(st, 2, r.source) &&
                                      text(st, 3, r.target) && text(st, 4, r.relation);
                           });
        }
        return ok;
    });
}

bool DocumentRepository::saveAnalysisCache(const DocumentId& doc, const std::string& fileHash,
                                           const PaperAnalysis& analysis) {
    const std::string serialized = json::serialize(analysis.toJson());
    return db_->execPrepared(
        "INSERT INTO analysis_cache(document_id,file_hash,schema_version,prompt_version,provider,"
        "model,generated_at,json) VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(document_id) DO UPDATE SET "
        "file_hash=excluded.file_hash,schema_version=excluded.schema_version,"
        "prompt_version=excluded.prompt_version,provider=excluded.provider,model=excluded.model,"
        "generated_at=excluded.generated_at,json=excluded.json",
        [&](sqlite3_stmt* st) {
            return text(st, 1, doc) && text(st, 2, fileHash) &&
                   integer(st, 3, analysis.meta.schemaVersion) &&
                   integer(st, 4, analysis.meta.promptVersion) && text(st, 5, analysis.meta.provider) &&
                   text(st, 6, analysis.meta.model) && integer(st, 7, analysis.meta.generatedAt) &&
                   text(st, 8, serialized);
        });
}

std::optional<PaperAnalysis> DocumentRepository::loadAnalysisCache(const DocumentId& doc,
                                                                    const std::string& fileHash,
                                                                    std::string& error) const {
    std::optional<PaperAnalysis> result;
    bool found = false;
    bool queried = db_->queryPrepared(
        "SELECT schema_version,prompt_version,provider,model,generated_at,json FROM analysis_cache "
        "WHERE document_id=? AND file_hash=?",
        [&](sqlite3_stmt* st) { return text(st, 1, doc) && text(st, 2, fileHash); },
        [&](sqlite3_stmt* st) {
            found = true;
            result = PaperAnalysis::parse(columnText(st, 5), error);
            if (!result) return;
            // Metadata columns are authoritative for cache invalidation and
            // remain checked against the serialized manifest.
            if (sqlite3_column_int(st, 0) != result->meta.schemaVersion ||
                sqlite3_column_int(st, 1) != result->meta.promptVersion ||
                columnText(st, 2) != result->meta.provider ||
                columnText(st, 3) != result->meta.model ||
                sqlite3_column_int64(st, 4) != result->meta.generatedAt) {
                result.reset();
                error = "analysis cache metadata mismatch";
            }
        });
    if (!queried) {
        error = db_->error();
        return std::nullopt;
    }
    if (!found && error.empty()) error = "analysis cache miss";
    return result;
}

ConversationId ChatRepository::createConversation(const DocumentId& doc, const std::string& title) {
    const ConversationId id = nextConversationId();
    const bool saved = db_->execPrepared(
        "INSERT INTO conversations(id,document_id,title,created_at) VALUES(?,?,?,?)",
        [&](sqlite3_stmt* st) {
            return text(st, 1, id) && text(st, 2, doc) && text(st, 3, title) &&
                   integer(st, 4, nowMs());
        });
    return saved ? id : ConversationId{};
}

bool ChatRepository::renameConversation(const ConversationId& conv, const std::string& title) {
    return db_->execPrepared("UPDATE conversations SET title=? WHERE id=?",
                             [&](sqlite3_stmt* st) {
                                 return text(st, 1, title) && text(st, 2, conv);
                             });
}

bool ChatRepository::deleteConversation(const ConversationId& conv) {
    return db_->transaction([&] {
        bool ok = db_->execPrepared(
            "DELETE FROM message_references WHERE message_id IN "
            "(SELECT id FROM messages WHERE conversation_id=?)",
            [&](sqlite3_stmt* st) { return text(st, 1, conv); });
        ok = ok && db_->execPrepared("DELETE FROM messages WHERE conversation_id=?",
                                     [&](sqlite3_stmt* st) { return text(st, 1, conv); });
        ok = ok && db_->execPrepared("DELETE FROM conversations WHERE id=?",
                                     [&](sqlite3_stmt* st) { return text(st, 1, conv); });
        return ok;
    });
}

std::vector<std::pair<ConversationId, std::string>> ChatRepository::conversationsFor(
    const DocumentId& doc) const {
    std::vector<std::pair<ConversationId, std::string>> out;
    db_->queryPrepared(
        "SELECT id,title FROM conversations WHERE document_id=? ORDER BY created_at,id",
        [&](sqlite3_stmt* st) { return text(st, 1, doc); },
        [&](sqlite3_stmt* st) { out.emplace_back(columnText(st, 0), columnText(st, 1)); });
    return out;
}

std::vector<std::pair<ConversationId, std::string>> ChatRepository::searchConversations(
    const DocumentId& doc, const std::string& query) const {
    std::vector<std::pair<ConversationId, std::string>> out;
    const std::string pattern = "%" + query + "%";
    db_->queryPrepared(
        "SELECT id,title FROM conversations WHERE document_id=? AND title LIKE ? "
        "ORDER BY created_at,id",
        [&](sqlite3_stmt* st) { return text(st, 1, doc) && text(st, 2, pattern); },
        [&](sqlite3_stmt* st) { out.emplace_back(columnText(st, 0), columnText(st, 1)); });
    return out;
}

bool ChatRepository::saveMessage(const ConversationId& conv, const ChatMessage& msg) {
    return db_->transaction([&] {
      bool ok = db_->execPrepared(
          "INSERT OR REPLACE INTO messages(id,conversation_id,role,text,created_at,incomplete) "
          "VALUES(?,?,?,?,?,?)",
          [&](sqlite3_stmt* st) {
              return text(st, 1, msg.id) && text(st, 2, conv) && text(st, 3, msg.role) &&
                     text(st, 4, msg.text) && integer(st, 5, msg.createdAt) &&
                     integer(st, 6, msg.incomplete ? 1 : 0);
          });
      ok = ok && db_->execPrepared("DELETE FROM message_references WHERE message_id=?",
                                   [&](sqlite3_stmt* st) { return text(st, 1, msg.id); });
      const auto saveReference = [&](const ContextReference& ref) {
        const auto& a = ref.anchor;
        return db_->execPrepared(
            "INSERT INTO message_references(message_id,ref_type,anchor_text,page,block_id,"
            "display_name,extracted,section_id,reference_id,is_source,x,y,w,h,document_id,"
            "object_type,object_id,caption,latex,table_rows,related_sources,image_mime,image_blob,"
            "image_width,image_height) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [&](sqlite3_stmt* st) {
                return text(st, 1, msg.id) && text(st, 2, referenceTypeCode(ref.type)) &&
                       text(st, 3, a.anchorText) && integer(st, 4, a.page) &&
                       optionalText(st, 5, a.block) && text(st, 6, ref.displayName) &&
                       text(st, 7, ref.extractedText) && optionalText(st, 8, a.section) &&
                       text(st, 9, ref.id) && integer(st, 10, 0) && real(st, 11, a.bounds.x) &&
                       real(st, 12, a.bounds.y) && real(st, 13, a.bounds.width) &&
                       real(st, 14, a.bounds.height) && text(st, 15, a.document) &&
                       text(st, 16, a.objectType) && text(st, 17, a.objectId) &&
                       text(st, 18, ref.caption) && text(st, 19, ref.latex) &&
                       text(st, 20, tableRowsJson(ref.tableRows)) &&
                       text(st, 21, relatedSourcesJson(ref.relatedSources)) &&
                       text(st, 22, ref.image ? ref.image->mimeType : "") &&
                       blob(st, 23, ref.image ? ref.image->bytes : std::vector<std::uint8_t>{}) &&
                       integer(st, 24, ref.image ? ref.image->width : 0) &&
                       integer(st, 25, ref.image ? ref.image->height : 0);
            });
      };
      for (const auto& ref : msg.references) ok = ok && saveReference(ref);
      const auto saveSource = [&](const ChatSource& source) -> bool {
          const auto& a = source.anchor;
          return db_->execPrepared(
                       "INSERT INTO message_references(message_id,ref_type,anchor_text,page,"
                       "block_id,display_name,extracted,section_id,reference_id,is_source,x,y,w,h,"
                       "document_id,object_type,object_id,caption,latex,table_rows,related_sources,"
                       "image_mime,image_blob,image_width,image_height) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                       [&](sqlite3_stmt* st) {
                           return text(st, 1, msg.id) && text(st, 2, "source") &&
                                  text(st, 3, a.anchorText) && integer(st, 4, a.page) &&
                                  optionalText(st, 5, a.block) && text(st, 6, "") &&
                                  text(st, 7, "") && optionalText(st, 8, a.section) &&
                                  text(st, 9, source.citationId) && integer(st, 10, 1) &&
                                  real(st, 11, a.bounds.x) && real(st, 12, a.bounds.y) &&
                                  real(st, 13, a.bounds.width) && real(st, 14, a.bounds.height) &&
                                  text(st, 15, a.document) && text(st, 16, a.objectType) &&
                                  text(st, 17, a.objectId) && text(st, 18, "") &&
                                  text(st, 19, "") && text(st, 20, "") && text(st, 21, "") &&
                                  text(st, 22, "") && blob(st, 23, {}) && integer(st, 24, 0) &&
                                  integer(st, 25, 0);
                         });
      };
      if (!msg.sourceRecords.empty())
          for (const auto& source : msg.sourceRecords) ok = ok && saveSource(source);
      else
          for (const auto& anchor : msg.sources)
              ok = ok && saveSource({anchorReferenceId(anchor), anchor});
      return ok;
    });
}

std::vector<ChatMessage> ChatRepository::loadRecent(const ConversationId& conv, std::size_t n) {
    std::vector<ChatMessage> out;
    db_->queryPrepared(
        "SELECT id,role,text,created_at,incomplete FROM messages WHERE conversation_id=? "
        "ORDER BY created_at DESC,rowid DESC LIMIT ?",
        [&](sqlite3_stmt* st) {
            return text(st, 1, conv) && integer(st, 2, static_cast<sqlite3_int64>(n));
        },
        [&](sqlite3_stmt* st) {
            ChatMessage m;
            m.id = columnText(st, 0);
            m.role = columnText(st, 1);
            m.text = columnText(st, 2);
            m.createdAt = sqlite3_column_int64(st, 3);
            m.incomplete = sqlite3_column_int(st, 4) != 0;
            out.push_back(std::move(m));
        });
    std::reverse(out.begin(), out.end());
    for (auto& m : out) {
        db_->queryPrepared(
            "SELECT ref_type,anchor_text,page,block_id,display_name,extracted,section_id,"
            "reference_id,is_source,x,y,w,h,document_id,object_type,object_id,caption,latex,"
            "table_rows,related_sources,image_mime,image_blob,image_width,image_height "
            "FROM message_references "
            "WHERE message_id=? ORDER BY rowid",
            [&](sqlite3_stmt* st) { return text(st, 1, m.id); },
            [&](sqlite3_stmt* st) {
                DocumentAnchor anchor;
                anchor.document = columnText(st, 13);
                anchor.page = sqlite3_column_int(st, 2);
                anchor.bounds = {static_cast<float>(sqlite3_column_double(st, 9)),
                                 static_cast<float>(sqlite3_column_double(st, 10)),
                                 static_cast<float>(sqlite3_column_double(st, 11)),
                                 static_cast<float>(sqlite3_column_double(st, 12))};
                anchor.anchorText = columnText(st, 1);
                anchor.block = columnOptionalText(st, 3);
                anchor.section = columnOptionalText(st, 6);
                anchor.objectType = columnText(st, 14);
                anchor.objectId = columnText(st, 15);
                if (sqlite3_column_int(st, 8) != 0) {
                    const auto citationId = columnText(st, 7);
                    m.sources.push_back(anchor);
                    m.sourceRecords.push_back({citationId.empty() ? anchorReferenceId(anchor)
                                                                    : citationId,
                                               std::move(anchor)});
                    return;
                }
                ContextReference ref;
                ref.id = columnText(st, 7);
                ref.type = referenceTypeFromCode(columnText(st, 0));
                ref.anchor = std::move(anchor);
                ref.displayName = columnText(st, 4);
                ref.extractedText = columnText(st, 5);
                ref.caption = columnText(st, 16);
                ref.latex = columnText(st, 17);
                ref.tableRows = parseTableRows(columnText(st, 18));
                ref.relatedSources = parseRelatedSources(columnText(st, 19));
                const auto mime = columnText(st, 20);
                const auto imageBytes = columnBlob(st, 21);
                if (!mime.empty() || !imageBytes.empty())
                    ref.image = ReferenceImage{mime, imageBytes, sqlite3_column_int(st, 22),
                                               sqlite3_column_int(st, 23)};
                m.references.push_back(std::move(ref));
            });
    }
    return out;
}

} // namespace reader
