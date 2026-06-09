#include "internal.h"

// ── Diagnostics ───────────────────────────────────────────────────────────────

struct FixItEntry {
    uint32_t    start_line = 0, start_col = 0;
    uint32_t    end_line   = 0, end_col   = 0;
    std::string replacement; // empty = pure deletion
};

struct DiagEntry {
    std::string file;
    uint32_t    line     = 0;
    uint32_t    col      = 0;
    uint32_t    end_line = 0; // squiggle range end (same as line/col if no range)
    uint32_t    end_col  = 0;
    uint8_t     severity = 0;
    std::string message;
    std::string check_name;
    std::vector<FixItEntry> fixits;
};

struct CB_DiagIter {
    std::vector<DiagEntry> entries;
    size_t pos = 0;
    DiagEntry current; // stable storage for cb_diag_next pointer fields
};

// Map clang's DiagnosticsEngine::Level (Ignored=0, Note=1, Remark=2, Warning=3,
// Error=4, Fatal=5) onto the CB severity scale documented in clang_bridge.h
// (note=0, remark=1, warning=2, error=3, fatal=4).  A direct cast is off by one
// and turns every Error into a Fatal and every Note into a Remark.
static uint8_t cb_severity_from_level(DiagnosticsEngine::Level lvl) {
    switch (lvl) {
        case DiagnosticsEngine::Ignored: return 0;
        case DiagnosticsEngine::Note:    return 0;
        case DiagnosticsEngine::Remark:  return 1;
        case DiagnosticsEngine::Warning: return 2;
        case DiagnosticsEngine::Error:   return 3;
        case DiagnosticsEngine::Fatal:   return 4;
    }
    return 0;
}

CB_DiagIter *cb_diag_iter(CB_TransUnit *tu) {
    auto *it = new CB_DiagIter{};
    const SourceManager &SM = tu->ast->getSourceManager();
    for (auto it2 = tu->ast->stored_diag_begin();
         it2 != tu->ast->stored_diag_end(); ++it2) {
        const StoredDiagnostic &sd = *it2;
        DiagEntry e;

        auto ploc = SM.getPresumedLoc(sd.getLocation());
        if (ploc.isValid()) {
            e.file = ploc.getFilename() ? ploc.getFilename() : "";
            e.line = e.end_line = (uint32_t)ploc.getLine();
            e.col  = e.end_col  = (uint32_t)ploc.getColumn();
        }

        // Expand squiggle range from the first reported source range.
        for (const auto &range : sd.getRanges()) {
            auto sl = SM.getPresumedLoc(range.getBegin());
            auto el = SM.getPresumedLoc(range.getEnd());
            if (sl.isValid()) { e.line = sl.getLine(); e.col = sl.getColumn(); }
            if (el.isValid()) { e.end_line = el.getLine(); e.end_col = el.getColumn(); }
            break; // first range is sufficient
        }

        e.severity = cb_severity_from_level(sd.getLevel());
        e.message  = sd.getMessage().str();

        // Collect fix-it hints (source replacements / quick-fixes).
        for (const auto &fixit : sd.getFixIts()) {
            FixItEntry f;
            auto sl = SM.getPresumedLoc(fixit.RemoveRange.getBegin());
            auto el = SM.getPresumedLoc(fixit.RemoveRange.getEnd());
            if (sl.isValid()) { f.start_line = sl.getLine(); f.start_col = sl.getColumn(); }
            if (el.isValid()) { f.end_line   = el.getLine(); f.end_col   = el.getColumn(); }
            f.replacement = fixit.CodeToInsert;
            e.fixits.push_back(std::move(f));
        }

        it->entries.push_back(std::move(e));
    }
    return it;
}

int cb_diag_next(CB_DiagIter *it, CB_Diag *out) {
    if (it->pos >= it->entries.size()) return 0;
    it->current = it->entries[it->pos++];
    out->file       = it->current.file.c_str();
    out->line       = it->current.line;
    out->col        = it->current.col;
    out->end_line   = it->current.end_line;
    out->end_col    = it->current.end_col;
    out->severity   = it->current.severity;
    out->message    = it->current.message.c_str();
    out->check_name = it->current.check_name.empty()
                    ? nullptr : it->current.check_name.c_str();
    return 1;
}

// Fix-it access — valid immediately after cb_diag_next returns 1.
size_t cb_diag_fixit_count(const CB_DiagIter *it) {
    return it->current.fixits.size();
}

void cb_diag_fixit_get(const CB_DiagIter *it, size_t i, CB_FixIt *out) {
    const auto &f = it->current.fixits[i];
    out->start_line  = f.start_line;
    out->start_col   = f.start_col;
    out->end_line    = f.end_line;
    out->end_col     = f.end_col;
    out->replacement = f.replacement.c_str();
}

void cb_diag_iter_destroy(CB_DiagIter *it) { delete it; }

