#include "document/DocumentModel.h"

namespace reader {

const TextBlock* DocumentModel::findBlock(const BlockId& id) const {
    auto it = blockIndex_.find(id);
    return it == blockIndex_.end() ? nullptr : &blocks[it->second];
}

const Section* DocumentModel::findSection(const SectionId& id) const {
    auto it = sectionIndex_.find(id);
    return it == sectionIndex_.end() ? nullptr : &sections[it->second];
}

const Equation* DocumentModel::findEquation(const EquationId& id) const {
    for (const auto& e : equations)
        if (e.id == id) return &e;
    return nullptr;
}

const Figure* DocumentModel::findFigure(const FigureId& id) const {
    for (const auto& f : figures)
        if (f.id == id) return &f;
    return nullptr;
}

const Table* DocumentModel::findTable(const TableId& id) const {
    for (const auto& t : tables)
        if (t.id == id) return &t;
    return nullptr;
}

const Section* DocumentModel::sectionForPage(int page) const {
    const Section* best = nullptr;
    for (const auto& s : sections) {
        if (page >= s.startPage && page <= s.endPage) {
            if (!best || s.level > best->level) best = &s;
        }
    }
    return best;
}

const Section* DocumentModel::sectionForBlock(const BlockId& id) const {
    for (const auto& s : sections)
        for (const auto& b : s.blocks)
            if (b == id) return &s;
    return nullptr;
}

const TextBlock* DocumentModel::blockAtPage(int page, float yCenter) const {
    const TextBlock* best = nullptr;
    float bestDist = 1e30f;
    for (const auto& b : blocks) {
        if (b.page != page) continue;
        float cy = b.bounds.y + b.bounds.height * 0.5f;
        float d = cy > yCenter ? cy - yCenter : yCenter - cy;
        if (d < bestDist) {
            bestDist = d;
            best = &b;
        }
    }
    return best;
}

std::string DocumentModel::sectionText(const SectionId& id) const {
    const Section* s = findSection(id);
    if (!s) return {};
    std::string out;
    for (const auto& bid : s->blocks) {
        const TextBlock* b = findBlock(bid);
        if (!b) continue;
        if (!out.empty()) out += "\n";
        out += b->text;
    }
    return out;
}

void DocumentModel::rebuildIndex() {
    blockIndex_.clear();
    sectionIndex_.clear();
    for (std::size_t i = 0; i < blocks.size(); ++i) blockIndex_[blocks[i].id] = i;
    for (std::size_t i = 0; i < sections.size(); ++i) sectionIndex_[sections[i].id] = i;
}

} // namespace reader
