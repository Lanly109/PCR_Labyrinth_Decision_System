#include "console.h"
#include "server_runtime.hpp"
#include "settings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace labyrinth::gui {
namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr std::size_t queue_limit = 256;
constexpr std::size_t queue_bytes_limit = 6 * 1024 * 1024;

std::string str(const slint::SharedString& text) { return std::string(std::string_view(text)); }
slint::SharedString ss(std::string_view text) { return slint::SharedString(text); }

template<class T>
auto model(std::vector<T> rows) {
    return std::make_shared<slint::VectorModel<T>>(std::move(rows));
}

auto strings(const std::vector<std::string>& input) {
    std::vector<slint::SharedString> values;
    values.reserve(input.size());
    for (const auto& item : input) values.emplace_back(item);
    return model(std::move(values));
}

std::string text(const Json& value, std::string_view name) {
    if (!value.is_object()) return {};
    const auto iter = value.find(name);
    if (iter == value.end() || iter->is_null()) return {};
    return iter->is_string() ? iter->get<std::string>() : iter->dump();
}

std::string account_label(const Json& record) {
    auto name = text(record, "account_name");
    if (name.empty()) name = text(record, "uid");
    return name.empty() ? "—" : name;
}

int number(const Json& value, std::string_view name, int fallback = 0) {
    if (!value.is_object()) return fallback;
    const auto iter = value.find(name);
    return iter != value.end() && iter->is_number_integer() ? iter->get<int>() : fallback;
}

const Json& item(const Json& value, std::string_view name) {
    static const Json empty;
    if (!value.is_object()) return empty;
    const auto iter = value.find(name);
    return iter == value.end() ? empty : *iter;
}

std::string join(const std::vector<std::string>& parts, std::string_view delimiter) {
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) result += delimiter;
        result += part;
    }
    return result;
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kinds{{
    {"", "全部类型"}, {"team", "编队"}, {"path", "寻路"}, {"opening", "开局"},
    {"score_check", "刷分"}, {"ticket_clear", "清票"}, {"event", "事件"}, {"shop", "商店"},
    {"unit_choice", "角色"}, {"select_ticket", "自选"}, {"ticket_choice", "印记"},
    {"relic_choice", "遗物"}
}};

std::string kind_label(std::string_view value) {
    if (value.empty()) return "—";
    for (const auto& [key, label] : kinds) if (key == value) return std::string(label);
    return std::string(value);
}

std::string milliseconds(const Json& log, int precision) {
    const auto& value = item(log, "elapsed_ms");
    if (!value.is_number()) return "—";
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value.get<double>() << " ms";
    return stream.str();
}

std::string local_time(const std::string& timestamp, bool show_date) {
    if (timestamp.size() < 20 || timestamp.back() != 'Z') return timestamp;
    std::tm utc{};
    std::istringstream parser(timestamp.substr(0, 19));
    parser >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (parser.fail()) return timestamp;
    std::tm local{};
#ifdef _WIN32
    const auto seconds = _mkgmtime64(&utc);
    if (_localtime64_s(&local, &seconds) != 0) return timestamp;
#else
    const auto seconds = timegm(&utc);
    if (!localtime_r(&seconds, &local)) return timestamp;
#endif
    std::ostringstream formatted;
    formatted << std::put_time(&local, show_date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S");
    if (timestamp[19] == '.') formatted << timestamp.substr(19, timestamp.size() - 20);
    return formatted.str();
}

std::string region_label(const Json& request) {
    const auto region = number(request, "region");
    return region > 0 ? "第 " + std::to_string(region) + " 层" : "";
}

std::string event_label(const Json& request, const Json& catalog) {
    return catalog_name(catalog, "labyrinth_event", "event_id", "event_text", number(request, "event_id"));
}

std::string unit_name(const Json& catalog, int id) {
    return catalog_name(catalog, "unit_data", "unit_id", "unit_name", id);
}

std::vector<std::string> unit_list(const Json& units, const Json& catalog) {
    std::vector<std::string> names;
    if (units.is_array()) {
        for (const auto& unit : units) if (unit.is_number_integer())
            names.push_back(unit_name(catalog, unit.get<int>()));
    }
    return names;
}

std::string block_type_name(int type) {
    static constexpr std::array<std::string_view, 9> labels{
        "未知格子", "起点", "普通", "极难", "印记", "事件", "遗物", "商店", "首领"
    };
    return std::string(labels[type > 0 && type < static_cast<int>(labels.size()) ? type : 0]);
}

std::string block_name(const Json& request, int id) {
    const auto& blocks = item(request, "map");
    const auto found = std::ranges::find_if(blocks, [id](const auto& block) {
        return id > 0 && number(block, "block_id") == id;
    });
    return block_type_name(found == blocks.end() ? 0 : number(*found, "block_type"));
}

std::string relic_label(const Json& catalog, int id) {
    const auto& relics = item(item(catalog, "tables"), "labyrinth_relic");
    const auto row = std::ranges::find_if(relics, [id](const auto& relic) {
        return number(relic, "relic_id") == id;
    });
    if (row == relics.end()) return "未知遗物";
    const auto mark = number(*row, "relic_mark_id");
    const auto category = mark == 0 ? "无系列" :
        catalog_name(catalog, "labyrinth_relic_mark_series", "relic_mark_id", "relic_mark_title", mark);
    return text(*row, "rarity") + "星" + category;
}

std::string probability_label(const Json& value) {
    if (!value.is_number()) return "未评估";
    const auto probability = value.get<double>();
    if (!(probability >= 0 && probability <= 1)) return "未评估";
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(1) << probability * 100 << '%';
    return formatted.str();
}

std::string choice_name(std::string_view kind, const Json& option, const Json& catalog) {
    if (kind == "event") {
        const auto id = number(option, "choice_id");
        auto label = catalog_name(catalog, "labyrinth_event_choice", "choice_id", "unit_comment", id);
        if (label.starts_with("#")) {
            label = catalog_name(catalog, "labyrinth_event_choice", "choice_id", "description", id);
        }
        return label;
    }
    if (kind == "unit_choice") return unit_name(catalog, number(option, "unit_id"));
    if (kind == "ticket_choice")
        return catalog_name(catalog, "labyrinth_summon_unit", "item_id", "name", number(option, "ticket_id"));
    return relic_label(catalog, number(option, "relic_id"));
}

std::string brief_choice(std::string_view kind, const Json& option, const Json& catalog) {
    if (kind == "ticket_choice") {
        static constexpr std::array<std::string_view, 6> elements{"", "火", "水", "风", "光", "暗"};
        const auto element = (number(option, "ticket_id") / 100000) % 10;
        if (element > 0 && element < static_cast<int>(elements.size())) return std::string(elements[element]);
    }
    return choice_name(kind, option, catalog);
}

std::string team_quest_label(const Json& request, const Json& catalog) {
    const auto quest_id = number(request, "quest_id");
    const auto& tables = item(catalog, "tables");
    const auto& quests = item(tables, "labyrinth_quest_data");
    const auto is_quest = [quest_id](const auto& quest) { return number(quest, "quest_id") == quest_id; };
    const auto quest = std::ranges::find_if(quests, is_quest);
    const auto type = quest == quests.end() ? 0 : number(*quest, "quest_type");
    if (type == 1) return "普通";
    if (type == 2) {
        const auto& encounters = item(item(catalog, "recruitment"), "ex_quests");
        const auto encounter = std::ranges::find_if(encounters, is_quest);
        if (encounter != encounters.end()) {
            auto name = text(*encounter, "name");
            if (!name.empty()) return name;
        }
        return "极难";
    }
    if (type == 3) {
        auto name = catalog_name(catalog, "labyrinth_boss_quests", "quest_id", "unit_name", quest_id);
        return name.starts_with("#") ? "首领" : name;
    }
    return "未知关卡";
}

std::string brief_request(const Json& record, const Json& catalog) {
    const auto kind = text(record, "kind");
    const auto& request = item(record, "request");
    const auto region = region_label(request);
    const auto at_region = [&region](std::string detail) {
        return region.empty() ? detail : region + " · " + detail;
    };
    if (kind == "team")
        return at_region(team_quest_label(request, catalog));
    if (kind == "path") return at_region("当前 " + block_name(request, number(request, "block_id")));
    if (kind == "opening") return at_region("第 " + text(request, "attempt") + " 次开局");
    if (kind == "ticket_clear") {
        auto guild = catalog_name(catalog, "labyrinth_enter_guild", "guild_id", "guild_name", number(request, "guild_id"));
        const auto tickets = text(request, "ticket_count");
        return at_region(tickets.empty() ? guild : guild + " · " + tickets + " 张票");
    }
    if (kind == "score_check") {
        auto detail = catalog_name(catalog, "labyrinth_enter_guild", "guild_id", "guild_name",
                                   number(request, "guild_id"));
        std::vector<std::string> counts;
        if (const auto& units = item(request, "unit_count"); units.is_number_integer())
            counts.push_back(units.dump() + "角色");
        if (const auto& relics = item(request, "relic_count"); relics.is_number_integer())
            counts.push_back(relics.dump() + "遗物");
        if (!counts.empty()) detail += " · " + join(counts, "、");
        return at_region(std::move(detail));
    }
    if (kind == "event") return at_region(event_label(request, catalog));
    if (kind == "shop") return at_region("已刷新 " + text(request, "reset_count") + " 次");
    if (kind == "select_ticket")
        return at_region(catalog_name(catalog, "labyrinth_summon_unit", "item_id", "name",
                                      number(request, "ticket_id")));
    if (kind == "unit_choice" || kind == "ticket_choice" || kind == "relic_choice") {
        std::vector<std::string> options;
        for (const auto& choice : item(request, "choices"))
            options.push_back(brief_choice(kind, choice, catalog));
        if (!options.empty()) return at_region(join(options, "、"));
    }
    return region.empty() ? "—" : region;
}

enum class LogOutcome { decision = 1, noop, error };

LogOutcome log_outcome(const Json& record) {
    if (number(record, "http_status") >= 400) return LogOutcome::error;
    return text(item(record, "response"), "action") == "noop" ? LogOutcome::noop : LogOutcome::decision;
}

std::string grouped_score(int64_t score) {
    auto label = std::to_string(score);
    const std::size_t first_digit = score < 0 ? 1 : 0;
    for (auto end = label.size(); end > first_digit + 3; end -= 3)
        label.insert(end - 3, 1, ',');
    return label;
}

std::string score_comparison(const Json& record) {
    if (text(record, "kind") != "score_check" || number(record, "http_status") >= 400) return {};
    const auto& response = item(record, "response");
    const auto action = text(response, "action");
    if (action != "fight" && action != "retry_run") return {};
    const auto& score = item(response, "score");
    const auto& minimum = item(response, "minimum_score");
    if (!score.is_number_integer() || !minimum.is_number_integer()) return {};
    const auto current = score.get<int64_t>();
    const auto target = minimum.get<int64_t>();
    return grouped_score(current) + (current < target ? " < " : " >= ") + grouped_score(target);
}

std::string brief_response(const Json& record, const Json& catalog) {
    const auto& response = item(record, "response");
    const auto action = text(response, "action");
    const auto kind = text(record, "kind");
    if (const auto status = number(record, "http_status"); status >= 400)
        return "HTTP " + std::to_string(status) + " · " + text(response, "error");
    if (action == "noop") {
        if (text(record, "reason") == "master_disabled") return "无动作 · 总控已关闭";
        if (kind == "path" && item(item(record, "diagnostics"), "path_no_match") == true)
            return "无符合路径";
        return "无动作";
    }
    if (action == "move")
        return "前往 " + block_name(item(record, "request"), number(response, "next_block_id"));
    if (action == "team") {
        std::vector<std::string> parties;
        for (const auto& party : item(response, "parties")) {
            auto names = unit_list(party, catalog);
            if (!names.empty()) parties.push_back(join(names, "、"));
        }
        return parties.empty() ? "—" : join(parties, " / ");
    }
    if (action == "manual") return "转人工";
    if (action == "retry_run") return "撤退重开";
    if (action == "fight") return "继续挑战";
    if (action == "continue_run")
        return "继续开局 · " + catalog_name(catalog, "labyrinth_enter_guild", "guild_id", "guild_name",
                                           number(response, "guild_id"));
    if (action == "opening_accept") return "接受开局";
    if (action == "opening_retry") return "重新开局";
    if (action == "opening_stop") return "停止刷开局";
    if (action == "shop_plan") {
        const auto count = item(response, "buy_lineup_ids").size();
        const std::string after = text(response, "after") == "refresh" ? "刷新商店" : "关闭商店";
        return count ? "购买 " + std::to_string(count) + " 项 · " + after : after;
    }
    if (action == "select") {
        if (kind == "select_ticket")
            return join(unit_list(item(response, "unit_ids"), catalog), "、");
        const auto& request = item(record, "request");
        const auto& choices = item(request, kind == "event" ? "options" : "choices");
        const auto target = number(response, "choice_num");
        for (const auto& choice : choices) {
            if (number(choice, "choice_num") == target) return "选择「" + choice_name(kind, choice, catalog) + "」";
        }
        return "选择 " + std::to_string(target);
    }
    return action.empty() ? "—" : action;
}

void add_detail(std::vector<std::string>& lines, const Json& object, std::string_view key, std::string_view label) {
    const auto value = text(object, key);
    if (!value.empty()) lines.push_back(std::string(label) + "  " + value);
}

std::string full_request(const Json& record, const Json& catalog) {
    const auto& request = item(record, "request");
    const auto kind = text(record, "kind");
    std::vector<std::string> lines{brief_request(record, catalog)};
    add_detail(lines, request, "enter_id", "局 ID");
    if (kind != "path") add_detail(lines, request, "block_id", "格子 ID");
    add_detail(lines, request, "difficulty", "难度");
    add_detail(lines, request, "gold", "金币");
    if (kind == "team") {
        add_detail(lines, request, "quest_id", "关卡 ID");
        const auto& previous = item(request, "past_attempts");
        const auto defeats = std::ranges::count_if(previous, [](const auto& entry) {
            return text(entry, "outcome") == "defeat";
        });
        lines.push_back("已记录败场  " + std::to_string(defeats));
        lines.push_back("可用角色  " + std::to_string(item(request, "owned_unit_ids").size()));
    }
    if (kind == "select_ticket") {
        add_detail(lines, request, "select_count", "需要选择");
        auto names = unit_list(item(request, "candidate_unit_ids"), catalog);
        if (!names.empty()) lines.push_back("候选  " + join(names, "、"));
    }
    const auto& choices = item(request, kind == "event" ? "options" : "choices");
    for (const auto& choice : choices) {
        auto description = text(choice, "choice_num") + " · " + choice_name(kind, choice, catalog);
        if (number(choice, "condition_type") == 1 && number(choice, "condition_value") > 0)
            description += "（需 " + text(choice, "condition_value") + " 金币）";
        lines.push_back(std::move(description));
    }
    return join(lines, "\n");
}

void add_opening_failure(std::vector<std::string>& lines, const Json& failure, const Json& catalog) {
    const auto kind = text(failure, "kind");
    if (kind == "map_missing") lines.push_back("缺少该区域的地图");
    if (kind == "no_route") lines.push_back("从起点无法走到该区域末列");
    if (kind == "boss_mismatch") {
        lines.push_back("可达首领不在配置名单内");
        std::vector<std::string> allowed;
        for (const auto& id : item(failure, "allowed_boss_unit_ids")) {
            if (!id.is_number_integer()) continue;
            const auto unit = id.get<int>();
            auto label = catalog_name(catalog, "labyrinth_boss_units", "unit_id", "unit_name", unit);
            if (label.starts_with("#")) label = unit_name(catalog, unit);
            allowed.push_back(std::move(label));
        }
        if (!allowed.empty()) lines.push_back("允许首领  " + join(allowed, "、"));
    }
    if (kind == "cell_shortfall") {
        lines.push_back("最接近达标的可达路线仍缺少");
        for (const auto& cell : item(failure, "cells")) {
            lines.push_back(block_type_name(number(cell, "block_type")) + "  实际 " +
                text(cell, "reached") + " / 要求 " + text(cell, "required"));
        }
    }
}

std::string full_response(const Json& record, const Json& catalog) {
    const auto& response = item(record, "response");
    const auto& diagnostics = item(record, "diagnostics");
    const auto& parties = item(response, "parties");
    const auto& recommendations = item(diagnostics, "team_recommendations");
    const bool show_candidates = parties.is_array() && parties.size() == 1 &&
        recommendations.is_array() && !recommendations.empty();
    const auto& displayed = show_candidates ? recommendations : parties;
    std::vector<std::string> lines{parties.is_array() ?
        "推荐 " + std::to_string(displayed.size()) + " 支队伍" : brief_response(record, catalog)};
    const auto& predictions = item(diagnostics, "party_win_probabilities");
    std::size_t index = 0;
    for (const auto& team : displayed) {
        const auto& party = show_candidates ? item(team, "units") : team;
        auto label = "第 " + std::to_string(index + 1) + " 队";
        if (show_candidates)
            label += " · 预测胜率 " + probability_label(item(team, "win_probability"));
        else if (predictions.is_array() && index < predictions.size())
            label += " · 预测胜率 " + probability_label(predictions[index]);
        auto units = party.get<Ids>();
        std::ranges::sort(units);
        for (const auto& candidate : item(item(diagnostics, "simulator"), "candidates")) {
            if (item(candidate, "units") != units) continue;
            if (candidate.contains("remaining_seconds")) {
                label += " · 剩余 " + text(candidate, "remaining_seconds") + " 秒";
                if (!candidate.value("victory", false)) label += " · 未胜利";
            }
            break;
        }
        lines.push_back(label + "  " + join(unit_list(party, catalog), "、"));
        ++index;
    }
    const auto& simulator = item(diagnostics, "simulator");
    if (simulator.is_object()) {
        if (simulator.value("fallback", false)) lines.push_back("模拟未获得胜利结果，使用 Critic 推荐");
        add_detail(lines, simulator, "error", "模拟器信息");
    }
    if (text(record, "reason") == "battle_retry_no_team") lines.push_back("没有可重试的新队伍");
    add_detail(lines, response, "defeat_count", "败场");
    add_detail(lines, response, "max_retries", "最多重试");
    add_detail(lines, response, "score", "判定分数");
    add_detail(lines, response, "minimum_score", "目标分数");
    add_detail(lines, response, "rejected_region", "未达条件区域");
    add_opening_failure(lines, item(diagnostics, "opening_failure"), catalog);
    const auto& recruitment = item(diagnostics, "unit_recruitment");
    if (recruitment.is_object()) {
        const auto points = [](const Json& score) {
            if (!score.is_number()) return std::string("—");
            std::ostringstream out;
            out << std::fixed << std::setprecision(1) << score.get<double>() * 100;
            return out.str();
        };
        if (number(recruitment, "projected_samples") > 0)
            lines.push_back("开局补位估算 · " + text(recruitment, "projected_samples") + " 组假设队伍");
        if (item(recruitment, "boss_context") == false)
            lines.emplace_back("客户端未提供本局首领关卡，跳过首领模型评分");
        lines.emplace_back("候选加权增益");
        for (const auto& candidate : item(recruitment, "candidates")) {
            const auto& gain = item(candidate, "gain");
            const auto sign = gain.is_number() && gain.get<double>() >= 0 ? "+" : "";
            lines.push_back(unit_name(catalog, number(candidate, "unit_id")) + "  " + sign +
                            points(gain) + " 分");
        }
        for (const auto& goal : item(recruitment, "targets")) {
            const auto key = text(goal, "target");
            auto label = text(goal, "label");
            if (label.empty()) label = key;
            std::vector<std::string> scores;
            for (const auto& score : item(goal, "scores")) scores.push_back(points(score));
            lines.push_back(label + " ×" + text(goal, "weight") +
                            (item(goal, "model") == false ? " · 仅储备  " : "  ") + join(scores, " / "));
        }
        lines.emplace_back("各目标依次显示基线和三个候选分数");
    }
    if (text(response, "action") == "shop_plan") {
        const auto& request = item(record, "request");
        const auto& items = item(request, "items");
        for (const auto& lineup : item(response, "buy_lineup_ids")) {
            if (!lineup.is_number_integer()) continue;
            const auto id = lineup.get<int>();
            const auto found = std::ranges::find_if(items, [id](const auto& product) {
                return number(product, "lineup_id") == id;
            });
            if (found == items.end()) {
                lines.push_back("商品 #" + std::to_string(id));
                continue;
            }
            const auto type = number(*found, "reward_type");
            const auto reward = number(*found, "reward_id");
            auto name = type == 26 ? catalog_name(catalog, "labyrinth_relic", "relic_id", "name", reward) :
                catalog_name(catalog, "labyrinth_summon_unit", "item_id", "name", reward);
            lines.push_back(name + " ×" + text(*found, "count") + " · " + text(*found, "price") + " 金币");
        }
    }
    return join(lines, "\n");
}

Json verified_catalog(const fs::path& root, const Json& data) {
    const auto config = parse_config(data);
    auto catalog = read_json(root / config.catalog);
    validate_ticket_pools(config, parse_catalog(catalog));
    return catalog;
}

fs::path sibling_temp(const fs::path& path, std::string_view suffix) {
    auto temporary = path;
    temporary += std::string(suffix) + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return temporary;
}

void save_json(const fs::path& path, const Json& value) {
    const auto temporary = sibling_temp(path, ".new.");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << value.dump(4) << '\n';
        output.close();
        fs::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

bool config_file_unchanged(const fs::path& path, const std::optional<Json>& saved) {
    std::error_code error;
    const auto exists = fs::exists(path, error);
    if (error) throw fs::filesystem_error("could not check config", path, error);
    return saved ? exists && read_json(path) == *saved : !exists;
}

struct Entry {
    Json value;
    std::size_t bytes;
    std::uint64_t sequence = 0;
    std::optional<std::array<std::string, 6>> rendered = std::nullopt;
    std::streamoff file_end = 0;
};

// Take a snapshot of complete lines. In bounded mode, scan backward in
// chunks so a long-lived log does not have to be read from the beginning.
std::pair<std::streamoff, std::streamoff> history_window(std::istream& input, std::size_t limit) {
    input.seekg(0, std::ios::end);
    std::streamoff cursor = input.tellg();
    std::streamoff end = 0;
    std::array<char, 64 * 1024> chunk{};
    while (cursor > 0) {
        const auto start = std::max<std::streamoff>(0, cursor - static_cast<std::streamoff>(chunk.size()));
        const auto count = cursor - start;
        input.seekg(start);
        if (!input.read(chunk.data(), count)) return {};
        for (auto i = count; i > 0; --i) {
            if (chunk[static_cast<std::size_t>(i - 1)] != '\n') continue;
            if (!end) {
                end = start + i;
                if (!limit) return {0, end};
            } else if (--limit == 0) {
                return {start + i, end};
            }
        }
        cursor = start;
    }
    return {0, end};
}

class LogBuffer {
public:
    void push(const Json& value, std::size_t size, std::streamoff file_end) {
        std::lock_guard lock(mutex_);
        if (size > queue_bytes_limit) {
            ++dropped_;
            return;
        }
        while (!entries_.empty() && (entries_.size() >= queue_limit || bytes_ + size > queue_bytes_limit)) {
            bytes_ -= entries_.front().bytes;
            entries_.pop_front();
            ++dropped_;
        }
        entries_.push_back({value, size, 0, std::nullopt, file_end});
        bytes_ += size;
    }

    auto drain() {
        std::lock_guard lock(mutex_);
        bytes_ = 0;
        return std::pair{std::exchange(entries_, {}), std::exchange(dropped_, 0)};
    }

private:
    std::mutex mutex_;
    std::deque<Entry> entries_;
    std::size_t bytes_ = 0;
    std::size_t dropped_ = 0;
};

class ServerWorker {
public:
    ServerWorker() = default;
    ServerWorker(const ServerWorker&) = delete;
    ServerWorker& operator=(const ServerWorker&) = delete;
    ~ServerWorker() { shutdown(); }

    void shutdown() {
        stop();
        if (thread_.joinable()) thread_.join();
        control_.reset();
    }

    void start(const fs::path& root, const fs::path& config, const fs::path& log,
               bool enabled, LogBuffer& logs) {
        if (thread_.joinable()) throw std::logic_error("server already running");
        std::error_code ec;
        if (!log.parent_path().empty()) fs::create_directories(log.parent_path(), ec);
        if (ec) throw fs::filesystem_error("could not create log directory", log.parent_path(), ec);
        finished_.store(false);
        failure_.clear();
        auto control = std::make_shared<ServerControl>();
        control->enabled.store(enabled);
        thread_ = std::thread([this, control, root, config, log, &logs] {
            try {
                ServerRunOptions options;
                options.root = root;
                options.config_path = config;
                options.decision_log_path = log;
                options.decision_log = [&logs](const Json& value, std::size_t size, std::streamoff file_end) {
                    logs.push(value, size, file_end);
                };
                if (run_server({}, options, control.get()) != 0) failure_ = "服务监听异常退出";
            } catch (const std::exception& error) {
                failure_ = error.what();
            }
            finished_.store(true);
        });
        control_ = std::move(control);
    }

    std::future<Json> reload(fs::path root, fs::path path, Config config, Json draft,
                             std::optional<Json> saved) {
        return std::async(std::launch::async,
            [control = control_, root = std::move(root), path = std::move(path),
             config = std::move(config), draft = std::move(draft), saved = std::move(saved)]() mutable {
                auto catalog = read_json(root / config.catalog);
                const auto candidate = sibling_temp(path, ".candidate.");
                save_json(candidate, draft);
                try {
                    control->reload(root, std::move(config), parse_catalog(catalog), [&] {
                        if (!config_file_unchanged(path, saved))
                            throw std::runtime_error("配置文件已被外部修改，请重新打开控制台后编辑");
                        fs::rename(candidate, path);
                    });
                } catch (...) {
                    std::error_code ignored;
                    fs::remove(candidate, ignored);
                    throw;
                }
                return catalog;
            });
    }

    bool online() const { return control_ && control_->ready(); }
    bool running() const { return thread_.joinable(); }
    bool finished() const { return thread_.joinable() && finished_.load(); }
    std::string backend() const { return control_ ? control_->backend() : ""; }
    void stop() { if (control_) control_->stop(); }
    void enable(bool enabled) { if (control_) control_->enabled.store(enabled); }
    std::string join_finished() {
        thread_.join();
        control_.reset();
        return std::exchange(failure_, {});
    }

private:
    std::shared_ptr<ServerControl> control_;
    std::thread thread_;
    std::atomic_bool finished_{false};
    std::string failure_;
};

struct Paths {
    fs::path root;
    fs::path config;
    fs::path log;
};

Paths paths(const std::vector<std::string>& args) {
    Paths result{executable_dir(args.empty() ? nullptr : args.front().c_str()), {}, {}};
#ifdef __APPLE__
    const auto contents = result.root.parent_path();
    const auto bundle = contents.parent_path();
    if (result.root.filename() == "MacOS" && contents.filename() == "Contents" && bundle.extension() == ".app")
        result.root = bundle.parent_path();
#endif
    result.config = result.root / "config" / "labyrinth.json";
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& flag = args[i];
        if ((flag != "--config" && flag != "--decision-log") || i + 1 >= args.size())
            throw std::runtime_error("用法：黎明界决策系统 [--config PATH] [--decision-log PATH]");
        const auto value = path_from_utf8(args[++i]);
        if (flag == "--config") result.config = fs::absolute(value);
        else result.log = fs::absolute(value);
    }
    if (result.log.empty()) result.log = result.config.parent_path() / "logs" / "decisions.jsonl";
    return result;
}

class Console {
public:
    explicit Console(Paths paths) : paths_(std::move(paths)), ui_(ConsoleWindow::create()) {
        ui_->set_app_version("v" LABYRINTH_VERSION);
#ifdef __APPLE__
        // Slint's default font selection can leave CJK glyphs blank with Skia on macOS.
        ui_->set_ui_font_family("Heiti SC");
#endif
        ui_->set_section_names(strings(section_names()));
        ui_->set_account_options(strings({"全部账号"}));
        std::vector<slint::SharedString> kind_options;
        kind_options.reserve(kinds.size());
        for (const auto& kind : kinds) kind_options.emplace_back(kind.second);
        ui_->set_kind_options(model(std::move(kind_options)));
        connect_callbacks();
        try {
            saved_ = read_json(paths_.config);
            draft_ = *saved_;
            history_limit_ = static_cast<std::size_t>(parse_config(draft_).log_history_limit);
            catalog_ = verified_catalog(paths_.root, draft_);
            log_catalog_ = catalog_;
            update_endpoint(draft_);
            start(paths_.config);
            ui_->set_initial_startup(true);
        } catch (const std::exception& error) {
            show_notice("无法启动 · " + std::string(error.what()), true);
            if (!draft_.is_object()) draft_ = {{"schema_version", 1}};
        }
        read_history();
        update_settings();
        refresh_logs();
        timer_.start(slint::TimerMode::Repeated, 120ms, [this] { tick(); });
    }

    ~Console() {
        timer_.stop();
        if (hot_reload_.valid()) hot_reload_.wait();
        worker_.shutdown();
        remove_candidate();
    }

    void run() { ui_->run(); }

private:
    void connect_callbacks() {
        ui_->on_master_changed([this](bool enabled) {
            if (!starting_candidate_) worker_.enable(enabled);
        });
        ui_->on_section_changed([this](int value) {
            if (section_ == value) return;
            if (section_ == raw_section && raw_edited()) {
                if (!apply_raw(str(ui_->get_raw_draft()))) return;
            }
            section_ = value;
            subpage_ = 0;
            if (section_ == raw_section) set_raw();
            update_settings();
        });
        ui_->on_subpage_changed([this](int value) {
            if (value < 0 || static_cast<std::size_t>(value) >= settings_.subpages.size() || value == subpage_)
                return;
            subpage_ = value;
            update_settings();
        });
        ui_->on_ticket_filtered([this](const slint::SharedString&) { update_settings(); });
        ui_->on_ticket_changed([this](int index) {
            if (index < 0 || static_cast<std::size_t>(index) >= settings_.ticket_picker.labels.size()) return;
            ticket_index_ = index;
            update_settings();
        });
        ui_->on_setting_edited([this](const slint::SharedString& key, const slint::SharedString& value) {
            edit(str(key), str(value));
        });
        ui_->on_order_edited([this](const slint::SharedString& key, int delta) {
            auto current = settings_.edits.find(str(key));
            if (current == settings_.edits.end() || !current->second.move) return;
            current->second.move(draft_, delta);
            update_settings();
            update_dirty();
        });
        ui_->on_browse_path([this](const slint::SharedString& key, const slint::SharedString& value) {
            begin_browse(str(key), str(value));
        });
        ui_->on_browse_navigate([this](const slint::SharedString& value) {
            browse_navigate(str(value));
        });
        ui_->on_browse_choose([this](const slint::SharedString& value) {
            choose_path(str(value));
        });
        ui_->on_apply_raw([this](const slint::SharedString& value) { apply_raw(str(value)); });
        ui_->on_save_config([this] { save(); });
        ui_->on_revert_config([this] {
            try {
                auto clean = saved_.value_or(Json{{"schema_version", 1}});
                auto next_catalog = verified_catalog(paths_.root, clean);
                draft_ = std::move(clean);
                catalog_ = std::move(next_catalog);
                invalid_.clear();
                set_raw();
                show_config("已撤销更改", false);
                update_settings();
                update_dirty();
            } catch (const std::exception& error) {
                show_config(error.what(), true);
            }
        });
        ui_->on_log_filtered([this] { refresh_logs(); });
        ui_->on_select_log([this](int row) {
            if (updating_logs_ || row < 0 || static_cast<std::size_t>(row) >= visible_.size()) return;
            selected_ = visible_[static_cast<std::size_t>(row)]->sequence;
            show_entry(visible_[static_cast<std::size_t>(row)].get());
        });
        ui_->on_sort_log([this](int column, bool ascending) {
            sort_column_ = column;
            ascending_ = ascending;
            refresh_logs();
        });
    }

    void begin_browse(const std::string& key, const std::string& value) {
        const auto field = std::ranges::find(settings_.fields, key, &Field::key);
        if (field == settings_.fields.end() || field->path_kind == PathKind::none) return;
        browse_key_ = key;
        browse_location_ = paths_.root;
        ui_->set_browse_directory(field->path_kind == PathKind::folder);
        ui_->set_browse_location("");
        ui_->set_browse_items(model(std::vector<BrowseItem>{}));
        try {
            const auto path = paths_.root / path_from_utf8(value);
            std::error_code error;
            auto directory = fs::is_directory(path, error) ? path : path.parent_path();
            while (!directory.empty() && !fs::is_directory(directory, error)) {
                if (directory == directory.root_path()) break;
                directory = directory.parent_path();
            }
            if (directory.empty() || !fs::is_directory(directory, error)) directory = paths_.root;
            show_directory(directory);
        } catch (const std::exception&) {
            ui_->set_browse_error("无法读取初始目录");
        }
    }

    void browse_navigate(const std::string& value) {
        try {
            show_directory(browse_location_ / path_from_utf8(value));
        } catch (const std::exception&) {
            ui_->set_browse_error("无法打开目录，请检查路径和权限");
        }
    }

    void show_directory(const fs::path& path) {
        const auto directory = fs::absolute(path).lexically_normal();
        const auto location = path_utf8(directory);
        (void)Json(location).dump();
        std::vector<BrowseItem> entries;
        for (const auto& entry : fs::directory_iterator(directory)) {
            std::error_code error;
            const bool is_directory = entry.is_directory(error);
            if (is_directory || (!ui_->get_browse_directory() && entry.is_regular_file(error))) {
                const auto name = path_utf8(entry.path().filename());
                // POSIX permits filenames that neither Slint nor the JSON config can represent.
                try {
                    (void)Json(name).dump();
                } catch (const Json::type_error&) {
                    continue;
                }
                BrowseItem row;
                row.name = ss(name);
                row.directory = is_directory;
                entries.push_back(std::move(row));
            }
        }
        std::ranges::sort(entries, [](const BrowseItem& left, const BrowseItem& right) {
            if (left.directory != right.directory) return left.directory;
            return std::string_view(left.name) < std::string_view(right.name);
        });
        browse_location_ = directory;
        ui_->set_browse_location(ss(location));
        ui_->set_browse_items(model(std::move(entries)));
        ui_->set_browse_error("");
    }

    void choose_path(const std::string& value) {
        if (browse_key_.empty()) return;
        try {
            const auto path = (browse_location_ / path_from_utf8(value)).lexically_normal();
            const bool valid = ui_->get_browse_directory() ? fs::is_directory(path) : fs::is_regular_file(path);
            if (!valid) {
                ui_->set_browse_error("所选路径已不存在或类型不符");
                return;
            }
            const auto relative = path.lexically_relative(paths_.root);
            const bool inside = !relative.empty() && *relative.begin() != "..";
            edit(browse_key_, path_utf8(inside ? relative : path));
            if (const auto invalid = invalid_.find(browse_key_); invalid != invalid_.end()) {
                ui_->set_browse_error(ss(invalid->second.second));
                return;
            }
            ui_->set_browse_open(false);
            update_settings();
        } catch (const std::exception&) {
            ui_->set_browse_error("无法读取所选路径");
        }
    }

    bool raw_edited() const { return str(ui_->get_raw_draft()) != raw_snapshot_; }

    void set_raw() {
        raw_snapshot_ = draft_.dump(4);
        ui_->set_raw_draft(ss(raw_snapshot_));
    }

    bool apply_raw(const std::string& raw) {
        try {
            auto next = Json::parse(raw);
            auto next_catalog = verified_catalog(paths_.root, next);
            draft_ = std::move(next);
            catalog_ = std::move(next_catalog);
            invalid_.clear();
            set_raw();
            show_config("JSON 校验通过", false);
            update_dirty();
            return true;
        } catch (const std::exception& error) {
            show_config(error.what(), true);
            return false;
        }
    }

    void edit(const std::string& key, const std::string& value) {
        const auto field = settings_.edits.find(key);
        if (field == settings_.edits.end()) return;
        bool redraw = false;
        try {
            field->second.apply(draft_, value);
            redraw = field->second.redraw;
            invalid_.erase(key);
            show_config(invalid_.empty() ? "" : invalid_.begin()->second.second, !invalid_.empty());
        } catch (const std::exception& error) {
            const auto message = field->second.title + " · " + error.what();
            if (!field->second.redraw) invalid_[key] = {value, message};
            show_config(message, true);
            redraw = field->second.redraw;
        }
        if (redraw) {
            if (key.starts_with("/policies/event/fixed_choices/")) update_event_selection(key);
            else update_settings();
        }
        update_dirty();
    }

    void update_event_selection(const std::string& key) {
        const Json::json_pointer path(key);
        const auto selected = draft_.contains(path) ? draft_.at(path).dump() : std::string{};
        const auto rows = ui_->get_settings();
        // Updating the existing rows preserves keyboard focus and scroll position.
        for (std::size_t i = 0; i < settings_.fields.size(); ++i) {
            auto& field = settings_.fields[i];
            if (field.kind != FieldKind::event_option || field.key != key) continue;
            const bool checked = field.value == selected;
            if (field.checked == checked) continue;
            field.checked = checked;
            auto row = rows->row_data(i).value();
            row.checked = checked;
            rows->set_row_data(i, row);
        }
    }

    slint::Image unit_portrait(Id id) {
        if (id <= 0) return {};
        auto [entry, inserted] = unit_portraits_.try_emplace(id);
        if (inserted) {
            const auto path = paths_.root / "data" / "unit_portraits" / (std::to_string(id) + ".png");
            std::error_code error;
            if (fs::is_regular_file(path, error)) entry->second = slint::Image::load_from_path(ss(path_utf8(path)));
        }
        return entry->second;
    }

    auto field_rows(const std::vector<Field>& fields) {
        std::vector<SettingRow> rows;
        for (const auto& field : fields) {
            auto value = field.value;
            if (const auto invalid = invalid_.find(field.key); invalid != invalid_.end()) value = invalid->second.first;
            SettingRow row{};
            row.key = ss(field.key);
            row.kind = static_cast<int>(field.kind);
            row.title = ss(field.title);
            row.hint = ss(field.hint);
            row.value = ss(value);
            row.checked = field.checked;
            row.options = strings(field.options);
            row.selected = field.selected;
            row.numeric = field.numeric;
            row.path_kind = static_cast<int>(field.path_kind);
            row.order_group = ss(field.order_group);
            row.order_index = field.order_index;
            row.order_count = field.order_count;
            if (!field.toggle_options.empty()) {
                std::vector<SettingToggleOption> options;
                for (const auto& option : field.toggle_options) {
                    row.portrait_options |= option.unit_id > 0;
                    options.push_back({ss(option.key), ss(option.label), option.checked,
                                       unit_portrait(option.unit_id)});
                }
                row.toggle_minimum = field.toggle_minimum;
                row.toggle_exclusive = field.toggle_exclusive;
                row.toggle_selected_count = static_cast<int>(
                    std::ranges::count(field.toggle_options, true, &ToggleOption::checked));
                row.toggle_options = model(std::move(options));
            }
            rows.push_back(std::move(row));
        }
        return model(std::move(rows));
    }

    void update_settings() {
        try {
            const auto query = ui_->get_ticket_query();
            auto settings = make_settings_page(draft_, catalog_, section_, subpage_, str(query), ticket_index_);
            if (settings.subpages != settings_.subpages) ui_->set_subpages(strings(settings.subpages));
            settings_ = std::move(settings);
            ui_->set_section(section_);
            ui_->set_section_title(ss(settings_.title));
            ui_->set_section_description(ss(settings_.description));
            std::vector<bool> groups;
            for (int section = 0; section < static_cast<int>(section_names().size()); ++section)
                groups.push_back(!section_subpages(catalog_, section).empty());
            ui_->set_section_groups(model(std::move(groups)));
            ui_->set_subpage_index(subpage_);
            ui_->set_settings(field_rows(settings_.fields));
            const auto& picker = settings_.ticket_picker;
            const auto buttons = [this](const std::vector<RankedOption>& units) {
                std::vector<RankedButton> rows;
                rows.reserve(units.size());
                for (const auto& unit : units)
                    rows.push_back({ss(unit.key), ss(unit.label), unit.rank,
                                    unit.unit_id ? unit_portrait(unit.unit_id) : slint::Image{}});
                return model(std::move(rows));
            };
            ui_->set_ticket_picker({strings(picker.labels), picker.index, static_cast<int>(picker.total),
                buttons(picker.candidates), buttons(picker.priority)});
            ui_->set_priority_options(buttons(settings_.priority_options));
            std::vector<SettingsMatrix> matrices;
            for (const auto& table : settings_.matrices) {
                std::vector<MatrixRow> rows;
                for (const auto& [label, cells] : table.rows) {
                    std::vector<MatrixCell> data;
                    for (const auto& cell : cells) {
                        auto value = cell.value;
                        if (const auto invalid = invalid_.find(cell.key); invalid != invalid_.end()) value = invalid->second.first;
                        MatrixCell rendered;
                        rendered.key = ss(cell.key);
                        rendered.value = ss(value);
                        rendered.is_boolean = cell.is_boolean;
                        rendered.checked = cell.checked;
                        data.push_back(std::move(rendered));
                    }
                    MatrixRow row;
                    row.label = ss(label);
                    row.cells = model(std::move(data));
                    rows.push_back(std::move(row));
                }
                SettingsMatrix rendered;
                rendered.title = ss(table.title);
                rendered.headers = strings(table.headers);
                rendered.rows = model(std::move(rows));
                matrices.push_back(std::move(rendered));
            }
            ui_->set_matrices(model(std::move(matrices)));
            ui_->set_opening_map_columns(strings(settings_.opening_map_columns));
        } catch (const std::exception& error) {
            show_config(error.what(), true);
        }
    }

    void update_dirty() {
        const bool changed = saved_ ? draft_ != *saved_ : !draft_.is_null();
        ui_->set_dirty(changed || !invalid_.empty());
    }

    void update_endpoint(const Json& data) {
        const auto config = parse_config(data);
        ui_->set_endpoint(ss(config.host + ":" + std::to_string(config.port)));
    }

    void show_notice(const std::string& message, bool error) {
        ui_->set_notice(ss(message));
        ui_->set_notice_error(error);
    }

    void show_config(const std::string& message, bool error) {
        ui_->set_config_message(ss(message));
        ui_->set_config_error(error);
    }

    void start(const fs::path& config, bool provisional = false) {
        worker_.start(paths_.root, config, paths_.log, !provisional && ui_->get_decisions_enabled(), queue_);
        ui_->set_busy(true);
    }

    void remove_candidate() {
        if (candidate_path_.empty()) return;
        std::error_code ignored;
        fs::remove(candidate_path_, ignored);
        candidate_path_.clear();
    }

    void save() {
        if (hot_reload_.valid() || restart_pending_ || starting_candidate_) return;
        if (!invalid_.empty()) {
            show_config(invalid_.begin()->second.second, true);
            return;
        }
        try {
            auto next = parse_config(draft_);
            if (!config_file_unchanged(paths_.config, saved_))
                throw std::runtime_error("配置文件已被外部修改，请重新打开控制台后编辑");

            candidate_ = draft_;
            if (worker_.online() && saved_) {
                const auto previous = parse_config(*saved_);
                if (next.host == previous.host && next.port == previous.port) {
                    hot_reload_ = worker_.reload(paths_.root, paths_.config, std::move(next), candidate_, saved_);
                    ui_->set_busy(true);
                    show_config("正在应用配置", false);
                    show_notice("", false);
                    return;
                }
            }
            candidate_catalog_ = verified_catalog(paths_.root, candidate_);
            candidate_path_ = sibling_temp(paths_.config, ".candidate.");
            save_json(candidate_path_, candidate_);
            restart_pending_ = true;
            ui_->set_busy(true);
            show_config("正在重启服务", false);
            show_notice("", false);
            worker_.stop();
        } catch (const std::exception& error) {
            remove_candidate();
            show_config(error.what(), true);
        }
    }

    void apply_candidate() {
        try {
            update_endpoint(candidate_);
            start(candidate_path_, true);
            starting_candidate_ = true;
        } catch (const std::exception& error) {
            rollback(error.what());
        }
    }

    void rollback(std::string failure) {
        starting_candidate_ = false;
        restart_pending_ = false;
        show_config("新配置启动失败 · " + failure, true);
        show_notice("新配置启动失败 · " + failure, true);
        remove_candidate();
        if (saved_) {
            try {
                if (!config_file_unchanged(paths_.config, saved_)) throw std::runtime_error("配置文件已被外部修改");
                update_endpoint(*saved_);
                start(paths_.config);
                return;
            } catch (const std::exception& error) {
                show_notice("恢复上次配置失败 · " + std::string(error.what()), true);
            }
        }
        ui_->set_busy(false);
    }

    void finish_save(Json catalog, std::string_view message) {
        const auto pending_edits = draft_ != candidate_ || !invalid_.empty() ||
            (section_ == raw_section && raw_edited());
        saved_ = std::move(candidate_);
        const auto limit = static_cast<std::size_t>(parse_config(*saved_).log_history_limit);
        if (limit != history_limit_) {
            const bool expanding = history_limit_ && (!limit || limit > history_limit_);
            history_limit_ = limit;
            if (expanding) read_history();
            trim_history();
        }
        catalog_ = std::move(catalog);
        log_catalog_ = catalog_;
        for (const auto& entry : history_) entry->rendered.reset();
        displayed_.reset();
        refresh_logs();
        update_settings();
        update_dirty();
        ui_->set_busy(false);
        show_config(pending_edits ? "已保存上次修改，当前还有未保存的更改" : std::string(message), false);
    }

    void service_tick() {
        if (hot_reload_.valid() && hot_reload_.wait_for(0ms) == std::future_status::ready) {
            try {
                finish_save(hot_reload_.get(), "已保存并生效");
            } catch (const std::exception& error) {
                show_config("应用配置失败 · " + std::string(error.what()), true);
            }
        }
        if (worker_.finished()) {
            const auto error = worker_.join_finished();
            if (starting_candidate_) {
                auto failure = std::exchange(candidate_failure_, {});
                if (failure.empty()) failure = error.empty() ? "服务提前退出" : error;
                rollback(std::move(failure));
                return;
            }
            if (!restart_pending_ && !error.empty()) show_notice(error, true);
        }
        if (restart_pending_ && !worker_.running()) {
            restart_pending_ = false;
            apply_candidate();
            return;
        }
        const bool online = worker_.online();
        ui_->set_online(online);
        if (online || !worker_.running()) ui_->set_initial_startup(false);
        ui_->set_busy(hot_reload_.valid() || restart_pending_ || starting_candidate_ || (!online && worker_.running()));
        const auto device = online ? worker_.backend() : "";
        if (str(ui_->get_device()) != device) ui_->set_device(ss(device));
        if (online && starting_candidate_ && candidate_failure_.empty()) {
            try {
                if (!config_file_unchanged(paths_.config, saved_)) throw std::runtime_error("配置文件已被外部修改");
                fs::rename(candidate_path_, paths_.config);
            } catch (const std::exception& error) {
                candidate_failure_ = error.what();
                worker_.stop();
                return;
            }
            candidate_path_.clear();
            starting_candidate_ = false;
            worker_.enable(ui_->get_decisions_enabled());
            finish_save(std::move(candidate_catalog_), "已保存，服务已重启");
        }
    }

    void trim_history() {
        while (history_limit_ && history_.size() > history_limit_) history_.pop_front();
    }

    void store_entry(Entry entry) {
        entry.sequence = ++sequence_;
        history_.push_back(std::make_shared<Entry>(std::move(entry)));
        trim_history();
    }

    void read_history() {
        std::ifstream input(paths_.log, std::ios::binary);
        if (!input) return;
        const auto [start, end] = history_window(input, history_limit_);
        if (input.bad()) return;
        input.seekg(start);
        if (!input) return;

        history_.clear();
        selected_.reset();
        loaded_file_end_ = end;
        std::string line;
        auto position = start;
        while (position < end && std::getline(input, line)) {
            position += static_cast<std::streamoff>(line.size()) + 1;
            if (position > end || line.size() > queue_bytes_limit) continue;
            auto record = Json::parse(line, nullptr, false);
            if (record.is_object()) store_entry({std::move(record), line.size()});
        }
    }

    void tick() {
        service_tick();
        auto [incoming, dropped] = queue_.drain();
        skipped_ += dropped;
        for (auto& entry : incoming)
            if (!entry.file_end || entry.file_end > loaded_file_end_) store_entry(std::move(entry));
        if (!incoming.empty() || dropped) logs_dirty_ = true;
        if (logs_dirty_ && ui_->get_page() == 0 && !ui_->get_frozen()) refresh_logs();
    }

    bool matches(const Entry& entry, const std::string& uid, std::string_view kind, int outcome,
                 std::string_view query) const {
        if (!uid.empty() && text(entry.value, "uid") != uid) return false;
        if (!kind.empty() && text(entry.value, "kind") != kind) return false;
        if (!query.empty() && text(entry.value, "request_id").find(query) == std::string::npos) return false;
        return outcome == 0 || outcome == static_cast<int>(log_outcome(entry.value));
    }

    const std::array<std::string, 6>& columns(Entry& entry) const {
        if (!entry.rendered) {
            const auto& value = entry.value;
            auto result = brief_response(value, log_catalog_);
            if (const auto comparison = score_comparison(value); !comparison.empty())
                result += " · " + comparison;
            entry.rendered = std::array{local_time(text(value, "timestamp"), false),
                account_label(value), kind_label(text(value, "kind")),
                brief_request(value, log_catalog_), std::move(result), milliseconds(value, 1)};
        }
        return *entry.rendered;
    }

    void refresh_logs() {
        logs_dirty_ = false;
        updating_logs_ = true;
        const auto old = ui_->get_account_index();
        const auto uid = old > 0 && static_cast<std::size_t>(old) < accounts_.size() ?
            accounts_[static_cast<std::size_t>(old)] : "";
        std::map<std::string, std::string> known;
        for (const auto& entry : history_) {
            auto account = text(entry->value, "uid");
            if (account.empty()) continue;
            auto name = text(entry->value, "account_name");
            if (name.empty()) known.try_emplace(std::move(account));
            else known.insert_or_assign(std::move(account), std::move(name));
        }
        // Keep the active filter if its last row aged out of the ring buffer.
        if (!uid.empty()) known.try_emplace(uid);
        std::map<std::string, std::size_t> label_counts;
        for (const auto& [account, name] : known) ++label_counts[name.empty() ? account : name];
        std::vector<std::string> accounts{""};
        std::vector<std::string> labels{"全部账号"};
        for (const auto& [account, name] : known) {
            accounts.push_back(account);
            const auto& label = name.empty() ? account : name;
            labels.push_back(label_counts.at(label) > 1 ? label + " · " + account : label);
        }
        if (accounts != accounts_ || labels != account_labels_) {
            const auto selected = std::ranges::find(accounts, uid);
            ui_->set_account_options(strings(labels));
            ui_->set_account_index(selected == accounts.end() ? 0 : static_cast<int>(selected - accounts.begin()));
            accounts_ = std::move(accounts);
            account_labels_ = std::move(labels);
        }
        const auto kind_index = std::clamp(ui_->get_kind_index(), 0, static_cast<int>(kinds.size() - 1));
        const auto kind = kinds[static_cast<std::size_t>(kind_index)].first;
        const auto query = str(ui_->get_query());
        const auto outcome = ui_->get_outcome_index();

        struct VisibleRow {
            std::shared_ptr<Entry> entry;
            std::array<std::string, 6> columns;
        };
        std::vector<VisibleRow> rows;
        for (auto i = history_.rbegin(); i != history_.rend(); ++i) {
            if (matches(**i, uid, kind, outcome, query)) rows.push_back({*i, columns(**i)});
        }
        if (sort_column_ >= 0 && sort_column_ < 6) {
            std::stable_sort(rows.begin(), rows.end(), [this](const VisibleRow& a, const VisibleRow& b) {
                const auto less = [this](const VisibleRow& left, const VisibleRow& right) {
                    const auto column = static_cast<std::size_t>(sort_column_);
                    if (column == 5) {
                        const auto& l = item(left.entry->value, "elapsed_ms");
                        const auto& r = item(right.entry->value, "elapsed_ms");
                        return l < r;
                    }
                    if (!column) return text(left.entry->value, "timestamp") < text(right.entry->value, "timestamp");
                    return left.columns[column] < right.columns[column];
                };
                return ascending_ ? less(a, b) : less(b, a);
            });
        }
        std::vector<LogRow> table;
        visible_.clear();
        int selected_row = -1;
        for (const auto& row : rows) {
            if (selected_ && row.entry->sequence == *selected_) selected_row = static_cast<int>(visible_.size());
            std::vector<slint::StandardListViewItem> values;
            for (const auto& cell : row.columns) values.push_back({slint::SharedString(cell)});
            table.push_back({model(std::move(values)), log_outcome(row.entry->value) != LogOutcome::decision});
            visible_.push_back(row.entry);
        }
        ui_->set_log_rows(model(std::move(table)));
        ui_->set_log_selected_row(selected_row);
        ui_->set_log_count(ss(std::to_string(visible_.size()) + " / " + std::to_string(history_.size()) + " 条" +
            (skipped_ ? " · 内存缓冲跳过 " + std::to_string(skipped_) + " 条，可查日志文件" : "")));
        if (selected_row < 0) show_entry(nullptr);
        else show_entry(visible_[static_cast<std::size_t>(selected_row)].get());
        updating_logs_ = false;
    }

    void show_entry(const Entry* entry) {
        const auto displayed = entry ? entry->sequence : 0;
        if (displayed_ == displayed) return;
        displayed_ = displayed;
        if (!entry) {
            ui_->set_detail_title("选择一条日志查看详情");
            ui_->set_detail_meta("");
            ui_->set_request_summary("");
            ui_->set_response_summary("");
            ui_->set_request_json("");
            ui_->set_response_json("");
            return;
        }
        const auto& record = entry->value;
        ui_->set_detail_title(ss(kind_label(text(record, "kind")) + " · " + local_time(text(record, "timestamp"), true)));
        auto id = text(record, "request_id");
        ui_->set_detail_meta(ss("账号 " + account_label(record) + " · HTTP " +
            text(record, "http_status") + " · " + milliseconds(record, 3) +
            "\n请求 ID " + (id.empty() ? "—" : id)));
        ui_->set_request_summary(ss(full_request(record, log_catalog_)));
        ui_->set_response_summary(ss(full_response(record, log_catalog_)));
        ui_->set_request_json(ss(item(record, "request").dump(2)));
        ui_->set_response_json(ss(item(record, "response").dump(2)));
    }

    Paths paths_;
    slint::ComponentHandle<ConsoleWindow> ui_;
    Json draft_ = {{"schema_version", 1}};
    Json catalog_;
    Json log_catalog_;
    std::optional<Json> saved_;
    Json candidate_;
    Json candidate_catalog_;
    fs::path candidate_path_;
    std::string candidate_failure_;
    SettingsPage settings_;
    std::unordered_map<Id, slint::Image> unit_portraits_;
    std::map<std::string, std::pair<std::string, std::string>> invalid_;
    std::string raw_snapshot_;
    LogBuffer queue_;
    ServerWorker worker_;
    std::future<Json> hot_reload_;
    slint::Timer timer_;
    std::deque<std::shared_ptr<Entry>> history_;
    std::vector<std::shared_ptr<Entry>> visible_;
    std::vector<std::string> accounts_{""};
    std::vector<std::string> account_labels_{"全部账号"};
    std::optional<std::uint64_t> selected_;
    std::optional<std::uint64_t> displayed_;
    std::uint64_t sequence_ = 0;
    std::size_t history_limit_ = log_history_limits.front();
    std::streamoff loaded_file_end_ = 0;
    std::size_t skipped_ = 0;
    std::string browse_key_;
    fs::path browse_location_;
    int section_ = 0;
    int subpage_ = 0;
    int ticket_index_ = 0;
    int sort_column_ = 0;
    bool ascending_ = false;
    bool updating_logs_ = false;
    bool logs_dirty_ = false;
    bool restart_pending_ = false;
    bool starting_candidate_ = false;
};

} // namespace
} // namespace labyrinth::gui

namespace {
int run_gui(const std::vector<std::string>& args) {
    try {
        labyrinth::gui::Console console(labyrinth::gui::paths(args));
        console.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Labyrinth GUI: " << error.what() << '\n';
        return 1;
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(labyrinth::path_utf8(std::filesystem::path(argv[i])));
    return run_gui(args);
}
#else
int main(int argc, char* argv[]) {
    return run_gui({argv, argv + argc});
}
#endif
