#include "labyrinth.hpp"

#include <algorithm>
#include <iterator>
#include <random>
#include <span>
#include <tuple>
#include <unordered_set>

namespace labyrinth {
namespace {
int priority(const Ids& ordered, int id) {
    auto it = std::find(ordered.begin(), ordered.end(), id);
    return it == ordered.end() ? 0 : static_cast<int>(ordered.end() - it);
}

Ids select_units(const Request& req, const Config& config, const Catalog& catalog) {
    const auto id = req.body.at("ticket_id").get<Id>();
    std::span<const Id> pool;
    const auto ticket = std::ranges::find(catalog.opening_tickets, id, &OpeningTicket::ticket_id);
    const bool opening = ticket != catalog.opening_tickets.end();
    if (opening) {
        if (!config.opening_ticket) return {};
        const auto configured = config.opening_ticket_units.find(id);
        pool = configured == config.opening_ticket_units.end()
            ? std::span<const Id>(ticket->default_units) : std::span<const Id>(configured->second);
    } else {
        if (!config.select_ticket) return {};
        const auto configured = config.ticket_priority_units.find(id);
        if (configured == config.ticket_priority_units.end()) return {};
        pool = configured->second;
    }
    const auto candidates = req.body.at("candidate_unit_ids").get<Ids>();
    const std::unordered_set<Id> available(candidates.begin(), candidates.end());
    auto count = req.body.at("select_count").get<size_t>();
    Ids result;
    for (auto id : pool) {
        if (!available.count(id)) continue;
        result.push_back(id);
        if (!opening && result.size() == count) return result;
    }
    if (result.size() < count) return {};
    if (result.size() == count) return result;
    static thread_local std::mt19937 rng{std::random_device{}()};
    Ids sampled;
    std::sample(result.begin(), result.end(), std::back_inserter(sampled), count, rng);
    return sampled;
}

int ticket_choice(const Request& req, const Config& config) {
    std::pair<int, Id> best{-1, 0};
    int number = 0;
    for (const auto& choice : req.body.at("choices")) {
        Id id = choice.at("ticket_id").get<Id>();
        const auto score = std::make_pair(priority(config.talent_priority, (id / 100000) % 10), id);
        if (score > best) {
            best = score;
            number = choice.at("choice_num").get<int>();
        }
    }
    return number;
}

int relic_choice(const Request& req, const Config& config, const Catalog& catalog) {
    std::unordered_map<int, int64_t> owned_marks;
    for (auto id : req.owned_relics) {
        auto relic = catalog.relics.find(id);
        if (relic != catalog.relics.end())
            owned_marks[relic->second.mark] += relic->second.mark_count;
    }
    std::tuple<int, int, int, int64_t, Id> best{-1, 0, 0, 0, 0};
    int number = 0;
    for (const auto& choice : req.body.at("choices")) {
        auto id = choice.at("relic_id").get<Id>();
        auto relic = catalog.relics.find(id);
        if (relic == catalog.relics.end()) return 0;
        const auto& r = relic->second;
        const auto score = std::make_tuple(r.rarity, priority(config.mark_priority, r.mark),
                                           r.mark_count, owned_marks[r.mark], id);
        if (score > best) {
            best = score;
            number = choice.at("choice_num").get<int>();
        }
    }
    return number;
}

bool can_choose(const Request& req, int number) {
    for (const auto& option : req.body.at("options")) {
        if (option.at("choice_num") != number) continue;
        const auto type = option.at("condition_type").get<int>();
        return type == 0 || (type == 1 && req.gold >= option.at("condition_value").get<int>());
    }
    return false;
}

int event_choice(const Request& req, const Config& config) {
    const auto id = req.body.at("event_id").get<Id>();
    const auto choice = config.fixed_event_choices.find(id);
    return choice != config.fixed_event_choices.end() && can_choose(req, choice->second) ? choice->second : 0;
}
} // namespace

Json decide(const Request& req, const Config& config, const Catalog& catalog, TeamModel* model,
            Json* diagnostics, TeamSimulator* simulator) {
    Json reply = {{"request_id", req.request_id}, {"action", "noop"}};
    int choice = 0;
    switch (req.kind) {
    case Kind::select_ticket:
        if (auto units = select_units(req, config, catalog); !units.empty()) {
            reply["action"] = "select";
            reply["unit_ids"] = std::move(units);
        }
        break;
    case Kind::unit_choice:
        if (config.recruitment.enabled) choice = plan_recruitment(req, config.recruitment, catalog, model, diagnostics);
        break;
    case Kind::ticket_choice:
        if (config.ticket_choice) choice = ticket_choice(req, config);
        break;
    case Kind::relic_choice:
        if (config.relic_choice) choice = relic_choice(req, config, catalog);
        break;
    case Kind::event:
        if (config.event) choice = event_choice(req, config);
        break;
    case Kind::shop:
        if (config.shop.enabled) return plan_shop(req, config.shop, catalog);
        break;
    case Kind::score_check:
        if (config.score_farm) return check_score(req, config, catalog);
        break;
    case Kind::ticket_clear: {
        if (req.body.at("ticket_count").get<int>() <= config.passport_reserve) break;
        const auto& guilds = config.ticket_clear_guilds;
        const auto current = std::ranges::find(guilds, req.body.at("guild_id").get<Id>());
        const auto start = current == guilds.end() ? 0 : static_cast<std::size_t>(current - guilds.begin()) + 1;
        for (std::size_t offset = 0; offset < guilds.size(); ++offset) {
            const auto guild = guilds[(start + offset) % guilds.size()];
            if (!catalog.guild_ids.contains(guild)) continue;
            reply["action"] = "continue_run";
            reply["guild_id"] = guild;
            break;
        }
        break;
    }
    case Kind::path:
        if (config.path) return plan_path(req, config.opening, diagnostics);
        break;
    case Kind::opening:
        if (config.opening.enabled) return plan_opening(req, config.opening, diagnostics);
        break;
    case Kind::team:
        if (config.team) {
            auto quest = catalog.quests.find(req.body.at("quest_id").get<Id>());
            if (quest == catalog.quests.end() || quest->second.type < 1 || quest->second.type > 3 ||
                !config.team_scopes.at(quest->second.type - 1)) break;
            const auto& history = req.body.at("past_attempts");
            const auto defeats = std::count_if(history.begin(), history.end(), [](const auto& attempt) {
                return attempt.at("outcome") == "defeat";
            });
            const bool retrying = req.enter_id > 0 && req.block_id > 0 && defeats > 0;
            if (!retrying || defeats <= config.battle_max_retries) {
                if (!model) throw std::logic_error("team policy enabled without a model");
                const auto budget = static_cast<int>(std::clamp<decltype(defeats + 1)>(defeats + 1, 8, 128));
                auto result = plan_team(req, budget, catalog, *model, config.team_ignore_npc, simulator, diagnostics,
                                        config.third_boss_party_count);
                if (auto* plan = std::get_if<TeamPlan>(&result)) {
                    reply["action"] = "team";
                    reply["parties"] = std::move(plan->parties);
                    if (diagnostics) {
                        auto& probabilities = (*diagnostics)["party_win_probabilities"] = Json::array();
                        for (const auto probability : plan->win_probabilities)
                            probabilities.push_back(probability ? Json(*probability) : Json(nullptr));
                    }
                    break;
                }
                if (!retrying || std::get<TeamPlanFailure>(result) != TeamPlanFailure::no_candidates) break;
                if (diagnostics) (*diagnostics)["battle_retry_no_team"] = true;
            }
            reply["action"] = config.restart_at_battle_retry_limit ? "retry_run" : "manual";
            reply["defeat_count"] = defeats;
            reply["max_retries"] = config.battle_max_retries;
        }
        break;
    }
    if (choice) {
        reply["action"] = "select";
        reply["choice_num"] = choice;
    }
    return reply;
}
} // namespace labyrinth
