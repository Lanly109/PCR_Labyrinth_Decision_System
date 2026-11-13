#include "labyrinth.hpp"

#include <algorithm>
#include <climits>
#include <fstream>
#include <unordered_set>

namespace labyrinth {
namespace {
int number(const Json& value, const char* field, int min = 0) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<uint64_t>() > INT_MAX))
        throw BadRequest(std::string(field) + " must be an integer");
    const auto n = value.get<int64_t>();
    if (n < min || n > INT_MAX) throw BadRequest(std::string(field) + " out of range");
    return static_cast<int>(n);
}

int field(const Json& value, const char* key, int min = 0) {
    return number(value.at(key), key, min);
}

const Json& array(const Json& value, const char* key) {
    const auto& items = value.at(key);
    if (!items.is_array()) throw BadRequest(std::string(key) + " must be an array");
    return items;
}

Ids ids(const Json& value, const char* key) {
    Ids result;
    for (const auto& item : array(value, key)) result.push_back(number(item, key, 1));
    return result;
}

void unique(const std::vector<int>& keys, const char* field) {
    if (std::unordered_set<int>(keys.begin(), keys.end()).size() != keys.size())
        throw BadRequest(std::string("duplicate ") + field);
}

void parties(const Json& value, const char* key) {
    const auto& all = array(value, key);
    if (all.size() > 3) throw BadRequest("too many parties");
    for (const auto& party : all) {
        if (!party.is_array() || party.size() > 5) throw BadRequest("invalid party");
        for (const auto& id : party) number(id, key, 1);
    }
}

void choices(const Json& body, const char* key, const char* id_key, bool reward, bool event) {
    Ids numbers;
    for (const auto& entry : array(body, key)) {
        numbers.push_back(field(entry, "choice_num", 1));
        field(entry, id_key, 1);
        if (reward) field(entry, "count", 1);
        if (event) {
            field(entry, "condition_type");
            field(entry, "condition_value");
            field(entry, "unit_id");
        }
    }
    unique(numbers, "choice_num");
}
} // namespace

Json read_json(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path_utf8(path));
    try {
        return Json::parse(file);
    } catch (const Json::exception& e) {
        throw std::runtime_error("Invalid JSON in " + path_utf8(path) + ": " + e.what());
    }
}

Request parse_request(Json body) {
    static const std::unordered_map<std::string, Kind> kinds = {
        {"select_ticket", Kind::select_ticket}, {"unit_choice", Kind::unit_choice},
        {"ticket_choice", Kind::ticket_choice}, {"relic_choice", Kind::relic_choice},
        {"event", Kind::event}, {"shop", Kind::shop}, {"team", Kind::team},
        {"opening", Kind::opening}, {"path", Kind::path}, {"score_check", Kind::score_check},
        {"ticket_clear", Kind::ticket_clear}
    };
    try {
        if (field(body, "protocol_version") != 1) throw BadRequest("unsupported protocol_version");
        auto kind = kinds.find(body.at("kind").get<std::string>());
        if (kind == kinds.end()) throw BadRequest("unknown kind");
        auto request_id = body.at("request_id").get<std::string>();
        if (request_id.empty()) throw BadRequest("request_id must not be empty");
        Request req{kind->second, std::move(request_id), field(body, "enter_id"),
                    field(body, "region"), field(body, "block_id"), field(body, "difficulty"),
                    field(body, "gold"), ids(body, "owned_unit_ids"), ids(body, "owned_relic_ids"), {}};
        switch (req.kind) {
        case Kind::select_ticket:
            field(body, "ticket_id", 1);
            field(body, "select_count", 1);
            ids(body, "candidate_unit_ids");
            break;
        case Kind::unit_choice:
        case Kind::ticket_choice:
        case Kind::relic_choice:
            for (auto name : {"ticket_id", "current_choice_count", "remain_choice_count"})
                if (body.contains(name)) field(body, name);
            if (req.kind == Kind::unit_choice) {
                if (body.contains("boss_quest_ids") && ids(body, "boss_quest_ids").size() > 10)
                    throw BadRequest("too many boss quests");
                if (body.contains("future_ex_regions")) {
                    const auto regions = ids(body, "future_ex_regions");
                    if (regions.size() > 5 || std::ranges::any_of(regions, [](int region) { return region > 5; }))
                        throw BadRequest("invalid future EX regions");
                }
            }
            choices(body, "choices", req.kind == Kind::unit_choice ? "unit_id" :
                    req.kind == Kind::ticket_choice ? "ticket_id" : "relic_id",
                    req.kind != Kind::unit_choice, false);
            break;
        case Kind::event:
            field(body, "event_id", 1);
            choices(body, "options", "choice_id", false, true);
            break;
        case Kind::shop: {
            field(body, "reset_count");
            field(body, "refresh_cost");
            field(body, "reset_limit");
            const auto& items = array(body, "items");
            if (items.size() > shop_max_items)
                throw BadRequest("shop page has more than " + std::to_string(shop_max_items) + " items");
            Ids lineups;
            for (const auto& item : items) {
                lineups.push_back(field(item, "lineup_id", 1));
                field(item, "reward_type");
                field(item, "reward_id", 1);
                field(item, "count", 1);
                field(item, "price");
                field(item, "sold");
                field(item, "availability", -1);
            }
            unique(lineups, "lineup_id");
            break;
        }
        case Kind::ticket_clear:
            field(body, "guild_id", 1);
            field(body, "ticket_count");
            field(body, "difficulty", 1);
            break;
        case Kind::score_check: {
            field(body, "enter_id", 1);
            field(body, "guild_id", 1);
            field(body, "quest_id", 1);
            if (req.region < 1 || req.region > 5 || req.difficulty <= 0 || req.block_id <= 0)
                throw BadRequest("invalid score check position");
            for (auto key : {"unit_count", "relic_count", "normal_clear_count", "ex_clear_count", "boss_clear_count"})
                if (field(body, key) > 10000) throw BadRequest("score count out of range");
            const auto& areas = array(body, "areas");
            if (areas.size() != static_cast<size_t>(req.region)) throw BadRequest("incomplete score areas");
            Ids regions;
            for (const auto& area : areas) {
                const auto region = field(area, "region", 1);
                if (region > req.region || field(area, "block_count") > 10000 ||
                    !area.at("cleared").is_boolean()) throw BadRequest("invalid score area");
                regions.push_back(region);
            }
            unique(regions, "score region");
            break;
        }
        case Kind::opening:
        case Kind::path: {
            field(body, "enter_id", 1);
            field(body, "difficulty", 1);
            const bool path = req.kind == Kind::path;
            if (path) {
                if (req.region < 1 || req.region > 5 || req.block_id <= 0)
                    throw BadRequest("invalid path position");
            } else {
                req.opening_attempt = field(body, "attempt", 1);
            }
            const auto& blocks = array(body, "map");
            if (blocks.empty() || blocks.size() > 1024) throw BadRequest("invalid route map size");
            std::array<Ids, 6> block_ids;
            for (const auto& block : blocks) {
                const auto region = field(block, "region", 1);
                const auto type = field(block, "block_type", -1);
                if (region > 5 || type > 8 || (path && region != req.region))
                    throw BadRequest("invalid route block region or type");
                OpeningBlock parsed{field(block, "block_id", 1), region, field(block, "column", 1), type,
                                    ids(block, "next_block_ids"), {}};
                if (block.contains("boss_unit_ids")) parsed.boss_units = ids(block, "boss_unit_ids");
                if (path) {
                    const auto& cleared = block.at("cleared");
                    if (!cleared.is_boolean()) throw BadRequest("cleared must be a boolean");
                    parsed.cleared = cleared.get<bool>();
                }
                block_ids[region].push_back(parsed.id);
                unique(parsed.next, "next_block_ids");
                req.opening_blocks.push_back(std::move(parsed));
            }
            for (const auto& region : block_ids) unique(region, "opening block_id");
            break;
        }
        case Kind::team:
            field(body, "quest_id", 1);
            field(body, "party_count", 1);
            if (body.contains("existing_parties")) parties(body, "existing_parties");
            if (body.contains("unit_npc_flags")) {
                Ids marked;
                for (const auto& entry : array(body, "unit_npc_flags")) {
                    const auto id = field(entry, "unit_id", 1);
                    const auto& flag = entry.at("is_npc");
                    if (!flag.is_boolean()) throw BadRequest("is_npc must be boolean");
                    marked.push_back(id);
                    if (flag.get<bool>()) req.npc_units.insert(id);
                }
                unique(marked, "unit NPC flag");
            }
            for (const auto& attempt : array(body, "past_attempts")) {
                parties(attempt, "parties");
                auto outcome = attempt.at("outcome").get<std::string>();
                if (outcome != "unknown" && outcome != "defeat" && outcome != "victory")
                    throw BadRequest("invalid outcome");
            }
            break;
        }
        req.body = std::move(body);
        return req;
    } catch (const Json::exception& e) {
        throw BadRequest(e.what());
    }
}
} // namespace labyrinth
