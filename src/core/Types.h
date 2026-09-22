#pragma once
#include <cstdint>
#include <string>

namespace reader {

using DocumentId = std::string;
using BlockId = std::string;
using SectionId = std::string;
using EquationId = std::string;
using FigureId = std::string;
using TableId = std::string;
using CitationId = std::string;
using ReferenceId = std::string;
using MessageId = std::string;
using ConceptId = std::string;
using NoteId = std::string;
using AnnotationId = std::string;
using ConversationId = std::string;

struct Rect {
    float x = 0, y = 0, width = 0, height = 0;
    bool contains(float px, float py) const {
        return px >= x && px <= x + width && py >= y && py <= y + height;
    }
    bool valid() const { return width > 0 && height > 0; }
};

using TimestampMs = std::int64_t;
TimestampMs nowMs();

std::string sha256Hex(const std::string& data);
std::string sha256File(const std::string& path);

std::string trim(const std::string& s);
std::string toLower(std::string s);
bool startsWith(const std::string& s, const std::string& prefix);

} // namespace reader
