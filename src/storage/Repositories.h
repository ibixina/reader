#pragma once
#include "ai/ChatManager.h"
#include "ai/References.h"
#include "analysis/PaperAnalysis.h"
#include "document/DocumentAnchor.h"
#include "document/DocumentModel.h"
#include "storage/Database.h"
#include <optional>
#include <string>
#include <vector>

namespace reader {

struct Note {
    NoteId id;
    DocumentAnchor anchor;
    std::string text;
    TimestampMs createdAt = 0;
    TimestampMs updatedAt = 0;
};

struct UserAnnotation {
    AnnotationId id;
    DocumentAnchor anchor;
    std::string kind; // highlight|underline|strikethrough|note|bookmark
    std::string color;
};

// Geometrically + textually anchored (§39) so notes survive re-extraction.
class AnnotationRepository {
public:
    explicit AnnotationRepository(Database* db) : db_(db) {}
    bool saveNote(const DocumentId& doc, const Note& note);
    std::vector<Note> notesFor(const DocumentId& doc) const;
    bool deleteNote(const DocumentId& doc, const NoteId& id);
    bool saveAnnotation(const DocumentId& doc, const UserAnnotation& ann);
    std::vector<UserAnnotation> annotationsFor(const DocumentId& doc) const;
    bool deleteAnnotation(const DocumentId& doc, const AnnotationId& id);

private:
    Database* db_;
};

class DocumentRepository {
public:
    explicit DocumentRepository(Database* db) : db_(db) {}
    bool saveDocument(const Document& doc);
    std::vector<Document> recentDocuments(std::size_t limit = 20) const;
    // Persist the extracted model and its document-scoped stable IDs.
    bool saveModel(const DocumentModel& model);
    bool loadModel(const DocumentId& doc, DocumentModel& model) const;
    bool loadModel(const DocumentId& doc, const std::string& expectedFileHash,
                   DocumentModel& model) const;
    bool saveReadingState(const DocumentId& doc, int page, double scrollY, double zoom);
    bool loadReadingState(const DocumentId& doc, int& page, double& scrollY, double& zoom) const;
    bool saveAnalysisRefs(const DocumentId& doc, const PaperAnalysis& analysis);
    bool saveAnalysisCache(const DocumentId& doc, const std::string& fileHash,
                           const PaperAnalysis& analysis);
    std::optional<PaperAnalysis> loadAnalysisCache(const DocumentId& doc,
                                                   const std::string& fileHash,
                                                   std::string& error) const;
    static std::string esc(const std::string& s);

private:
    Database* db_;
};

class ChatRepository : public IChatStore {
public:
    explicit ChatRepository(Database* db) : db_(db) {}
    ConversationId createConversation(const DocumentId& doc, const std::string& title);
    bool renameConversation(const ConversationId& conv, const std::string& title);
    bool deleteConversation(const ConversationId& conv);
    std::vector<ChatMessage> loadRecent(const ConversationId& conv, std::size_t n) override;
    bool saveMessage(const ConversationId& conv, const ChatMessage& msg) override;
    std::vector<std::pair<ConversationId, std::string>> conversationsFor(const DocumentId& doc) const;
    std::vector<std::pair<ConversationId, std::string>> searchConversations(
        const DocumentId& doc, const std::string& query) const;

private:
    Database* db_;
};

} // namespace reader
