#include "labyrinth.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <span>

namespace labyrinth {
namespace {
constexpr size_t snapshots = 4;
constexpr size_t party_size = 5;
constexpr double score_tolerance = 1e-9;

int quest_region(Id id) {
    return (id / 100000) % 10;
}

struct Candidate {
    int choice = 0;
    Id unit = 0;
};

struct Goal {
    std::string key, name;
    Id quest;
    int weight, parties;
    bool boss;
};

std::vector<Goal> target_quests(const Request& req, const RecruitPolicy& policy, const Catalog& catalog) {
    std::map<int, Id> bosses;
    for (const auto& value : req.body.value("boss_quest_ids", Json::array())) {
        const Id id = value.get<Id>();
        const int region = quest_region(id);
        const auto quest = catalog.quests.find(id);
        if (region < req.region || !policy.boss_targets.contains(region) ||
            quest == catalog.quests.end() || quest->second.type != 3) continue;
        auto [entry, inserted] = bosses.emplace(region, id);
        if (!inserted) entry->second = std::min(entry->second, id);
    }
    std::vector<Goal> goals;
    for (const auto& [region, rule] : policy.boss_targets) {
        const auto quest = bosses.find(region);
        if (!rule.weight || (quest == bosses.end() &&
            (rule.parties == 1 || req.body.contains("boss_quest_ids")))) continue;
        goals.push_back({"boss_" + std::to_string(region), recruit_boss_label(region, rule.parties),
                         quest == bosses.end() ? 0 : quest->second, rule.weight, rule.parties, true});
    }

    const auto regions = req.body.value("future_ex_regions", Ids{1, 2, 3, 4, 5});
    for (const auto& target : catalog.recruitment.ex_targets) {
        const auto weight = policy.ex_weights.find(target.quest_ids.front());
        if (weight == policy.ex_weights.end() || !weight->second) continue;
        const auto quest = std::ranges::find_if(target.quest_ids, [&](Id id) {
            const int region = quest_region(id);
            return region >= req.region && std::ranges::find(regions, region) != regions.end();
        });
        if (quest != target.quest_ids.end())
            goals.push_back({std::to_string(*quest), target.name, *quest, weight->second, 1, false});
    }
    return goals;
}

struct RelicCoverage {
    bool guard = false, healing = false;
};

RelicCoverage relic_coverage(const Request& req, const Catalog& catalog) {
    int64_t guard_marks = 0;
    RelicCoverage result;
    for (Id id : req.owned_relics) {
        const auto relic = catalog.relics.find(id);
        if (relic == catalog.relics.end()) continue;
        if (relic->second.mark == 5) guard_marks += relic->second.mark_count;
        result.healing |= catalog.recruitment.healing_relics.contains(id);
    }
    result.guard = catalog.recruitment.guard_mark_threshold > 0 &&
                   guard_marks >= catalog.recruitment.guard_mark_threshold;
    return result;
}

double survival(const Ids& roster, const Catalog& catalog, const RelicCoverage& relics) {
    int tanks = 0, healers = 0;
    for (Id id : roster) {
        const auto role = catalog.unit_roles.find(id);
        if (role == catalog.unit_roles.end()) continue;
        healers += role->second == 6;
        tanks += role->second == 7;
    }
    return (std::min(1.0, tanks / (relics.guard ? 2.0 : 3.0)) +
            std::min(1.0, healers / (relics.healing ? 1.0 : 2.0))) / 2.0;
}

bool disjoint(const Party& left, const Party& right) {
    return std::ranges::none_of(left, [&](Id id) { return std::ranges::binary_search(right, id); });
}

double strongest(std::span<const RankedParty> parties) {
    double score = 0.0;
    for (const auto& party : parties) score = std::max(score, party.win_probability);
    return score;
}

double three_party_readiness(std::vector<RankedParty> parties) {
    std::ranges::sort(parties, std::greater{}, &RankedParty::win_probability);
    double best = 0.0;
    for (size_t a = 0; a < parties.size(); ++a) {
        const auto first = parties[a].win_probability;
        if (3 * first <= best) break;
        best = std::max(best, first);
        for (size_t b = a + 1; b < parties.size(); ++b) {
            const auto second = parties[b].win_probability;
            if (first + 2 * second <= best) break;
            if (!disjoint(parties[a].units, parties[b].units)) continue;
            best = std::max(best, first + second);
            for (size_t c = b + 1; c < parties.size(); ++c) {
                const auto total = first + second + parties[c].win_probability;
                if (total <= best) break;
                if (!disjoint(parties[a].units, parties[c].units) ||
                    !disjoint(parties[b].units, parties[c].units)) continue;
                best = total;
                break;
            }
        }
    }
    return (strongest(parties) + best / 3.0) / 2.0;
}

std::vector<RankedParty> seed_parties(TeamModel& model, Id quest, Ids& remaining, const Ids& relics,
                                     int seconds, bool multi) {
    std::vector<RankedParty> result;
    for (int round = 0; round < (multi ? 3 : 1) && remaining.size() >= party_size; ++round) {
        auto seeds = model.recruit_seeds(quest, remaining, relics, seconds);
        if (seeds.empty()) break;
        if (multi) {
            const auto& first = std::ranges::max_element(seeds, {}, &RankedParty::win_probability)->units;
            std::erase_if(remaining, [&](Id id) { return std::ranges::find(first, id) != first.end(); });
        }
        for (auto& seed : seeds) {
            std::ranges::sort(seed.units);
            result.push_back(std::move(seed));
        }
    }
    return result;
}

std::array<double, snapshots> compare_parties(TeamModel& model, Id quest, const Ids& roster, const Ids& relics,
                                              const std::array<Candidate, snapshots>& candidates,
                                              int seconds, bool multi) {
    auto remaining = roster;
    const auto base = seed_parties(model, quest, remaining, relics, seconds, multi);
    std::set<Party> variants;
    for (size_t i = 1; i < candidates.size(); ++i) {
        const Id unit = candidates[i].unit;
        if (!model.supports_unit(unit) || std::ranges::binary_search(roster, unit)) continue;
        if (remaining.size() == party_size - 1) {
            Party party = remaining;
            party.insert(std::ranges::lower_bound(party, unit), unit);
            variants.insert(std::move(party));
        }
        for (const auto& seed : base) {
            for (size_t slot = 0; slot < seed.units.size(); ++slot) {
                auto party = seed.units;
                party[slot] = unit;
                std::ranges::sort(party);
                variants.insert(std::move(party));
            }
        }
    }
    const auto scored = model.recruit_scores(quest, {variants.begin(), variants.end()}, relics, seconds);
    const auto rank = [multi](const auto& pool) {
        return multi ? three_party_readiness(pool) : strongest(pool);
    };
    std::array<double, snapshots> scores;
    scores.fill(rank(base));
    for (size_t i = 1; i < candidates.size(); ++i) {
        auto pool = base;
        for (const auto& party : scored)
            if (std::ranges::binary_search(party.units, candidates[i].unit)) pool.push_back(party);
        scores[i] = rank(pool);
    }
    return scores;
}
// The Critic requires five characters. Use the same future recruits in all
// snapshots and let each offered unit replace the same one in each scenario.
std::vector<Party> opening_samples(const Ids& roster, const std::array<Candidate, snapshots>& candidates,
                                   const Catalog& catalog, TeamModel& model) {
    constexpr size_t sample_count = 8;
    if (roster.size() >= party_size - 1) return {};

    Ids fillers;
    for (const auto& [unit, role] : catalog.unit_roles) {
        if (model.supports_unit(unit) && !std::ranges::binary_search(roster, unit) &&
            std::ranges::none_of(candidates, [unit](const auto& candidate) { return candidate.unit == unit; }))
            fillers.push_back(unit);
    }
    std::ranges::sort(fillers);
    std::mt19937 random(20261108);
    std::ranges::shuffle(fillers, random);

    const auto missing = party_size - roster.size();
    const auto count = std::min(sample_count, fillers.size() / missing);
    std::vector<Party> parties;
    parties.reserve(count * snapshots);
    for (size_t sample = 0; sample < count; ++sample) {
        Party baseline = roster;
        const auto first = fillers.begin() + static_cast<std::ptrdiff_t>(sample * missing);
        baseline.insert(baseline.end(), first, first + static_cast<std::ptrdiff_t>(missing));
        for (const auto& candidate : candidates) {
            Party party = baseline;
            if (candidate.unit && model.supports_unit(candidate.unit) &&
                !std::ranges::binary_search(roster, candidate.unit))
                party[roster.size()] = candidate.unit;
            std::ranges::sort(party);
            parties.push_back(std::move(party));
        }
    }
    return parties;
}

std::array<double, snapshots> score_opening(TeamModel& model, Id quest, const std::vector<Party>& parties,
                                            const Ids& relics, int seconds) {
    const auto scored = model.recruit_scores(quest, parties, relics, seconds);
    if (scored.size() != parties.size()) throw std::logic_error("Unexpected opening recruitment score count");
    std::array<double, snapshots> result{};
    for (size_t i = 0; i < scored.size(); ++i)
        result[i % snapshots] += scored[i].win_probability;
    for (auto& score : result) score /= static_cast<double>(scored.size() / snapshots);
    return result;
}
} // namespace

int plan_recruitment(const Request& req, const RecruitPolicy& policy, const Catalog& catalog,
                     TeamModel* model, Json* diagnostics) {
    const auto& choices = req.body.at("choices");
    if (req.difficulty != policy.difficulty || choices.size() != snapshots - 1) return 0;

    std::array<Candidate, snapshots> candidates{};
    Ids owned = req.owned_units;
    std::ranges::sort(owned);
    owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
    std::array<Ids, snapshots> rosters;
    rosters[0] = owned;
    for (size_t i = 1; i < candidates.size(); ++i) {
        const auto& choice = choices.at(i - 1);
        const auto unit = choice.at("unit_id").get<Id>();
        candidates[i] = {choice.at("choice_num").get<int>(), unit};
        rosters[i] = owned;
        const auto place = std::ranges::lower_bound(rosters[i], unit);
        if (place == rosters[i].end() || *place != unit) rosters[i].insert(place, unit);
    }

    auto known = owned;
    if (model) std::erase_if(known, [&](Id id) { return !model->supports_unit(id); });
    const auto goals = target_quests(req, policy, catalog);
    const auto relics = relic_coverage(req, catalog);
    const auto opening = model ? opening_samples(known, candidates, catalog, *model) : std::vector<Party>{};
    bool projected = false;
    std::array<double, snapshots> totals{};
    int64_t total_weight = 0;
    Json target_log = Json::array();
    for (const auto& [key, name, quest, weight, parties, boss] : goals) {
        const bool multi = parties == 3;
        std::array<double, snapshots> scores{};
        const bool has_model = model && quest && (!opening.empty() || known.size() >= party_size - 1) &&
                               model->supports_quest(quest);
        const int seconds = boss ? 90 : 30;
        if (has_model && !opening.empty()) {
            scores = score_opening(*model, quest, opening, req.owned_relics, seconds);
            if (multi) for (auto& score : scores) score *= 2.0 / 3.0;
            projected = true;
        } else if (has_model) {
            scores = compare_parties(*model, quest, known, req.owned_relics, candidates, seconds, multi);
        }
        if (!has_model && !multi) continue;
        for (size_t i = 0; i < scores.size(); ++i) {
            if (multi) scores[i] = 0.8 * scores[i] + 0.2 * survival(rosters[i], catalog, relics);
            totals[i] += weight * scores[i];
        }
        total_weight += weight;
        if (diagnostics) target_log.push_back({{"target", key}, {"label", name}, {"quest_id", quest},
            {"weight", weight}, {"model", has_model}, {"scores", scores}});
    }

    size_t best = 0;
    for (size_t i = 1; i < totals.size(); ++i) {
        if (rosters[i] == owned) continue;
        const bool compared = projected && model->supports_unit(candidates[i].unit);
        if (!compared && totals[i] <= totals[0] + score_tolerance) continue;
        if (best == 0 || totals[i] > totals[best] + score_tolerance) best = i;
    }
    if (best == 0 && std::ranges::all_of(totals, [baseline = totals[0]](double score) {
            return std::abs(score - baseline) <= score_tolerance;
        })) {
        std::array<size_t, snapshots - 1> available{};
        size_t count = 0;
        for (size_t i = 1; i < rosters.size(); ++i) {
            if (rosters[i] != owned) available[count++] = i;
        }
        if (count > 0) {
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<size_t> pick(0, count - 1);
            best = available[pick(rng)];
        }
    }
    if (diagnostics) {
        auto& record = (*diagnostics)["unit_recruitment"];
        record = {{"targets", std::move(target_log)},
                  {"relic_guard", relics.guard}, {"relic_healing", relics.healing},
                  {"boss_context", req.body.contains("boss_quest_ids")},
                  {"projected_samples", projected ? opening.size() / snapshots : 0}};
        auto& gains = record["candidates"] = Json::array();
        for (size_t i = 1; i < totals.size(); ++i)
            gains.push_back({{"unit_id", candidates[i].unit}, {"choice_num", candidates[i].choice},
                             {"gain", total_weight ? (totals[i] - totals[0]) / total_weight : 0.0}});
    }
    return candidates[best].choice;
}
} // namespace labyrinth
