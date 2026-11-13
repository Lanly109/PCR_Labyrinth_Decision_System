#include "labyrinth.hpp"

#include <algorithm>
#include <set>
#include <limits>
#include <unordered_set>

namespace labyrinth {
namespace {
Formation canonical(Formation parties) {
    for (auto& party : parties) std::sort(party.begin(), party.end());
    return parties;
}

bool complete(const Formation& parties, size_t party_count, size_t party_size) {
    if (parties.size() != party_count) return false;
    std::unordered_set<Id> used;
    for (const auto& party : parties) {
        if (party.size() != party_size) return false;
        for (auto id : party) if (!used.insert(id).second) return false;
    }
    return true;
}

bool form_team(TeamModel& model, Id quest, int seconds, const Ids& remaining, const Ids& relics, size_t party_count,
               const std::set<Formation>& defeated, TeamPlan& plan, int& budget,
               const Request& request, const Catalog& catalog, TeamSimulator* simulator, Json* diagnostics) {
    if (!budget) return false;
    if (plan.parties.size() == party_count) {
        --budget;
        return !defeated.count(canonical(plan.parties));
    }
    const auto limit = simulator ? std::numeric_limits<size_t>::max() : static_cast<size_t>(budget);
    auto ranked = model.ranked(quest, remaining, relics, seconds, limit);
    if (party_count == 1)
        std::erase_if(ranked, [&](const auto& candidate) { return defeated.contains(canonical({candidate.units})); });
    if (simulator && ranked.size() > 1) simulator->rank(request, catalog, ranked, diagnostics);
    if (diagnostics && party_count == 1 && catalog.quests.at(quest).type != 3) {
        auto& recommendations = (*diagnostics)["team_recommendations"] = Json::array();
        for (const auto& candidate : ranked) {
            recommendations.push_back({{"units", candidate.units}, {"win_probability", candidate.win_probability}});
            if (recommendations.size() == 3) break;
        }
    }
    const std::unordered_set<Id> available(remaining.begin(), remaining.end());
    for (auto& [units, win_probability] : ranked) {
        if (!budget) break;
        std::unordered_set<Id> used(units.begin(), units.end());
        if (units.size() != 5 || used.size() != 5 ||
            std::any_of(units.begin(), units.end(), [&](Id id) { return !available.count(id); }))
            throw std::runtime_error("model returned an invalid party");
        Ids next;
        for (auto id : remaining) if (!used.count(id)) next.push_back(id);
        plan.parties.push_back(std::move(units));
        plan.win_probabilities.push_back(win_probability);
        if (form_team(model, quest, seconds, next, relics, party_count, defeated, plan, budget,
                      request, catalog, simulator, diagnostics)) return true;
        plan.win_probabilities.pop_back();
        plan.parties.pop_back();
    }
    return false;
}
} // namespace

TeamPlanResult plan_team(const Request& req, int max_attempts,
                         const Catalog& catalog, TeamModel& model, bool ignore_npc,
                         TeamSimulator* simulator, Json* diagnostics, int third_boss_party_count) {
    auto quest_id = req.body.at("quest_id").get<Id>();
    auto party_count = req.body.at("party_count").get<size_t>();
    auto quest = catalog.quests.find(quest_id);
    if (req.difficulty != 5 || quest == catalog.quests.end() || !model.supports_quest(quest_id))
        return TeamPlanFailure::unsupported;
    const auto type = quest->second.type;
    if ((type == 3 && party_count != 3) || ((type == 1 || type == 2) && party_count != 1) ||
        (type != 1 && type != 2 && type != 3)) return TeamPlanFailure::unsupported;
    if (type == 3 && req.region == 3) party_count = static_cast<size_t>(third_boss_party_count);
    if (req.owned_units.empty()) return TeamPlanFailure::no_candidates;

    Ids non_npc;
    if (ignore_npc && !req.npc_units.empty()) {
        std::unordered_set<Id> seen;
        for (Id id : req.owned_units)
            if (!req.npc_units.contains(id) && seen.insert(id).second) non_npc.push_back(id);
    }
    const auto& owned = non_npc.size() >= 5 ? non_npc : req.owned_units;
    const size_t size = type == 3 ? 5 : std::min<size_t>(5, owned.size());
    std::set<Formation> defeated;
    for (const auto& attempt : req.body.at("past_attempts")) {
        if (attempt.at("outcome") != "defeat") continue;
        auto parties = attempt.at("parties").get<Formation>();
        if (complete(parties, party_count, size)) defeated.insert(canonical(std::move(parties)));
    }
    if (party_count == 1 && owned.size() <= 5) {
        Formation all{owned};
        if (!complete(all, 1, size) || defeated.count(canonical(all))) return TeamPlanFailure::no_candidates;
        return TeamPlan{std::move(all), {std::nullopt}};
    }

    Ids known;
    std::unordered_set<Id> seen;
    for (auto id : owned) {
        if (model.supports_unit(id) && seen.insert(id).second) known.push_back(id);
    }
    if (known.size() < party_count * 5) return TeamPlanFailure::no_candidates;
    TeamPlan result;
    if (form_team(model, quest_id, type == 3 ? 90 : 30, known, req.owned_relics,
                  party_count, defeated, result, max_attempts, req, catalog, simulator, diagnostics))
        return result;
    return TeamPlanFailure::no_candidates;
}
} // namespace labyrinth
