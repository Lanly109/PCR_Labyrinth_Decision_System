#include "labyrinth.hpp"

#include <algorithm>
#include <numeric>
#include <tuple>
#include <unordered_set>

namespace labyrinth {
namespace {
using Region = std::unordered_map<Id, const OpeningBlock*>;

bool matches_boss(const OpeningBlock& block, const Ids& allowed) {
    return allowed.empty() || std::any_of(block.boss_units.begin(), block.boss_units.end(), [&](Id id) {
        return std::find(allowed.begin(), allowed.end(), id) != allowed.end();
    });
}

void count_cell(OpeningMinimums& remaining, const OpeningBlock& block) {
    if (block.type >= 2 && remaining[block.type] > 0) --remaining[block.type];
}

int last_column_of(const Region& region) {
    int last_column = 0;
    for (const auto& entry : region) last_column = std::max(last_column, entry.second->column);
    return last_column;
}

struct RouteFailure {
    bool reaches_terminal = false;
    std::optional<OpeningMinimums> closest;
    int64_t closest_shortfall = 0;

    void observe(const OpeningMinimums& remaining, bool boss_matches) {
        reaches_terminal = true;
        if (!boss_matches) return;
        const auto total = std::accumulate(remaining.begin(), remaining.end(), int64_t{0});
        if (!closest || std::tie(total, remaining) < std::tie(closest_shortfall, *closest)) {
            closest = remaining;
            closest_shortfall = total;
        }
    }

    Json describe(const Region& region, const OpeningMinimums& minimums, const Ids& bosses) const {
        if (region.empty()) return {{"kind", "map_missing"}};
        if (!reaches_terminal) return {{"kind", "no_route"}};
        if (!closest) return {{"kind", "boss_mismatch"}, {"allowed_boss_unit_ids", bosses}};
        Json cells = Json::array();
        for (size_t type = 2; type < closest->size(); ++type) {
            const auto deficit = closest->at(type);
            if (deficit > 0) {
                cells.push_back({{"block_type", type}, {"required", minimums[type]},
                                 {"reached", minimums[type] - deficit}});
            }
        }
        return {{"kind", "cell_shortfall"}, {"cells", std::move(cells)}};
    }
};

bool has_path(const OpeningBlock& block, const Region& region, int last_column,
              OpeningMinimums remaining, const Ids& bosses, std::unordered_set<Id>& visited,
              int& search_steps, RouteFailure* failure = nullptr) {
    // Bound work even for a request containing an unusually dense graph.
    if (++search_steps > 100000) throw std::runtime_error("opening map search limit exceeded");
    if (!visited.insert(block.id).second) return false;
    count_cell(remaining, block);

    bool found = false;
    if (block.column == last_column) {
        const auto boss_matches = matches_boss(block, bosses);
        found = boss_matches &&
            std::all_of(remaining.begin(), remaining.end(), [](int count) { return count == 0; });
        if (failure && !found) failure->observe(remaining, boss_matches);
    } else {
        for (auto id : block.next) {
            auto next = region.find(id);
            if (next != region.end() &&
                has_path(*next->second, region, last_column, remaining, bosses, visited, search_steps, failure)) {
                found = true;
                break;
            }
        }
    }
    visited.erase(block.id);
    return found;
}

bool has_route(const Region& region, const OpeningMinimums& minimums, const Ids& bosses,
               RouteFailure* failure) {
    const int last_column = last_column_of(region);
    std::unordered_set<Id> visited;
    int search_steps = 0;
    for (const auto& entry : region) {
        const auto& block = *entry.second;
        if (block.column == 1 &&
            has_path(block, region, last_column, minimums, bosses, visited, search_steps, failure))
            return true;
    }
    return false;
}
} // namespace

Json plan_opening(const Request& request, const OpeningPolicy& policy, Json* diagnostics) {
    std::array<Region, 6> regions;
    for (const auto& block : request.opening_blocks) regions[block.region].emplace(block.id, &block);

    Json reply = {{"request_id", request.request_id}, {"action", "opening_accept"},
                  {"max_attempts", policy.max_attempts}};
    const int last_region = request.difficulty == 1 ? 3 : 5;
    for (int region = 1; region <= last_region; ++region) {
        const auto& minimums = policy.regions[region];
        const auto& bosses = policy.boss_units[region];
        RouteFailure failure;
        if (has_route(regions[region], minimums, bosses, diagnostics ? &failure : nullptr)) continue;
        reply["action"] = request.opening_attempt < policy.max_attempts ? "opening_retry" : "opening_stop";
        reply["rejected_region"] = region;
        if (diagnostics) (*diagnostics)["opening_failure"] = failure.describe(regions[region], minimums, bosses);
        break;
    }
    return reply;
}
Json plan_path(const Request& request, const OpeningPolicy& policy, Json* diagnostics) {
    Json reply = {{"request_id", request.request_id}, {"action", "noop"}};
    Region region;
    std::unordered_set<Id> visited;
    auto remaining = policy.regions[request.region];
    for (const auto& block : request.opening_blocks) {
        region.emplace(block.id, &block);
        if (block.cleared) {
            visited.insert(block.id);
            count_cell(remaining, block);
        }
    }

    const auto current = region.find(request.block_id);
    const auto last_column = last_column_of(region);
    if (current == region.end() || !current->second->cleared || current->second->column == last_column)
        return reply;

    int search_steps = 0;
    const auto& bosses = policy.boss_units[request.region];
    for (auto id : current->second->next) {
        const auto next = region.find(id);
        if (next == region.end() ||
            !has_path(*next->second, region, last_column, remaining, bosses, visited, search_steps))
            continue;
        reply["action"] = "move";
        reply["next_block_id"] = id;
        return reply;
    }
    if (diagnostics) (*diagnostics)["path_no_match"] = true;
    return reply;
}
} // namespace labyrinth
