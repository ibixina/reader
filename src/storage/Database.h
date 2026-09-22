#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace reader {

// RAII SQLite wrapper (§56). Schema covers documents, reading_state,
// sections, blocks, anchors, annotations, notes, conversations, messages,
// message_references, concept_nodes, concept_edges, summaries.
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool ok() const { return db_ != nullptr && schemaOk_; }
    std::string error() const;

    bool exec(const std::string& sql);
    void migrate();

    using RowCallback = std::function<void(sqlite3_stmt*)>;
    using BindCallback = std::function<bool(sqlite3_stmt*)>;
    bool query(const std::string& sql, RowCallback cb);
    bool execPrepared(const std::string& sql, BindCallback bind = {});
    bool queryPrepared(const std::string& sql, BindCallback bind, RowCallback cb);
    bool transaction(const std::function<bool()>& work);
    bool readTransaction(const std::function<bool()>& work) const;

    sqlite3* handle() { return db_; }

private:
    sqlite3* db_ = nullptr;
    bool schemaOk_ = false;
    std::string openError_;
    mutable std::recursive_mutex mutex_;
};

std::string defaultDbPath();

} // namespace reader
