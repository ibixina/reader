#include "storage/Database.h"
#include <cstdlib>
#include <sqlite3.h>

namespace reader {

Database::Database(const std::string& path) {
    sqlite3* opened = nullptr;
    if (sqlite3_open(path.c_str(), &opened) != SQLITE_OK) {
        openError_ = opened ? sqlite3_errmsg(opened) : "open failed";
        if (opened) sqlite3_close(opened);
        db_ = nullptr;
        return;
    }
    db_ = opened;
    migrate();
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

std::string Database::error() const {
    return db_ ? sqlite3_errmsg(db_) : (openError_.empty() ? "open failed" : openError_);
}

bool Database::exec(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;
    char* msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg);
    if (msg) sqlite3_free(msg);
    return rc == SQLITE_OK;
}

bool Database::query(const std::string& sql, RowCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) cb(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool Database::execPrepared(const std::string& sql, BindCallback bind) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    bool bound = !bind || bind(st);
    const int rc = bound ? sqlite3_step(st) : SQLITE_MISUSE;
    sqlite3_finalize(st);
    return bound && rc == SQLITE_DONE;
}

bool Database::queryPrepared(const std::string& sql, BindCallback bind, RowCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    if (bind && !bind(st)) {
        sqlite3_finalize(st);
        return false;
    }
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) cb(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool Database::transaction(const std::function<bool()>& work) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || !exec("BEGIN IMMEDIATE")) return false;
    bool ok = false;
    try {
        ok = work();
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
    if (ok && exec("COMMIT")) return true;
    exec("ROLLBACK");
    return false;
}

bool Database::readTransaction(const std::function<bool()>& work) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;
    // A deferred read transaction gives all repository queries one coherent
    // snapshot while the same connection remains serialized by mutex_.
    if (sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    bool ok = false;
    try {
        ok = work();
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
    if (ok && sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK) return true;
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
}

void Database::migrate() {
    schemaOk_ = false;
    const char* statements[] = {
    "PRAGMA journal_mode=WAL;",
    R"(CREATE TABLE IF NOT EXISTS documents(
      id TEXT PRIMARY KEY, path TEXT, hash TEXT UNIQUE, title TEXT,
      authors TEXT, abstract_text TEXT, keywords TEXT,
      page_count INTEGER, last_opened INTEGER, extraction_version INTEGER DEFAULT 0,
      extraction_complete INTEGER DEFAULT 0))",
    R"(CREATE TABLE IF NOT EXISTS reading_state(
      document_id TEXT PRIMARY KEY, page INTEGER, scroll_y REAL, zoom REAL,
      updated_at INTEGER))",
    R"(CREATE TABLE IF NOT EXISTS sections(
      id TEXT, document_id TEXT, title TEXT, level INTEGER,
      start_page INTEGER, end_page INTEGER, PRIMARY KEY(document_id,id)))",
    R"(CREATE TABLE IF NOT EXISTS blocks(
      id TEXT, document_id TEXT, page INTEGER,
      x REAL, y REAL, w REAL, h REAL, text TEXT, PRIMARY KEY(document_id,id)))",
    R"(CREATE TABLE IF NOT EXISTS section_blocks(
      document_id TEXT, section_id TEXT, block_id TEXT, ordinal INTEGER,
      PRIMARY KEY(document_id,section_id,block_id)))",
    R"(CREATE TABLE IF NOT EXISTS line_spans(
      document_id TEXT, ordinal INTEGER, page INTEGER, x REAL, y REAL, w REAL, h REAL,
      text TEXT, font_size REAL, bold INTEGER, PRIMARY KEY(document_id,ordinal)))",
    R"(CREATE TABLE IF NOT EXISTS anchors(
      id INTEGER PRIMARY KEY AUTOINCREMENT, document_id TEXT, page INTEGER,
      x REAL, y REAL, w REAL, h REAL, anchor_text TEXT, section_id TEXT,
      block_id TEXT, kind TEXT))",
    R"(CREATE TABLE IF NOT EXISTS annotations(
      id TEXT PRIMARY KEY, document_id TEXT, page INTEGER,
      x REAL, y REAL, w REAL, h REAL, anchor_text TEXT,
      kind TEXT, color TEXT, created_at INTEGER, section_id TEXT, block_id TEXT,
      object_type TEXT, object_id TEXT))",
    R"(CREATE TABLE IF NOT EXISTS notes(
      id TEXT PRIMARY KEY, document_id TEXT, page INTEGER,
      x REAL, y REAL, w REAL, h REAL, anchor_text TEXT,
      text TEXT, created_at INTEGER, updated_at INTEGER, section_id TEXT, block_id TEXT,
      object_type TEXT, object_id TEXT))",
    R"(CREATE TABLE IF NOT EXISTS conversations(
      id TEXT PRIMARY KEY, document_id TEXT, title TEXT, created_at INTEGER))",
    R"(CREATE TABLE IF NOT EXISTS messages(
      id TEXT PRIMARY KEY, conversation_id TEXT, role TEXT, text TEXT,
      created_at INTEGER, incomplete INTEGER))",
    R"(CREATE TABLE IF NOT EXISTS message_references(
      message_id TEXT, ref_type TEXT, anchor_text TEXT, page INTEGER,
      block_id TEXT, display_name TEXT, extracted TEXT, section_id TEXT,
      reference_id TEXT, is_source INTEGER, x REAL, y REAL, w REAL, h REAL,
      document_id TEXT, object_type TEXT, object_id TEXT, caption TEXT, latex TEXT,
      table_rows TEXT, related_sources TEXT, image_mime TEXT, image_blob BLOB,
      image_width INTEGER, image_height INTEGER))",
    R"(CREATE TABLE IF NOT EXISTS concept_nodes(
      id TEXT, document_id TEXT, type TEXT, name TEXT,
      description TEXT, PRIMARY KEY(document_id,id)))",
    R"(CREATE TABLE IF NOT EXISTS concept_edges(
      document_id TEXT, source_id TEXT, target_id TEXT, relation TEXT))",
    R"(CREATE TABLE IF NOT EXISTS summaries(
      document_id TEXT, section_id TEXT, depth TEXT, text TEXT,
      PRIMARY KEY(document_id, section_id, depth)))",
    R"(CREATE TABLE IF NOT EXISTS document_objects(
      document_id TEXT, object_id TEXT, object_type TEXT, page INTEGER,
      x REAL, y REAL, w REAL, h REAL, text TEXT, caption TEXT,
      metadata TEXT, block_id TEXT, PRIMARY KEY(document_id,object_id)))",
    R"(CREATE TABLE IF NOT EXISTS analysis_cache(
      document_id TEXT PRIMARY KEY, file_hash TEXT, schema_version INTEGER,
      prompt_version INTEGER, provider TEXT, model TEXT, generated_at INTEGER,
      json TEXT NOT NULL))",
    "CREATE INDEX IF NOT EXISTS idx_message_refs_message ON message_references(message_id);"
    };
    for (const char* statement : statements) {
        if (!exec(statement)) return;
    }
    auto ensureMessageReferenceColumn = [&](const char* name, const char* alter) {
        bool present = false;
        if (!query("PRAGMA table_info(message_references);", [&](sqlite3_stmt* st) {
                const auto* value = sqlite3_column_text(st, 1);
                if (value && std::string(reinterpret_cast<const char*>(value)) == name)
                    present = true;
            }))
            return false;
        return present || exec(alter);
    };
    if (!ensureMessageReferenceColumn("section_id", "ALTER TABLE message_references ADD COLUMN section_id TEXT") ||
        !ensureMessageReferenceColumn("reference_id", "ALTER TABLE message_references ADD COLUMN reference_id TEXT") ||
        !ensureMessageReferenceColumn("is_source", "ALTER TABLE message_references ADD COLUMN is_source INTEGER DEFAULT 0") ||
        !ensureMessageReferenceColumn("x", "ALTER TABLE message_references ADD COLUMN x REAL") ||
        !ensureMessageReferenceColumn("y", "ALTER TABLE message_references ADD COLUMN y REAL") ||
        !ensureMessageReferenceColumn("w", "ALTER TABLE message_references ADD COLUMN w REAL") ||
        !ensureMessageReferenceColumn("h", "ALTER TABLE message_references ADD COLUMN h REAL"))
        return;
    auto ensureColumn = [&](const char* table, const char* name, const char* alter) {
        bool present = false;
        if (!query(std::string("PRAGMA table_info(") + table + ");", [&](sqlite3_stmt* st) {
                const auto* value = sqlite3_column_text(st, 1);
                if (value && std::string(reinterpret_cast<const char*>(value)) == name)
                    present = true;
            }))
            return false;
        return present || exec(alter);
    };
    if (!ensureColumn("documents", "abstract_text", "ALTER TABLE documents ADD COLUMN abstract_text TEXT") ||
        !ensureColumn("documents", "keywords", "ALTER TABLE documents ADD COLUMN keywords TEXT") ||
        !ensureColumn("documents", "extraction_version", "ALTER TABLE documents ADD COLUMN extraction_version INTEGER DEFAULT 0") ||
        !ensureColumn("documents", "extraction_complete", "ALTER TABLE documents ADD COLUMN extraction_complete INTEGER DEFAULT 0") ||
        !ensureColumn("notes", "section_id", "ALTER TABLE notes ADD COLUMN section_id TEXT") ||
        !ensureColumn("notes", "block_id", "ALTER TABLE notes ADD COLUMN block_id TEXT") ||
        !ensureColumn("annotations", "section_id", "ALTER TABLE annotations ADD COLUMN section_id TEXT") ||
        !ensureColumn("annotations", "block_id", "ALTER TABLE annotations ADD COLUMN block_id TEXT") ||
        !ensureColumn("notes", "object_type", "ALTER TABLE notes ADD COLUMN object_type TEXT") ||
        !ensureColumn("notes", "object_id", "ALTER TABLE notes ADD COLUMN object_id TEXT") ||
        !ensureColumn("annotations", "object_type", "ALTER TABLE annotations ADD COLUMN object_type TEXT") ||
        !ensureColumn("annotations", "object_id", "ALTER TABLE annotations ADD COLUMN object_id TEXT") ||
        !ensureMessageReferenceColumn("document_id", "ALTER TABLE message_references ADD COLUMN document_id TEXT"))
        return;
    if (!ensureMessageReferenceColumn("object_type", "ALTER TABLE message_references ADD COLUMN object_type TEXT") ||
        !ensureMessageReferenceColumn("object_id", "ALTER TABLE message_references ADD COLUMN object_id TEXT") ||
        !ensureMessageReferenceColumn("caption", "ALTER TABLE message_references ADD COLUMN caption TEXT") ||
        !ensureMessageReferenceColumn("latex", "ALTER TABLE message_references ADD COLUMN latex TEXT") ||
        !ensureMessageReferenceColumn("table_rows", "ALTER TABLE message_references ADD COLUMN table_rows TEXT") ||
        !ensureMessageReferenceColumn("related_sources", "ALTER TABLE message_references ADD COLUMN related_sources TEXT") ||
        !ensureMessageReferenceColumn("image_mime", "ALTER TABLE message_references ADD COLUMN image_mime TEXT") ||
        !ensureMessageReferenceColumn("image_blob", "ALTER TABLE message_references ADD COLUMN image_blob BLOB") ||
        !ensureMessageReferenceColumn("image_width", "ALTER TABLE message_references ADD COLUMN image_width INTEGER") ||
        !ensureMessageReferenceColumn("image_height", "ALTER TABLE message_references ADD COLUMN image_height INTEGER"))
        return;
    auto hasCompositeDocumentKey = [&](const char* table) {
        bool idKey = false;
        bool documentKey = false;
        const bool queried = query(std::string("PRAGMA table_info(") + table + ");",
                                   [&](sqlite3_stmt* st) {
            const auto* name = sqlite3_column_text(st, 1);
            if (!name || sqlite3_column_int(st, 5) <= 0) return;
            if (std::string(reinterpret_cast<const char*>(name)) == "id") idKey = true;
            if (std::string(reinterpret_cast<const char*>(name)) == "document_id")
                documentKey = true;
        });
        return queried && idKey && documentKey;
    };
    auto migrateScopedTable = [&](const char* table, const char* schema) {
        if (hasCompositeDocumentKey(table)) return true;
        const std::string legacy = std::string(table) + "_legacy";
        return transaction([&] {
            return exec(std::string("ALTER TABLE ") + table + " RENAME TO " + legacy + ";") &&
                   exec(schema) &&
                   exec(std::string("INSERT OR IGNORE INTO ") + table +
                        " SELECT * FROM " + legacy + ";") &&
                   exec(std::string("DROP TABLE ") + legacy + ";");
        });
    };
    if (!migrateScopedTable("sections", R"(CREATE TABLE sections(
          id TEXT, document_id TEXT, title TEXT, level INTEGER,
          start_page INTEGER, end_page INTEGER, PRIMARY KEY(document_id,id)))") ||
        !migrateScopedTable("blocks", R"(CREATE TABLE blocks(
          id TEXT, document_id TEXT, page INTEGER,
          x REAL, y REAL, w REAL, h REAL, text TEXT, PRIMARY KEY(document_id,id)))"))
        return;
    bool documentPartOfConceptKey = false;
    if (!query("PRAGMA table_info(concept_nodes);", [&](sqlite3_stmt* st) {
            const auto* value = sqlite3_column_text(st, 1);
            if (value && std::string(reinterpret_cast<const char*>(value)) == "document_id" &&
                sqlite3_column_int(st, 5) > 0)
                documentPartOfConceptKey = true;
        }))
        return;
    if (!documentPartOfConceptKey) {
        if (!transaction([&] {
                return exec("ALTER TABLE concept_nodes RENAME TO concept_nodes_legacy;") &&
                       exec(R"(CREATE TABLE concept_nodes(
                         id TEXT, document_id TEXT, type TEXT, name TEXT,
                         description TEXT, PRIMARY KEY(document_id,id)))") &&
                       exec(R"(INSERT OR IGNORE INTO concept_nodes(id,document_id,type,name,description)
                         SELECT id,document_id,type,name,description FROM concept_nodes_legacy)"
                       ) &&
                       exec("DROP TABLE concept_nodes_legacy;");
            }))
            return;
    }
    schemaOk_ = true;
}

std::string defaultDbPath() {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : "/tmp";
    return base + "/.local/share/paper-reader/library.db";
}

} // namespace reader
