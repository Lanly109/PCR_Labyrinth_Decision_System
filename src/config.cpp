#include "labyrinth.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <numeric>
#include <regex>
#include <set>

namespace labyrinth {
namespace {
using Keys = std::initializer_list<const char*>;

void keys(const Json& obj, Keys names) {
    if (!obj.is_object()) throw std::runtime_error("expected object");
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (std::find(names.begin(), names.end(), it.key()) == names.end())
            throw std::runtime_error("unknown field: " + it.key());
    }
}

int number(const Json& v, int min = 0, int max = INT_MAX) {
    if (!v.is_number_integer() || (v.is_number_unsigned() && v.get<uint64_t>() > uint64_t(max)))
        throw std::runtime_error("expected integer in range");
    auto n = v.get<int64_t>();
    if (n < min || n > max) throw std::runtime_error("integer out of range");
    return static_cast<int>(n);
}

int number(const Json& obj, const char* key, int min = 0, int max = INT_MAX) {
    try { return number(obj.at(key), min, max); }
    catch (const std::exception& e) { throw std::runtime_error(std::string(key) + ": " + e.what()); }
}

bool boolean(const Json& obj, const char* key) {
    const auto& v = obj.at(key);
    if (!v.is_boolean()) throw std::runtime_error(std::string(key) + ": expected boolean");
    return v.get<bool>();
}

std::string string(const Json& obj, const char* key) {
    auto s = obj.at(key).get<std::string>();
    if (s.empty() || s.find('\0') != std::string::npos)
        throw std::runtime_error(std::string(key) + ": expected nonempty string");
    return s;
}

int numeric_key(const std::string& s, int min, int max) {
    int n = 0;
    auto end = s.data() + s.size();
    auto result = std::from_chars(s.data(), end, n);
    if (result.ec != std::errc{} || result.ptr != end || n < min || n > max || std::to_string(n) != s)
        throw std::runtime_error("invalid numeric key: " + s);
    return n;
}

Ids priority(const Json& v, size_t required_size = 0, int max = INT_MAX) {
    if (!v.is_array() || (required_size && v.size() != required_size))
        throw std::runtime_error("invalid priority array length");
    Ids result;
    std::set<int> seen;
    for (const auto& x : v) {
        auto id = number(x, 1, max);
        if (!seen.insert(id).second) throw std::runtime_error("duplicate priority ID");
        result.push_back(id);
    }
    return result;
}

Ids opening_units(const Json& value, size_t required_size = 0) {
    auto units = priority(value, required_size);
    const auto fes = std::ranges::find_if(units, is_fes_unit);
    if (fes != units.end())
        throw std::runtime_error("FES unit " + std::to_string(*fes) + " is not allowed in an opening ticket");
    return units;
}

template<size_t N>
std::array<int, N> numbers(const Json& v, int min, int max) {
    if (!v.is_array() || v.size() != N) throw std::runtime_error("invalid model array length");
    std::array<int, N> result{};
    for (size_t i = 0; i < N; ++i) result[i] = number(v[i], min, max);
    return result;
}

OpeningPolicy opening_policy(const Json& v) {
    keys(v, {"enabled", "max_attempts", "regions", "boss_unit_ids"});
    OpeningPolicy policy;
    policy.enabled = boolean(v, "enabled");
    if (v.contains("max_attempts")) policy.max_attempts = number(v, "max_attempts", 1);
    if (v.contains("regions")) {
        const auto& regions = v.at("regions");
        if (!regions.is_object()) throw std::runtime_error("opening.regions must be an object");
        for (auto it = regions.begin(); it != regions.end(); ++it) {
            auto& minimums = policy.regions[numeric_key(it.key(), 1, 5)];
            keys(*it, {"normal", "hard", "ticket", "event", "relic", "shop", "boss"});
            for (size_t i = 0; i < opening_cell_names.size(); ++i) {
                const auto name = opening_cell_names[i];
                if (it->contains(name)) minimums[i + 2] = number(*it, name);
            }
        }
    }
    if (v.contains("boss_unit_ids")) {
        const auto& bosses = v.at("boss_unit_ids");
        keys(bosses, {"3", "5"});
        for (const auto region : {3, 5}) {
            auto key = std::to_string(region);
            if (bosses.contains(key)) policy.boss_units[region] = priority(bosses.at(key));
        }
    }
    return policy;
}

ShopModel shop_model(const Json& v) {
    keys(v, {"base_prices", "refresh_fees", "scores", "regions"});
    ShopModel m{};
    const auto& prices = v.at("base_prices");
    keys(prices, {"random_imprint", "choice_imprint", "relic"});
    m.random_imprint_price = number(prices, "random_imprint", 1);
    m.choice_imprint_price = number(prices, "choice_imprint", 1);
    m.relic_prices = numbers<3>(prices.at("relic"), 1, INT_MAX);
    m.refresh_fees = numbers<2>(v.at("refresh_fees"), 0, INT_MAX);
    const auto& scores = v.at("scores");
    keys(scores, {"imprint", "relic"});
    m.imprint_score = number(scores, "imprint", 1);
    m.relic_score = number(scores, "relic", 1);
    const auto& regions = v.at("regions");
    keys(regions, {"2", "3", "4", "5"});
    for (auto it = regions.begin(); it != regions.end(); ++it) {
        keys(*it, {"random_slots", "choice_slots", "relic_slots", "relic_star_probabilities", "fixed_relic_ids"});
        ShopForecast f{};
        f.random_slots = number(*it, "random_slots", 0, shop_imprint_slots);
        f.choice_slots = number(*it, "choice_slots", 0, shop_imprint_slots);
        if (f.random_slots + f.choice_slots != shop_imprint_slots)
            throw std::runtime_error("shop forecast requires three imprint slots");
        if (it->contains("fixed_relic_ids")) {
            if (it->contains("relic_slots") || it->contains("relic_star_probabilities"))
                throw std::runtime_error("fixed relics and random relic slots are mutually exclusive");
            f.fixed_relic_ids = priority(it->at("fixed_relic_ids"), shop_max_relic_slots);
        } else {
            f.relic_slots = number(*it, "relic_slots", 0, shop_max_relic_slots);
            const auto& by_reset = it->at("relic_star_probabilities");
            if (!by_reset.is_array() || by_reset.size() != 3)
                throw std::runtime_error("expected relic probabilities for reset counts 0, 1, 2");
            for (size_t reset = 0; reset < by_reset.size(); ++reset) {
                const auto& row = by_reset[reset];
                if (!row.is_array() || row.size() != 3) throw std::runtime_error("expected three star probabilities");
                auto& probabilities = f.relic_star_probabilities[reset];
                for (size_t i = 0; i < row.size(); ++i) {
                    if (!row[i].is_number()) throw std::runtime_error("expected numeric probability");
                    auto p = row[i].get<double>();
                    if (!std::isfinite(p) || p < 0 || p > 1) throw std::runtime_error("invalid probability");
                    probabilities[i] = p;
                }
                auto total = std::accumulate(probabilities.begin(), probabilities.end(), 0.0);
                if (total <= 0) throw std::runtime_error("all star probabilities are zero");
                for (auto& p : probabilities) p /= total;
            }
        }
        m.regions.emplace(numeric_key(it.key(), 2, 5), std::move(f));
    }
    return m;
}

ShopPolicy shop_policy(const Json& v) {
    keys(v, {"enabled", "regions", "model"});
    ShopPolicy s;
    s.enabled = boolean(v, "enabled");
    if (v.contains("model")) s.model = shop_model(v.at("model"));
    if (v.contains("regions")) {
        const auto& regions = v.at("regions");
        keys(regions, {"2", "3", "4", "5"});
        for (auto it = regions.begin(); it != regions.end(); ++it) {
            keys(*it, {"purchase", "reserve_gold"});
            auto region = numeric_key(it.key(), 2, 5);
            auto purchase = boolean(*it, "purchase");
            if (purchase && !s.model.regions.count(region))
                throw std::runtime_error("missing shop forecast for region " + it.key());
            s.regions.emplace(region, ShopRegion{purchase, number(*it, "reserve_gold")});
        }
    }
    return s;
}

template<class T>
void index(std::unordered_map<Id, T>& map, Id id, T row) {
    if (!map.emplace(id, std::move(row)).second) throw std::runtime_error("duplicate catalog ID");
}
} // namespace

std::string recruit_boss_label(int region, int parties) {
    return "第 " + std::to_string(region) + " 层首领" +
           (parties == 3 ? " · 三队通关" : " · 一刀");
}

Config parse_config(const Json& v) {
    try {
        keys(v, {"schema_version", "server", "model", "policies"});
        if (number(v, "schema_version") != 1) throw std::runtime_error("unsupported schema_version");
        Config c;
        if (v.contains("server")) {
            const auto& server = v.at("server");
            keys(server, {"host", "port", "catalog", "log_history_limit"});
            if (server.contains("log_history_limit")) {
                c.log_history_limit = number(server, "log_history_limit");
                if (std::ranges::find(log_history_limits, c.log_history_limit) == log_history_limits.end())
                    throw std::runtime_error("日志保留条数只支持 500、1000、2000 或无限");
            }
            if (server.contains("host")) c.host = string(server, "host");
            if (server.contains("port")) c.port = number(server, "port", 1, 65535);
            if (server.contains("catalog")) c.catalog = path_from_utf8(string(server, "catalog"));
        }
        if (v.contains("model")) {
            const auto& model = v.at("model");
            keys(model, {"directory", "threads", "candidates", "candidate_seed", "use_gpu", "gpu_adapter"});
            if (model.contains("use_gpu")) c.model.use_gpu = boolean(model, "use_gpu");
            if (model.contains("gpu_adapter") && !model.at("gpu_adapter").is_null())
                c.model.gpu_adapter = string(model, "gpu_adapter");
            if (model.contains("directory")) c.model.directory = path_from_utf8(string(model, "directory"));
            if (model.contains("threads")) c.model.threads = number(model, "threads", 1, 128);
            if (model.contains("candidates")) c.model.candidates = number(model, "candidates", 1, 128);
            if (model.contains("candidate_seed")) {
                const auto& seed = model.at("candidate_seed");
                if (!seed.is_number_integer() || (!seed.is_number_unsigned() && seed.get<int64_t>() < 0))
                    throw std::runtime_error("candidate_seed must be a nonnegative integer");
                c.model.candidate_seed = seed.get<uint64_t>();
            }
        }
        if (!v.contains("policies")) return c;
        const auto& p = v.at("policies");
        keys(p, {"select_ticket", "unit_choice_targets", "ticket_choice", "relic_choice", "event", "shop", "team", "opening", "path", "score_farm", "ticket_clear"});
        if (p.contains("select_ticket")) {
            const auto& s = p.at("select_ticket");
            keys(s, {"enabled", "opening_enabled", "ticket_priority_units", "opening_ticket_units"});
            c.select_ticket = boolean(s, "enabled");
            if (s.contains("opening_enabled")) c.opening_ticket = boolean(s, "opening_enabled");
            if (s.contains("ticket_priority_units")) {
                const auto& lists = s.at("ticket_priority_units");
                if (!lists.is_object()) throw std::runtime_error("ticket_priority_units must be an object");
                for (const auto& [key, value] : lists.items())
                    c.ticket_priority_units.emplace(numeric_key(key, 1, INT_MAX), priority(value));
            }
            if (s.contains("opening_ticket_units")) {
                const auto& opening = s.at("opening_ticket_units");
                if (!opening.is_object()) throw std::runtime_error("opening_ticket_units must be an object");
                for (const auto& [key, value] : opening.items()) {
                    const auto id = numeric_key(key, 1, INT_MAX);
                    auto units = opening_units(value);
                    if (units.size() < opening_pick_count)
                        throw std::runtime_error("opening ticket " + key + " requires at least three units");
                    c.opening_ticket_units.emplace(id, std::move(units));
                }
            }
        }
        if (p.contains("unit_choice_targets")) {
            const auto& targets = p.at("unit_choice_targets");
            keys(targets, {"enabled", "difficulty", "boss_targets", "ex_weights"});
            auto& policy = c.recruitment;
            policy.enabled = boolean(targets, "enabled");
            policy.difficulty = number(targets, "difficulty", 1);
            const auto& bosses = targets.at("boss_targets");
            const auto& ex = targets.at("ex_weights");
            if (!bosses.is_object() || !ex.is_object())
                throw std::runtime_error("角色选择目标应为对象");
            for (const auto& [region, rule] : bosses.items()) {
                keys(rule, {"weight", "parties"});
                const int parties = number(rule, "parties", 1, 3);
                if (parties == 2) throw std::runtime_error("首领目标队伍数应为 1 或 3");
                policy.boss_targets.emplace(numeric_key(region, 1, 5),
                    RecruitBossTarget{number(rule, "weight"), parties});
            }
            for (const auto& [quest, weight] : ex.items())
                policy.ex_weights.emplace(numeric_key(quest, 1, INT_MAX), number(weight));
            if (policy.enabled &&
                std::ranges::none_of(policy.boss_targets, [](const auto& entry) { return entry.second.weight > 0; }) &&
                std::ranges::none_of(policy.ex_weights, [](const auto& entry) { return entry.second > 0; }))
                throw std::runtime_error("角色选择至少需要一项非零权重");
        }
        if (p.contains("ticket_choice")) {
            const auto& s = p.at("ticket_choice");
            keys(s, {"enabled", "talent_priority"});
            c.ticket_choice = boolean(s, "enabled");
            if (c.ticket_choice || s.contains("talent_priority"))
                c.talent_priority = priority(s.at("talent_priority"), 5, 5);
        }
        if (p.contains("relic_choice")) {
            const auto& s = p.at("relic_choice");
            keys(s, {"enabled", "mark_priority"});
            c.relic_choice = boolean(s, "enabled");
            if (c.relic_choice || s.contains("mark_priority")) c.mark_priority = priority(s.at("mark_priority"), 5, 5);
        }
        if (p.contains("event")) {
            const auto& event = p.at("event");
            keys(event, {"enabled", "fixed_choices"});
            c.event = boolean(event, "enabled");
            if (event.contains("fixed_choices")) {
                const auto& choices = event.at("fixed_choices");
                if (!choices.is_object()) throw std::runtime_error("fixed_choices must be an object");
                for (const auto& [key, choice] : choices.items())
                    c.fixed_event_choices.emplace(numeric_key(key, 1, INT_MAX), number(choice, 1));
            }
        }
        if (p.contains("team")) {
            const auto& s = p.at("team");
            keys(s, {"enabled", "scopes", "battle_retry", "ignore_npc", "simulator", "third_boss_party_count"});
            c.team = boolean(s, "enabled");
            if (s.contains("third_boss_party_count"))
                c.third_boss_party_count = number(s, "third_boss_party_count", 1, 3);
            if (s.contains("simulator")) {
                const auto& simulator = s.at("simulator");
                keys(simulator, {"enabled", "url"});
                c.simulator.enabled = boolean(simulator, "enabled");
                c.simulator.url = string(simulator, "url");
                static const std::regex url(R"(http://(?:\[[a-fA-F\d:]+\]|[^:/?#\s]+)(?::(\d{1,5}))?(?:/[^#\s]*)?)");
                std::smatch match;
                if (!std::regex_match(c.simulator.url, match, url))
                    throw std::runtime_error("模拟器 URL 应为 HTTP 地址，可包含 /simulate 路径");
                if (match[1].matched) {
                    const auto port = std::stoi(match[1].str());
                    if (port < 1 || port > 65535) throw std::runtime_error("模拟器端口应为 1 至 65535");
                }
            }
            if (s.contains("ignore_npc")) c.team_ignore_npc = boolean(s, "ignore_npc");
            if (s.contains("battle_retry")) {
                const auto& retry = s.at("battle_retry");
                keys(retry, {"max_retries", "on_limit"});
                if (retry.contains("max_retries"))
                    c.battle_max_retries = number(retry, "max_retries");
                if (retry.contains("on_limit")) {
                    const auto action = string(retry, "on_limit");
                    if (action != "manual" && action != "restart")
                        throw std::runtime_error("battle_retry.on_limit must be manual or restart");
                    c.restart_at_battle_retry_limit = action == "restart";
                }
            }
            if (s.contains("scopes")) {
                const auto& scopes = s.at("scopes");
                keys(scopes, {"normal", "ex", "boss"});
                const std::array<const char*, 3> names{"normal", "ex", "boss"};
                for (size_t i = 0; i < names.size(); ++i)
                    if (scopes.contains(names[i])) c.team_scopes[i] = boolean(scopes, names[i]);
            }
        }
        if (p.contains("score_farm")) {
            const auto& farm = p.at("score_farm");
            keys(farm, {"enabled", "minimum_score_by_guild"});
            c.score_farm = boolean(farm, "enabled");
            if (farm.contains("minimum_score_by_guild")) {
                const auto& minima = farm.at("minimum_score_by_guild");
                if (!minima.is_object()) throw std::runtime_error("minimum_score_by_guild must be an object");
                for (auto it = minima.begin(); it != minima.end(); ++it)
                    c.guild_minimum_scores.emplace(numeric_key(it.key(), 1, INT_MAX), number(*it));
            }
        }
        if (p.contains("ticket_clear")) {
            const auto& clear = p.at("ticket_clear");
            keys(clear, {"reserve_tickets", "guild_ids"});
            if (clear.contains("reserve_tickets")) c.passport_reserve = number(clear, "reserve_tickets", -1);
            if (clear.contains("guild_ids")) {
                c.ticket_clear_guilds = priority(clear.at("guild_ids"));
                if (c.ticket_clear_guilds.empty())
                    throw std::runtime_error("ticket_clear.guild_ids must contain at least one guild");
            }
        }
        if (p.contains("opening")) c.opening = opening_policy(p.at("opening"));
        if (p.contains("path")) {
            const auto& path = p.at("path");
            keys(path, {"enabled"});
            c.path = boolean(path, "enabled");
        }
        if (p.contains("shop")) c.shop = shop_policy(p.at("shop"));
        return c;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Invalid labyrinth config: ") + e.what());
    }
}

std::vector<OpeningTicket> parse_opening_tickets(const Json& catalog) {
    const auto& rows = catalog.at("opening_tickets");
    if (!rows.is_array()) throw std::runtime_error("expected opening_tickets array");
    std::vector<OpeningTicket> tickets;
    std::set<Id> ids;
    for (const auto& row : rows) {
        keys(row, {"ticket_id", "guild_id", "default_unit_ids"});
        const auto id = number(row, "ticket_id", 1);
        if (!ids.insert(id).second) throw std::runtime_error("duplicate opening ticket: " + std::to_string(id));
        tickets.push_back({id, number(row, "guild_id", 1), opening_units(row.at("default_unit_ids"), opening_pick_count)});
    }
    return tickets;
}

std::vector<SelectTicketRoster> parse_select_ticket_rosters(const Json& catalog) {
    const auto& rows = catalog.at("select_ticket_rosters");
    if (!rows.is_array()) throw std::runtime_error("expected select_ticket_rosters array");
    std::vector<SelectTicketRoster> tickets;
    std::set<Id> ids;
    for (const auto& row : rows) {
        keys(row, {"ticket_id", "label", "unit_ids"});
        const auto id = number(row, "ticket_id", 1);
        if (!ids.insert(id).second) throw std::runtime_error("duplicate select ticket: " + std::to_string(id));
        tickets.push_back({id, string(row, "label"), priority(row.at("unit_ids"))});
    }
    return tickets;
}

std::vector<RecruitExTarget> parse_recruit_ex_targets(const Json& data) {
    if (!data.contains("recruitment")) return {};
    const auto rows = data.at("recruitment").value("ex_quests", Json::array());
    if (!rows.is_array()) throw std::runtime_error("expected recruitment EX array");

    // The suffix identifies an encounter. The full quest IDs identify its floor variants.
    std::map<std::pair<Id, Id>, std::string> quests;
    for (const auto& row : rows) {
        keys(row, {"quest_id", "name"});
        const auto quest = number(row, "quest_id", 1);
        if (!quests.emplace(std::pair{quest % 100000, quest}, string(row, "name")).second)
            throw std::runtime_error("duplicate recruitment EX quest");
    }
    std::vector<RecruitExTarget> targets;
    Id encounter = -1;
    for (auto& [key, name] : quests) {
        const auto [suffix, quest] = key;
        if (suffix != encounter) {
            targets.push_back({std::move(name), {}});
            encounter = suffix;
        }
        targets.back().quest_ids.push_back(quest);
    }
    return targets;
}

Catalog parse_catalog(const Json& data) {
    try {
        if (number(data, "schema_version") != 1) throw std::runtime_error("unsupported schema_version");
        if (!data.at("source_db").is_object()) throw std::runtime_error("expected source_db metadata");
        const auto& t = data.at("tables");
        for (auto name : {"unit_data", "labyrinth_relic", "labyrinth_relic_mark_series",
             "labyrinth_summon_unit", "labyrinth_quest_data", "labyrinth_quest_data_display",
             "labyrinth_ex_quest_data_display", "labyrinth_event", "labyrinth_event_choice",
             "labyrinth_coin_shop_lineup", "labyrinth_enter_guild", "labyrinth_wave_group_data", "unit_role_mastery_id"}) {
            if (!t.at(name).is_array()) throw std::runtime_error(std::string("expected table array: ") + name);
        }
        Catalog c;
        for (const auto& wave : t.at("labyrinth_wave_group_data")) {
            Ids enemies;
            for (int slot = 1; slot <= 5; ++slot) {
                const auto key = "enemy_id_" + std::to_string(slot);
                const auto enemy = number(wave, key.c_str());
                if (enemy > 0) enemies.push_back(enemy);
            }
            index(c.wave_enemies, number(wave, "wave_group_id", 1), std::move(enemies));
        }
        for (const auto& row : t.at("unit_role_mastery_id")) {
            const auto role = number(row, "unit_role_id", 0, unit_role_count);
            if (role == 0) continue;
            const auto slot = number(row, "slot_id", 1, 4);
            auto& id = c.role_mastery_ids[role][slot - 1];
            if (id != 0) throw std::runtime_error("duplicate role mastery slot");
            id = number(row, "mastery_id", 1);
        }
        if (data.contains("recruitment")) {
            const auto& row = data.at("recruitment");
            auto& recruit = c.recruitment;
            recruit.ex_targets = parse_recruit_ex_targets(data);
            if (row.contains("healing_relics")) {
                const auto ids = priority(row.at("healing_relics"));
                recruit.healing_relics.insert(ids.begin(), ids.end());
            }
            if (row.contains("guard_mark_threshold")) recruit.guard_mark_threshold = number(row, "guard_mark_threshold");
        }
        if (t.contains("unit_role_data")) {
            const auto& roles = t.at("unit_role_data");
            if (!roles.is_array()) throw std::runtime_error("expected unit_role_data array");
            for (const auto& role : roles)
                index(c.unit_roles, number(role, "unit_id", 1), number(role, "unit_role_id", 1, unit_role_count));
        }
        for (const auto& r : t.at("labyrinth_relic"))
            index(c.relics, number(r, "relic_id", 1), Relic{number(r, "rarity"), number(r, "relic_mark_id"),
                number(r, "relic_mark_count"), r.at("name").get<std::string>()});
        for (const auto& r : t.at("labyrinth_summon_unit"))
            index(c.tickets, number(r, "item_id", 1), Ticket{number(r, "summon_type"), number(r, "summon_num"),
                number(r, "price"), r.at("name").get<std::string>()});
        for (const auto& row : t.at("labyrinth_enter_guild")) {
            const auto id = number(row, "guild_id", 1);
            c.guild_ids.insert(id);
            const auto name = row.find("guild_name");
            if (name != row.end() && name->is_string()) c.guild_names.emplace(id, name->get<std::string>());
        }
        c.opening_tickets = parse_opening_tickets(data);
        c.select_ticket_rosters = parse_select_ticket_rosters(data);
        if (!c.opening_tickets.empty() || !c.select_ticket_rosters.empty()) {
            std::set<Id> unit_ids;
            for (const auto& row : t.at("unit_data")) unit_ids.insert(number(row, "unit_id", 1));
            for (const auto& opening : c.opening_tickets) {
                const auto ticket = c.tickets.find(opening.ticket_id);
                if (ticket == c.tickets.end() || ticket->second.summon_type != 1 ||
                    ticket->second.summon_num != static_cast<int>(opening_pick_count))
                    throw std::runtime_error("invalid opening ticket: " + std::to_string(opening.ticket_id));
                if (!c.guild_ids.contains(opening.guild_id))
                    throw std::runtime_error("unknown opening guild: " + std::to_string(opening.guild_id));
                for (const auto unit : opening.default_units)
                    if (!unit_ids.contains(unit))
                        throw std::runtime_error("unknown default opening unit: " + std::to_string(unit));
            }
            for (const auto& roster : c.select_ticket_rosters) {
                const auto ticket = c.tickets.find(roster.ticket_id);
                if (ticket == c.tickets.end() || ticket->second.summon_type != 1 || ticket->second.summon_num != 1)
                    throw std::runtime_error("invalid ordinary select ticket: " + std::to_string(roster.ticket_id));
                for (const auto unit : roster.unit_ids)
                    if (!unit_ids.contains(unit))
                        throw std::runtime_error("unknown select ticket unit: " + std::to_string(unit));
            }
        }
        for (const auto& r : t.at("labyrinth_quest_data"))
            index(c.quests, number(r, "quest_id", 1), Quest{number(r, "quest_type"), number(r, "wave_group_id"),
                number(r, "limit_time")});
        for (const auto& target : c.recruitment.ex_targets) {
            for (const auto id : target.quest_ids) {
                const auto quest = c.quests.find(id);
                if (quest == c.quests.end() || quest->second.type != 2)
                    throw std::runtime_error("invalid recruitment EX quest: " + std::to_string(id));
            }
        }
        for (const auto& r : t.at("labyrinth_event"))
            index(c.events, number(r, "event_id", 1), std::array<Id, 3>{
                number(r, "choice_id_1"), number(r, "choice_id_2"), number(r, "choice_id_3")});
        for (const auto& r : t.at("labyrinth_event_choice"))
            index(c.event_options, number(r, "choice_id", 1), EventOption{number(r, "condition_type"),
                number(r, "condition_value"), number(r, "unit_id")});
        // Keep existing catalogs usable for policies that do not use scores.
        if (t.contains("labyrinth_setting")) {
            const auto& rows = t.at("labyrinth_setting");
            if (!rows.is_array()) throw std::runtime_error("expected labyrinth_setting array");
            for (const auto& row : rows) {
                const auto id = number(row, "setting_id", 1);
                if (id >= 201 && id <= 225) index(c.score_settings, id, number(row, "value"));
            }
        }
        if (t.contains("labyrinth_difficulty_data")) {
            const auto& rows = t.at("labyrinth_difficulty_data");
            if (!rows.is_array()) throw std::runtime_error("expected labyrinth_difficulty_data array");
            for (const auto& row : rows)
                index(c.score_rates, number(row, "difficulty", 1), number(row, "score_rate", 1, 10000));
        }
        return c;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Invalid game catalog: ") + e.what());
    }
}

void validate_ticket_pools(const Config& config, const Catalog& catalog) {
    for (const auto guild : config.ticket_clear_guilds)
        if (!catalog.guild_ids.contains(guild))
            throw std::runtime_error("Invalid labyrinth config: unknown ticket-clear guild: " + std::to_string(guild));
    for (const auto& entry : config.opening_ticket_units)
        if (std::ranges::find(catalog.opening_tickets, entry.first, &OpeningTicket::ticket_id) ==
            catalog.opening_tickets.end())
            throw std::runtime_error("Invalid labyrinth config: unknown opening ticket: " + std::to_string(entry.first));
    for (const auto& entry : config.ticket_priority_units)
        if (std::ranges::find(catalog.select_ticket_rosters, entry.first, &SelectTicketRoster::ticket_id) ==
            catalog.select_ticket_rosters.end())
            throw std::runtime_error("Invalid labyrinth config: unknown select ticket: " + std::to_string(entry.first));
}
} // namespace labyrinth
