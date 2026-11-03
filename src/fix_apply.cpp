#include "fix_apply.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <tuple>

namespace {

struct Resolved {
    size_t start, end;
    const std::string* text;
    int role;       // 0 opener insertion, 1 closer insertion, 2 replacement
    size_t order;   // acceptance index of the owning fix
};

bool same_edit(const Resolved& a, const Resolved& b) {
    return a.start == b.start && a.end == b.end && *a.text == *b.text;
}

bool conflicts(const Resolved& a, const Resolved& b) {
    if (a.start == a.end && b.start == b.end)
        return false;
    if (a.start == a.end)
        return b.start < a.start && a.start < b.end;
    if (b.start == b.end)
        return a.start < b.start && b.start < a.end;
    return a.start < b.end && b.start < a.end;
}

} // namespace

FixApplyResult apply_fixes(std::string_view text, const std::vector<const FixInfo*>& fixes) {
    FixApplyResult result;
    std::vector<std::vector<Resolved>> accepted;
    for (const FixInfo* fix : fixes) {
        std::vector<Resolved> cur;
        size_t min_start = std::string::npos;
        for (const FixEdit& e : fix->edits) {
            size_t s = lsp_position_to_byte_offset(text, e.start_line, e.start_col);
            size_t t = lsp_position_to_byte_offset(text, e.end_line, e.end_col);
            cur.push_back({s, std::max(s, t), &e.new_text, 0, accepted.size()});
            min_start = std::min(min_start, s);
        }
        for (Resolved& r : cur)
            r.role = r.end > r.start ? 2 : (r.start == min_start ? 0 : 1);

        bool duplicate = false, clash = false;
        for (const auto& prev : accepted) {
            if (prev.size() == cur.size() &&
                std::equal(prev.begin(), prev.end(), cur.begin(), same_edit))
                duplicate = true;
            for (const Resolved& a : prev)
                for (const Resolved& b : cur)
                    clash = clash || conflicts(a, b);
        }
        if (duplicate) {
            result.outcomes.push_back(FixOutcome::Duplicate);
            continue;
        }
        if (clash) {
            ++result.skipped;
            result.outcomes.push_back(FixOutcome::Skipped);
            continue;
        }
        accepted.push_back(std::move(cur));
        ++result.applied;
        result.outcomes.push_back(FixOutcome::Applied);
    }

    std::vector<Resolved> all;
    for (auto& v : accepted)
        all.insert(all.end(), v.begin(), v.end());
    // Openers: later-accepted first (outermost).  Closers: earlier-accepted first.
    std::stable_sort(all.begin(), all.end(), [](const Resolved& a, const Resolved& b) {
        auto key = [](const Resolved& r) {
            return std::make_tuple(r.start, r.role,
                                   r.role == 0 ? ~r.order : r.order);
        };
        return key(a) < key(b);
    });
    size_t pos = 0;
    for (const Resolved& r : all) {
        if (r.start < pos)
            continue; // only reachable for edits of one fix overlapping each other
        result.text.append(text.substr(pos, r.start - pos));
        result.text += *r.text;
        pos = r.end;
    }
    result.text.append(text.substr(pos));
    return result;
}
