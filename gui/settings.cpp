#include "settings.hpp"
#ifdef _WIN32
#include "windows_gpu.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace labyrinth::gui {
namespace {
using Pointer = Json::json_pointer;
using NamedIds = std::vector<std::pair<int, std::string>>;

Json get(const Json& source, const std::string& key, Json fallback = nullptr) {
    const Pointer pointer(key);
    return source.contains(pointer) ? source.at(pointer) : std::move(fallback);
}

const Json& catalog_rows(const Json& catalog, std::string_view table) {
    static const Json empty = Json::array();
    const auto tables = catalog.find("tables");
    if (tables == catalog.end() || !tables->is_object()) return empty;
    const auto rows = tables->find(table);
    return rows != tables->end() && rows->is_array() ? *rows : empty;
}

template<class Integer>
Integer integer(std::string_view text, Integer minimum, Integer maximum) {
    Integer number{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, number);
    if (text.empty() || result.ec != std::errc{} || result.ptr != end ||
        number < minimum || number > maximum) {
        throw std::runtime_error("请输入 " + std::to_string(minimum) + " 至 " + std::to_string(maximum) + " 的整数");
    }
    return number;
}

std::string description(const Json& row, std::string_view key) {
    auto value = row.find(key);
    return value != row.end() && value->is_string() ? tidy(value->get<std::string>()) : "";
}

std::string unit_nickname(const Json& catalog, int id) {
    const auto nicknames = catalog.find("unit_nicknames");
    return nicknames != catalog.end() && nicknames->is_object()
        ? description(*nicknames, std::to_string(id / 100)) : std::string{};
}

NamedIds names(const Json& catalog, std::string_view table, std::string_view key,
               std::string_view name, int region = 0) {
    NamedIds result;
    std::set<int> ids;
    for (const auto& row : catalog_rows(catalog, table)) {
        if (region && row.value("region", 0) != region) continue;
        const auto id = row.value(key, 0);
        if (id <= 0 || !ids.insert(id).second) continue;
        auto label = table == "unit_data" && name == "unit_name" ? unit_nickname(catalog, id) : std::string{};
        if (label.empty()) label = description(row, name);
        result.emplace_back(id, std::move(label));
    }
    return result;
}

std::string id_label(const NamedIds& values, int id) {
    const auto found = std::ranges::find(values, id, &NamedIds::value_type::first);
    return found == values.end() || found->second.empty() ? "#" + std::to_string(id)
        : found->second + "  ·  " + std::to_string(id);
}

Ids ids_at(const Json& draft, const std::string& key, const Ids& fallback = {}) {
    return get(draft, key, fallback).get<Ids>();
}

bool move_priority(Ids& items, Ids::iterator from, int delta) {
    const auto target = (from - items.begin()) + delta;
    if (from == items.end() || target < 0 || target >= std::ssize(items) || delta == 0) return false;
    const auto to = items.begin() + target;
    if (from < to) std::rotate(from, from + 1, to + 1);
    else std::rotate(to, from, from + 1);
    return true;
}

std::unordered_set<Id> official_unit_matches(const Json& catalog, std::string_view query) {
    std::unordered_set<Id> matches;
    if (!query.empty())
        for (const auto& row : catalog_rows(catalog, "unit_data"))
            if (description(row, "unit_name").find(query) != std::string::npos)
                matches.insert(row.value("unit_id", 0));
    return matches;
}

// JSON Pointer interprets numeric tokens under null as array indices. The
// settings schema also has numeric-keyed maps, so initialize their parents
// explicitly and materialize fields the native parser requires together.
void prepare_edit(Json& config, std::string_view path) {
    constexpr std::string_view policies = "/policies/";
    if (!path.starts_with(policies)) return;
    const auto rest = path.substr(policies.size());
    const auto separator = rest.find('/');
    const auto policy = std::string(rest.substr(0, separator));
    auto& value = config["policies"][policy];
    if (value.is_null())
        value = policy == "ticket_clear" ? Json::object() : Json{{"enabled", false}};
    if (policy == "select_ticket") {
        if (path.starts_with("/policies/select_ticket/opening_ticket_units/") &&
            !value.contains("opening_ticket_units")) value["opening_ticket_units"] = Json::object();
        if (path.starts_with("/policies/select_ticket/ticket_priority_units/") &&
            !value.contains("ticket_priority_units")) value["ticket_priority_units"] = Json::object();
    } else if (policy == "score_farm") {
        if (!value.contains("minimum_score_by_guild"))
            value["minimum_score_by_guild"] = Json::object();
    } else if (policy == "opening") {
        if (!value.contains("regions")) value["regions"] = Json::object();
        if (!value.contains("boss_unit_ids")) value["boss_unit_ids"] = Json::object();
    } else if (policy == "event") {
        if (path.starts_with("/policies/event/fixed_choices/") && !value.contains("fixed_choices"))
            value["fixed_choices"] = Json::object();
    } else if (policy == "team" && path.starts_with("/policies/team/simulator/")) {
        if (!value.contains("simulator"))
            value["simulator"] = {{"enabled", false}, {"url", default_simulator_url}};
    } else if (policy == "shop") {
        constexpr std::string_view regions = "/policies/shop/regions/";
        if (path.starts_with(regions)) {
            const auto suffix = path.substr(regions.size());
            auto& region = value["regions"][std::string(suffix.substr(0, suffix.find('/')))];
            if (region.is_null()) region = {{"purchase", false}, {"reserve_gold", 0}};
        } else if (path.starts_with("/policies/shop/model/") && !value.contains("model")) {
            value["model"] = {
                {"base_prices", {{"random_imprint", 1200}, {"choice_imprint", 2800},
                                 {"relic", {1800, 2800, 3800}}}},
                {"refresh_fees", {300, 600}}, {"scores", {{"imprint", 100}, {"relic", 200}}},
                {"regions", Json::object()}
            };
        }
    }
}

struct Choice {
    std::string label;
    Json value;
};

struct Builder {
    const Json& draft;
    SettingsPage page;
    std::vector<Field>& fields() { return page.fields; }

    void heading(std::string title) {
        fields().push_back({{}, FieldKind::heading, std::move(title), {}, {}});
    }

    void note(std::string title, std::string value, std::string hint = {}) {
        fields().push_back({{}, FieldKind::note, std::move(title), std::move(hint), std::move(value)});
    }

    void register_edit(std::string key, const std::string& title,
                       std::function<void(Json&, std::string_view)> change, bool redraw = false,
                       std::function<void(Json&, int)> move = {}) {
        auto target = key;
        page.edits.emplace(std::move(key), Edit{title,
            [target = std::move(target), change = std::move(change)](Json& config, std::string_view input) {
                auto draft = config;
                prepare_edit(draft, target);
                change(draft, input);
                config = std::move(draft);
            }, std::move(move), redraw});
    }

    bool toggle_edit(const std::string& key, const std::string& title, bool fallback = false) {
        register_edit(key, title, [key](Json& config, std::string_view text) {
            config[Pointer(key)] = text == "true";
        });
        return get(draft, key, fallback).get<bool>();
    }

    void toggle(const std::string& key, std::string title, bool fallback = false, std::string hint = {}) {
        const bool checked = toggle_edit(key, title, fallback);
        fields().push_back({key, FieldKind::toggle, std::move(title), std::move(hint), {},
            checked});
    }

    void text(const std::string& key, std::string title, std::string fallback,
              PathKind path_kind = PathKind::none) {
        register_edit(key, title, [key](Json& config, std::string_view input) {
            if (input.empty()) throw std::runtime_error("请输入路径或地址");
            config[Pointer(key)] = input;
        });
        fields().push_back({key, FieldKind::input, std::move(title), {},
            get(draft, key, std::move(fallback)).get<std::string>()});
        fields().back().path_kind = path_kind;
    }

    void numeric_edit(const std::string& key, const std::string& title, int minimum, int maximum) {
        register_edit(key, title, [key, minimum, maximum](Json& config, std::string_view text) {
            config[Pointer(key)] = integer(text, minimum, maximum);
        });
    }

    void number(const std::string& key, std::string title, int fallback = 0,
                int minimum = 0, int maximum = std::numeric_limits<int>::max(), std::string hint = {}) {
        numeric_edit(key, title, minimum, maximum);
        fields().push_back({key, FieldKind::input, std::move(title), std::move(hint),
            get(draft, key, fallback).dump()});
        fields().back().numeric = minimum >= 0;
    }

    Cell numeric_cell(const std::string& key, const std::string& title, int fallback = 0,
                      int maximum = std::numeric_limits<int>::max()) {
        numeric_edit(key, title, 0, maximum);
        return {key, get(draft, key, fallback).dump()};
    }

    Cell toggle_cell(const std::string& key, const std::string& title) {
        return {key, {}, true, toggle_edit(key, title)};
    }

    void choice(const std::string& key, const std::string& title, std::vector<Choice> options,
                Json current, std::string hint = {},
                std::function<void(Json&, const Json&)> change = {}, bool redraw = false) {
        Field field{key, FieldKind::choice, title, std::move(hint), {}};
        field.selected = -1;
        std::vector<Json> values;
        for (auto& option : options) {
            if (current == option.value) field.selected = static_cast<int>(values.size());
            field.options.push_back(std::move(option.label));
            values.push_back(std::move(option.value));
        }
        if (!change) change = [key](Json& config, const Json& value) { config[Pointer(key)] = value; };
        register_edit(key, title, [values = std::move(values), change = std::move(change)]
            (Json& config, std::string_view text) {
                if (values.empty()) throw std::runtime_error("没有可选项");
                const auto index = integer<std::size_t>(text, 0, values.size() - 1);
                change(config, values.at(index));
            }, redraw);
        fields().push_back(std::move(field));
    }

    void toggle_group(const std::string& key, const NamedIds& options,
                      const Ids& defaults, int minimum = 0, bool portraits = false) {
        const auto selection = [key, defaults, minimum](const Json& config) {
            auto ids = ids_at(config, key, defaults);
            if (minimum > 0 && ids.empty()) return defaults;
            return ids;
        };
        const auto selected = selection(draft);
        Field group{key, FieldKind::button_group, {}, {}, {}};
        group.toggle_minimum = minimum;
        for (const auto& [id, label] : options) {
            const auto option = key + "/@" + std::to_string(id);
            register_edit(option, label, [key, id, minimum, selection](Json& config, std::string_view input) {
                auto items = selection(config);
                if (input == "true") {
                    if (std::ranges::find(items, id) == items.end()) items.push_back(id);
                } else {
                    if (std::ssize(items) <= minimum) return;
                    std::erase(items, id);
                }
                config[Pointer(key)] = std::move(items);
            });
            group.toggle_options.push_back({option, label, std::ranges::find(selected, id) != selected.end(),
                                            portraits ? id : 0});
        }
        fields().push_back(std::move(group));
    }

    void ordered_ids(const std::string& key, const std::string& title, const NamedIds& available,
                     const Ids& fallback = {}) {
        const auto selected = ids_at(draft, key, fallback);
        auto values = available;
        for (auto id : selected) {
            if (std::ranges::find(values, id, &NamedIds::value_type::first) == values.end())
                values.emplace_back(id, "");
        }
        std::vector<Choice> options;
        options.push_back({"移除 / 留空", nullptr});
        for (const auto& item : values) options.push_back({id_label(values, item.first), item.first});
        const auto rows = selected.size() + 1;
        for (std::size_t index = 0; index < rows; ++index) {
            const auto field = key + "/@" + std::to_string(index);
            const auto current = index < selected.size() ? Json(selected[index]) : Json(nullptr);
            const bool adding = index == selected.size();
            const auto label = adding ? "添加" + title : title + " " + std::to_string(index + 1);
            auto row_options = options;
            choice(field, label, std::move(row_options), current, {}, [key, index, fallback](Json& config, const Json& next) {
                auto items = ids_at(config, key, fallback);
                if (next.is_null()) {
                    if (index < items.size()) items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
                } else {
                    const auto id = next.get<int>();
                    const auto same = std::ranges::find(items, id);
                    if (same != items.end()) {
                        if (index >= items.size()) throw std::runtime_error("名单中已有此项");
                        std::swap(items[index], *same);
                    } else if (index < items.size()) {
                        items[index] = id;
                    } else {
                        items.push_back(id);
                    }
                }
                config[Pointer(key)] = items;
            }, true);
            if (index < selected.size()) {
                auto& row = fields().back();
                row.order_group = key;
                row.order_index = static_cast<int>(index);
                row.order_count = static_cast<int>(selected.size());
                page.edits.at(field).move = [key, index, fallback](Json& config, int delta) {
                    auto items = ids_at(config, key, fallback);
                    if (index >= items.size() ||
                        !move_priority(items, items.begin() + static_cast<std::ptrdiff_t>(index), delta)) return;
                    prepare_edit(config, key);
                    config[Pointer(key)] = std::move(items);
                };
            }
        }
    }
};

void server_page(Builder& b) {
    b.page.description = "修改监听地址或端口会重启服务，游戏数据可在线更新。";
    b.text("/server/host", "监听地址", "127.0.0.1");
    b.number("/server/port", "监听端口", 8081, 1, 65535, "客户端须使用同一端口");
    b.text("/server/catalog", "游戏数据 JSON", "data/game_catalog.json", PathKind::file);
    const std::string key = "/server/log_history_limit";
    std::vector<Choice> choices;
    for (const int limit : log_history_limits)
        choices.push_back({limit ? std::to_string(limit) + " 条" : "无限", limit});
    b.choice(key, "最近日志条数", std::move(choices), get(b.draft, key, log_history_limits.front()),
             "控制决策日志页可查看的历史范围");
}

void score_page(Builder& b, const Json& catalog) {
    b.page.description = "按公会配置最低分，判定时计入最终首领的战斗通关分。";
    b.toggle("/policies/score_farm/enabled", "启用刷分判断");
    Table table{"公会最低分", {"最低分"}, {}};
    for (const auto& [id, name] : names(catalog, "labyrinth_enter_guild", "guild_id", "guild_name")) {
        table.rows.emplace_back(name, std::vector{b.numeric_cell(
            "/policies/score_farm/minimum_score_by_guild/" + std::to_string(id), name + "最低分")});
    }
    if (!table.rows.empty()) b.page.matrices.push_back(std::move(table));
    else b.note("公会名单", "游戏数据 JSON 中暂无公会记录");
}

void ticket_clear_page(Builder& b, const Json& catalog) {
    b.page.description = "通关后，通行证多于保留票数时，跳过奖励展示并继续开局。";
    b.number("/policies/ticket_clear/reserve_tickets", "保留票数", default_passport_reserve, -1);
    const auto guilds = names(catalog, "labyrinth_enter_guild", "guild_id", "guild_name");
    if (guilds.empty()) return;
    b.heading("清票公会");
    b.toggle_group("/policies/ticket_clear/guild_ids", guilds, {default_ticket_clear_guild}, 1);
}

void opening_page(Builder& b, const Json& catalog, int subpage) {
    if (subpage == 1) {
        b.toggle("/policies/select_ticket/opening_enabled", "启用开局自动选人");
        const auto tickets = catalog.contains("opening_tickets") ? parse_opening_tickets(catalog) :
            std::vector<OpeningTicket>{};
        if (tickets.empty()) {
            b.note("开局选人", "游戏数据 JSON 中暂无开局票配置");
            return;
        }
        const auto guilds = names(catalog, "labyrinth_enter_guild", "guild_id", "guild_name");
        const auto units = names(catalog, "unit_data", "unit_id", "unit_name");
        const auto& rosters = catalog_rows(catalog, "labyrinth_opening_units");
        if (rosters.empty()) b.note("当前游戏数据暂无公会候选表，显示可选角色。", "");
        for (const auto& ticket : tickets) {
            const auto guild = std::ranges::find(guilds, ticket.guild_id, &NamedIds::value_type::first);
            b.heading(guild == guilds.end() ? "公会 " + std::to_string(ticket.guild_id) : guild->second);
            std::unordered_set<Id> members;
            for (const auto& row : rosters)
                if (row.value("ticket_id", 0) == ticket.ticket_id) members.insert(row.value("unit_id", 0));
            NamedIds options;
            for (const auto& unit : units)
                if (!is_fes_unit(unit.first) && (members.empty() || members.contains(unit.first)))
                    options.push_back(unit);
            const auto key = "/policies/select_ticket/opening_ticket_units/" + std::to_string(ticket.ticket_id);
            const auto& defaults = ticket.default_units;
            for (const auto id : ids_at(b.draft, key, defaults)) {
                if (is_fes_unit(id)) continue;
                if (std::ranges::find(options, id, &NamedIds::value_type::first) != options.end()) continue;
                auto label = catalog_name(catalog, "unit_data", "unit_id", "unit_name", id);
                options.emplace_back(id, label.starts_with("#") ? "未收录角色" : std::move(label));
            }
            for (auto& option : options)
                if (option.second.empty()) option.second = "未收录角色";
            b.toggle_group(key, options, defaults, 0, true);
        }
        return;
    }
    b.toggle("/policies/opening/enabled", "开局筛选");
    b.number("/policies/opening/max_attempts", "最多开局次数", 100, 1,
             std::numeric_limits<int>::max(), "计入首次开局");
    b.toggle("/policies/path/enabled", "自动寻路");
    for (const int region : {3, 5}) {
        const auto floor = std::to_string(region);
        b.heading("第 " + floor + " 层首领名单");
        auto choices = names(catalog, "labyrinth_boss_units", "unit_id", "unit_name", region);
        const std::string key = "/policies/opening/boss_unit_ids/" + floor;
        for (const int id : ids_at(b.draft, key)) {
            if (std::ranges::find(choices, id, &NamedIds::value_type::first) != choices.end()) continue;
            auto label = catalog_name(catalog, "unit_data", "unit_id", "unit_name", id);
            choices.emplace_back(id, label.starts_with("#") ? "未收录首领" : std::move(label));
        }
        Ids all;
        for (auto& [id, label] : choices) {
            all.push_back(id);
            if (label.empty()) label = "未收录首领";
        }
        if (choices.empty()) b.note("首领名单", "游戏数据 JSON 中暂无此区域的首领条目");
        else b.toggle_group(key, choices, all, 1);
    }

    Table table{"区域格子下限", {"普通", "极难", "印记", "事件", "遗物", "商店", "首领"}, {}};
    for (int region = 1; region <= 5; ++region) {
        std::vector<Cell> cells;
        const auto label = "第 " + std::to_string(region) + " 层";
        for (std::size_t i = 0; i < opening_cell_names.size(); ++i) {
            cells.push_back(b.numeric_cell("/policies/opening/regions/" + std::to_string(region) +
                "/" + opening_cell_names[i], label + table.headers[i]));
        }
        table.rows.emplace_back(label, std::move(cells));
    }
    b.page.matrices.push_back(std::move(table));

    // Observed in 53,336 complete opening maps. Column 1 is always the start.
    b.page.opening_map_columns = {
        "普通 + 印记/事件 + 普通 + 印记/事件 + 遗物",
        "印记/事件 + 普通 + 遗物/商店 + 普通/极难 + 印记/事件 + 遗物",
        "普通 + 事件/遗物 + 印记/事件 + 普通/极难 + 商店 + 首领",
        "印记/遗物 + 普通/极难 + 事件 + （普通/极难 + 普通） + 印记/遗物 + 商店",
        "普通 + 事件/遗物 + 普通/极难 + 遗物 + 普通/极难 + 商店 + 首领",
    };
}

void team_page(Builder& b, int subpage) {
    if (subpage == 2) {
        b.page.description = "验证全部候选，优先胜利且返秒最高的队伍。模拟失败时使用 Critic 推荐。";
        b.toggle("/policies/team/simulator/enabled", "启用模拟器验证");
        b.text("/policies/team/simulator/url", "模拟器 URL", default_simulator_url);
        b.note("角色练度", "模拟器满练度，帐号及黎明界加成取自客户端");
        return;
    }
    if (subpage == 1) {
        b.toggle("/model/use_gpu", "使用 GPU");
#ifdef _WIN32
        const std::string adapter_key = "/model/gpu_adapter";
        const auto selected = get(b.draft, adapter_key);
        std::vector<Choice> adapters{{"自动（高性能优先）", nullptr}};
        const auto detected = windows_gpu_adapters();
        for (const auto& adapter : detected) {
            auto label = adapter.name;
            if (std::ranges::count(detected, adapter.name, &WindowsGpuAdapter::name) > 1)
                label += " · " + std::to_string(adapter.index);
            adapters.push_back({std::move(label), adapter.id});
        }
        if (!selected.is_null() && std::ranges::none_of(adapters, [&](const auto& adapter) {
                return adapter.value == selected;
            }))
            adapters.push_back({"已选显卡不可用", selected});
        b.choice(adapter_key, "GPU 设备", std::move(adapters), selected);
#endif
        b.text("/model/directory", "模型目录", "models", PathKind::folder);
        b.number("/model/threads", "推理线程数", 4, 1, 128);
        b.number("/model/candidates", "候选队伍数", 64, 1, 128);
        constexpr std::string_view key = "/model/candidate_seed";
        b.register_edit(std::string(key), "采样种子", [key](Json& config, std::string_view text) {
            config[Pointer(std::string(key))] = integer<std::uint64_t>(
                text, 0, std::numeric_limits<std::uint64_t>::max());
        });
        b.fields().push_back({std::string(key), FieldKind::input, "采样种子", {},
            get(b.draft, std::string(key), 20261101).dump()});
        return;
    }
    b.toggle("/policies/team/enabled", "启用自动编队");
    b.toggle("/policies/team/ignore_npc", "忽略 NPC", false, "排除后至少剩 5 名角色时生效");
    const std::string count_key = "/policies/team/third_boss_party_count";
    const auto count = get(b.draft, count_key, 3);
    Field parties{count_key, FieldKind::button_group, "第三层首领队伍数", {}, {}};
    parties.toggle_exclusive = true;
    parties.toggle_minimum = 1;
    for (int value = 1; value <= 3; ++value) {
        const auto option = count_key + "/@" + std::to_string(value);
        const auto label = std::to_string(value);
        b.register_edit(option, label, [count_key, value](Json& config, std::string_view) {
            config[Pointer(count_key)] = value;
        });
        parties.toggle_options.push_back({option, label, count == value});
    }
    b.fields().push_back(std::move(parties));
    b.heading("适用战斗");
    Field scopes{"/policies/team/scopes", FieldKind::button_group, {}, {}, {}};
    for (const auto& [scope, label] : std::array{
             std::pair{"normal", "普通"}, std::pair{"ex", "极难"}, std::pair{"boss", "首领"}}) {
        const auto option = scopes.key + "/" + scope;
        scopes.toggle_options.push_back({option, label, b.toggle_edit(option, label, true)});
    }
    b.fields().push_back(std::move(scopes));

    b.heading("实战重试");
    b.number("/policies/team/battle_retry/max_retries", "重试次数", 4);
    const std::string action = "/policies/team/battle_retry/on_limit";
    const auto current = get(b.draft, action, "manual");
    Field actions{action, FieldKind::button_group, "达到上限", {}, {}};
    actions.toggle_exclusive = true;
    for (const auto& [value, label] : std::array{std::pair{"manual", "转人工"}, std::pair{"restart", "撤退重开"}}) {
        const auto option = action + "/@" + value;
        b.register_edit(option, label, [action, value](Json& config, std::string_view) {
            config[Pointer(action)] = value;
        });
        actions.toggle_options.push_back({option, label, current == value});
    }
    b.fields().push_back(std::move(actions));
}

void select_ticket_page(Builder& b, const Json& catalog, std::string_view query, int index) {
    b.toggle("/policies/select_ticket/enabled", "启用自选券自动选人");
    const auto rosters = catalog.contains("select_ticket_rosters") ? parse_select_ticket_rosters(catalog) :
        std::vector<SelectTicketRoster>{};
    if (rosters.empty()) {
        b.note("自选券", "游戏数据 JSON 中暂无自选券候选名单");
        return;
    }
    auto& picker = b.page.ticket_picker;
    for (const auto& roster : rosters) picker.labels.push_back(roster.label);
    picker.index = std::clamp(index, 0, static_cast<int>(rosters.size()) - 1);
    const auto& roster = rosters[static_cast<std::size_t>(picker.index)];
    picker.total = roster.unit_ids.size();
    const auto key = "/policies/select_ticket/ticket_priority_units/" + std::to_string(roster.ticket_id);
    const auto selected = ids_at(b.draft, key);
    const auto matches = official_unit_matches(catalog, query);
    const auto option_for = [&](Id id, int rank) {
        const auto option = key + "/@" + std::to_string(id);
        const auto label = catalog_name(catalog, "unit_data", "unit_id", "unit_name", id);
        if (!b.page.edits.contains(option)) {
            b.register_edit(option, label, [key, id](Json& config, std::string_view input) {
                auto items = ids_at(config, key);
                if (input == "true") {
                    if (std::ranges::find(items, id) == items.end()) items.push_back(id);
                } else {
                    std::erase(items, id);
                }
                config[Pointer(key)] = std::move(items);
            }, true, [key, id](Json& config, int delta) {
                auto items = ids_at(config, key);
                if (move_priority(items, std::ranges::find(items, id), delta))
                    config[Pointer(key)] = std::move(items);
            });
        }
        return RankedOption{option, label, rank, id};
    };
    for (std::size_t i = 0; i < selected.size(); ++i)
        picker.priority.push_back(option_for(selected[i], static_cast<int>(i) + 1));
    for (const auto id : roster.unit_ids) {
        const auto found = std::ranges::find(selected, id);
        auto option = option_for(id, found == selected.end() ? 0 : static_cast<int>(found - selected.begin()) + 1);
        if (query.empty() || option.label.find(query) != std::string::npos ||
            std::to_string(id).find(query) != std::string::npos || matches.contains(id))
            picker.candidates.push_back(std::move(option));
    }
}

void priority_queue(Builder& b, const std::string& policy, const std::string& field,
                    const std::string& title, const NamedIds& available) {
    b.page.description = "从左到右优先级递减。拖动手柄，或先点按钮再点目标位置。";
    const auto key = policy + "/" + field;
    const auto enabled = policy + "/enabled";
    Ids defaults;
    for (const auto& [id, label] : available) defaults.push_back(id);
    b.register_edit(enabled, title, [key, enabled, defaults](Json& config, std::string_view value) {
        config[Pointer(enabled)] = value == "true";
        if (value == "true" && !config.contains(Pointer(key))) config[Pointer(key)] = defaults;
    });
    b.fields().push_back({enabled, FieldKind::toggle, title, {}, {},
                         get(b.draft, enabled, false).get<bool>()});

    const auto selected = ids_at(b.draft, key, defaults);
    for (std::size_t i = 0; i < selected.size(); ++i) {
        const auto id = selected[i];
        const auto option = key + "/@" + std::to_string(id);
        const auto found = std::ranges::find(available, id, &NamedIds::value_type::first);
        const auto label = found == available.end() ? "#" + std::to_string(id) : found->second;
        b.page.edits.emplace(option, Edit{label, {}, [key, id, defaults](Json& config, int delta) {
            auto items = ids_at(config, key, defaults);
            if (move_priority(items, std::ranges::find(items, id), delta)) {
                prepare_edit(config, key);
                config[Pointer(key)] = std::move(items);
            }
        }});
        b.page.priority_options.push_back({option, label, static_cast<int>(i) + 1, 0});
    }
}

void select_page(Builder& b, const Json& catalog, int subpage, std::string_view query, int ticket_index) {
    b.page.description = "设置角色、印记和遗物的选择规则。";
    switch (subpage) {
    case 0: {
        b.page.description = "按当前角色、遗物和后续关卡，比较三个候选。权重填 0 可停用该目标。";
        b.toggle("/policies/unit_choice_targets/enabled", "启用角色选择");
        Table bosses{"首领权重", {"权重"}, {}};
        const std::string policy = "/policies/unit_choice_targets";
        const auto boss_targets = get(b.draft, policy + "/boss_targets", Json::object());
        for (const auto& [region, target] : boss_targets.items()) {
            const auto label = recruit_boss_label(std::stoi(region), target.at("parties").get<int>());
            bosses.rows.emplace_back(label, std::vector{b.numeric_cell(
                policy + "/boss_targets/" + region + "/weight", label)});
        }
        if (!bosses.rows.empty()) b.page.matrices.push_back(std::move(bosses));

        Table ex{"EX 关卡权重", {"权重"}, {}};
        for (const auto& target : parse_recruit_ex_targets(catalog)) {
            ex.rows.emplace_back(target.name, std::vector{b.numeric_cell(
                policy + "/ex_weights/" + std::to_string(target.quest_ids.front()), target.name)});
        }
        if (!ex.rows.empty()) b.page.matrices.push_back(std::move(ex));
        break;
    }
    case 1:
        b.page.description.clear();
        select_ticket_page(b, catalog, query, ticket_index);
        break;
    case 2:
        priority_queue(b, "/policies/ticket_choice", "talent_priority", "启用印记选择",
            {{1, "火"}, {2, "水"}, {3, "风"}, {4, "光"}, {5, "暗"}});
        break;
    case 3:
        priority_queue(b, "/policies/relic_choice", "mark_priority", "启用遗物选择",
            names(catalog, "labyrinth_relic_mark_series", "relic_mark_id", "relic_mark_title"));
        b.note("星级优先", "同星比较下方顺序");
        break;
    }
}

struct EventGroup {
    int background;
    std::string label;
    std::vector<const Json*> events;
};

int fallback_event_background(int id) {
    if (id >= 1101 && id <= 1104) return 2;
    switch (id) {
    case 1105: case 1109: case 1113: case 1117: return 4;
    case 1107: return 7;
    case 1108: return 8;
    case 1110: return 3;
    case 1111: return 10;
    case 1112: return 11;
    case 1314: case 2314: case 3314: return 5;
    case 1315: case 2315: case 3315: return 9;
    case 1316: case 2316: case 3316: return 6;
    default: return -id;
    }
}

std::vector<EventGroup> event_groups(const Json& catalog) {
    std::vector<EventGroup> groups{
        {2, "公会管理协会", {}}, {4, "斗技场", {}}, {7, "不可思议的石板", {}},
        {8, "地下城迷路", {}}, {3, "石头剪刀布", {}}, {10, "箱子里的声音", {}},
        {11, "会场魔物", {}}, {5, "钓鱼", {}}, {9, "郊游料理", {}}, {6, "老虎机", {}}
    };
    for (const auto& row : catalog_rows(catalog, "labyrinth_event")) {
        const auto id = row.value("event_id", 0);
        const auto bg = row.value("choice_bg_id", fallback_event_background(id));
        auto group = std::ranges::find(groups, bg, &EventGroup::background);
        if (group == groups.end()) {
            groups.push_back({bg, description(row, "event_text"), {&row}});
        } else group->events.push_back(&row);
    }
    std::erase_if(groups, [](const auto& group) { return group.events.empty(); });
    return groups;
}

void event_page(Builder& b, const Json& catalog, int subpage) {
    b.page.description = "点选一个选项，再点一次取消。未选时手动处理。";
    b.toggle("/policies/event/enabled", "启用事件决策");
    const auto groups = event_groups(catalog);
    if (groups.empty()) {
        b.note("事件数据", "游戏数据 JSON 中暂无事件记录");
        return;
    }
    const auto& group = groups[static_cast<std::size_t>(
        std::clamp(subpage, 0, static_cast<int>(groups.size()) - 1))];
    const auto& options = catalog_rows(catalog, "labyrinth_event_choice");
    for (const auto* event : group.events) {
        const auto id = event->at("event_id").get<int>();
        const auto id_text = std::to_string(id);
        const auto key = "/policies/event/fixed_choices/" + id_text;
        auto title = description(*event, "event_text");
        if (title.empty()) title = group.label.empty() ? "事件" : group.label;
        b.fields().push_back({{}, FieldKind::event_heading, title, {}, id_text});

        const auto selected = get(b.draft, key);
        std::array<bool, 4> available{};
        for (int slot_number = 1; slot_number <= 3; ++slot_number) {
            const auto number = std::to_string(slot_number);
            const auto option_id = event->value("choice_id_" + number, 0);
            if (!option_id) continue;
            const auto option = std::ranges::find_if(options, [option_id](const auto& row) {
                return row.value("choice_id", 0) == option_id;
            });
            if (option == options.end()) continue;
            auto reward = description(*option, "description");
            auto name = description(*option, "unit_comment");
            if (reward == name) reward.clear();
            if (option->value("condition_type", 0) == 1) {
                const auto gold = option->value("condition_value", 0);
                if (gold > 0) {
                    if (!reward.empty()) reward += "  ·  ";
                    reward += "至少 " + std::to_string(gold) + " 金币";
                }
            }
            if (name.empty()) name = std::exchange(reward, {});
            if (name.empty()) name = "选项 " + number;
            available[slot_number] = true;
            b.fields().push_back({key, FieldKind::event_option, std::move(name), std::move(reward),
                                  number, selected == slot_number});
        }
        b.register_edit(key, title, [id_text, available](Json& config, std::string_view text) {
            const auto choice = integer<int>(text, 1, static_cast<int>(available.size()) - 1);
            if (!available[choice]) throw std::runtime_error("该事件没有此选项");
            auto& configured = config[Pointer("/policies/event/fixed_choices")];
            if (configured.contains(id_text) && configured.at(id_text) == choice) configured.erase(id_text);
            else configured[id_text] = choice;
        }, true);
    }
}

void probability_cell(Builder& b, std::vector<Cell>& cells, const std::string& key,
                      const std::string& title) {
    b.register_edit(key, title, [key](Json& config, std::string_view text) {
        auto parsed = Json::parse(text);
        if (!parsed.is_number() || parsed.get<double>() < 0 || parsed.get<double>() > 1)
            throw std::runtime_error("请输入 0 至 1 的概率");
        config[Pointer(key)] = parsed;
    });
    cells.push_back({key, get(b.draft, key, 0).dump()});
}

void shop_page(Builder& b, const Json& catalog, int subpage) {
    b.page.description = "按区域设置购买规则和刷新预测参数。";
    constexpr std::string_view prefix = "/policies/shop/model";
    if (subpage == 0) {
        b.toggle("/policies/shop/enabled", "启用商店购买");
        Table table{"区域购买规则", {"自动购买", "保留金币"}, {}};
        for (int region = 2; region <= 5; ++region) {
            const auto key = "/policies/shop/regions/" + std::to_string(region);
            const auto label = "第 " + std::to_string(region) + " 层";
            table.rows.emplace_back(label, std::vector{
                b.toggle_cell(key + "/purchase", label + "购买开关"),
                b.numeric_cell(key + "/reserve_gold", label + "保留金币")
            });
        }
        b.page.matrices.push_back(std::move(table));
        return;
    }
    if (subpage == 1) {
        const auto path = std::string(prefix);
        b.heading("基准价格");
        b.number(path + "/base_prices/random_imprint", "随机印记", 1200, 1);
        b.number(path + "/base_prices/choice_imprint", "自选印记", 2800, 1);
        const std::array<int, 3> prices{1800, 2800, 3800};
        for (int i = 0; i < 3; ++i) b.number(path + "/base_prices/relic/" + std::to_string(i),
            std::to_string(i + 1) + " 星遗物", prices[static_cast<std::size_t>(i)], 1);
        b.heading("刷新与收益");
        b.number(path + "/refresh_fees/0", "首次刷新费", 300);
        b.number(path + "/refresh_fees/1", "第二次刷新费", 600);
        b.number(path + "/scores/imprint", "印记估值", 100, 1);
        b.number(path + "/scores/relic", "遗物估值", 200, 1);
        return;
    }

    const auto key = std::string(prefix) + "/regions/" + std::to_string(subpage);
    const auto region = get(b.draft, key);
    const int mode = region.is_null() ? 0 : region.contains("fixed_relic_ids") ? 2 : 1;
    b.choice(key + "/@mode", "遗物预测类型", {{"未配置", 0}, {"随机遗物", 1}, {"固定遗物", 2}}, mode, {},
        [key, mode](Json& config, const Json& value) {
            const auto next_mode = value.get<int>();
            if (next_mode == mode) return;
            const auto prior = get(config, key, Json::object());
            if (!next_mode) {
                const Pointer list("/policies/shop/model/regions");
                if (config.contains(list)) config[list].erase(key.substr(key.find_last_of('/') + 1));
                return;
            }
            Json forecast = {{"random_slots", prior.value("random_slots", 2)},
                {"choice_slots", prior.value("choice_slots", 1)}};
            if (next_mode == 2) forecast["fixed_relic_ids"] = Json::array();
            else {
                forecast["relic_slots"] = 3;
                forecast["relic_star_probabilities"] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
            }
            config[Pointer(key)] = std::move(forecast);
        }, true);
    if (!mode) return;
    b.number(key + "/random_slots", "随机印记槽位", 2, 0, shop_imprint_slots);
    b.number(key + "/choice_slots", "自选印记槽位", 1, 0, shop_imprint_slots, "两种印记槽位合计为 3");
    if (mode == 2) {
        b.ordered_ids(key + "/fixed_relic_ids", "固定遗物",
            names(catalog, "labyrinth_relic", "relic_id", "name"));
        return;
    }
    b.number(key + "/relic_slots", "随机遗物槽位", 3, 0, shop_max_relic_slots);
    Table table{"星级概率", {"1 星", "2 星", "3 星"}, {}};
    for (int r = 0; r < 3; ++r) {
        std::vector<Cell> cells;
        const auto label = "刷新 " + std::to_string(r) + " 次";
        for (int s = 0; s < 3; ++s) probability_cell(b, cells, key + "/relic_star_probabilities/" +
            std::to_string(r) + "/" + std::to_string(s), label + " · " + std::to_string(s + 1) + " 星");
        table.rows.emplace_back(label, std::move(cells));
    }
    b.page.matrices.push_back(std::move(table));
}
} // namespace

const std::vector<std::string>& section_names() {
    static const std::vector<std::string> names{
        "服务", "刷分", "开局与寻路", "编队", "选择策略", "事件", "商店", "原始配置"
    };
    return names;
}

std::vector<std::string> section_subpages(const Json& catalog, int section) {
    switch (section) {
    case 1: return {"分数目标", "清票"};
    case 2: return {"区域与寻路", "开局选人"};
    case 3: return {"自动编队", "编队模型", "模拟器验证"};
    case 4: return {"角色选择", "自选券", "印记选择", "遗物选择"};
    case 5: {
        std::vector<std::string> labels;
        for (const auto& group : event_groups(catalog)) labels.push_back(group.label);
        return labels;
    }
    case 6: return {"区域购买", "预测基础", "第 2 层预测", "第 3 层预测", "第 4 层预测", "第 5 层预测"};
    default: return {};
    }
}

std::string tidy(std::string text) {
    std::size_t offset = 0;
    while ((offset = text.find("\\n", offset)) != std::string::npos) text.replace(offset, 2, " ");
    std::ranges::replace(text, '\n', ' ');
    return text;
}

std::string catalog_name(const Json& catalog, std::string_view table, std::string_view key,
                         std::string_view name, int id) {
    if (table == "unit_data" && key == "unit_id" && name == "unit_name") {
        auto nickname = unit_nickname(catalog, id);
        if (!nickname.empty()) return nickname;
    }
    for (const auto& row : catalog_rows(catalog, table)) {
        if (row.value(key, 0) == id) {
            const auto label = description(row, name);
            if (!label.empty()) return label;
        }
    }
    return "#" + std::to_string(id);
}

SettingsPage make_settings_page(const Json& draft, const Json& catalog, int section, int subpage,
                                std::string_view query, int ticket_index) {
    Builder builder{draft, {}};
    builder.page.title = section_names().at(static_cast<std::size_t>(section));
    builder.page.subpages = section_subpages(catalog, section);
    switch (section) {
    case 0: server_page(builder); break;
    case 1:
        if (subpage == 1) ticket_clear_page(builder, catalog);
        else score_page(builder, catalog);
        break;
    case 2: opening_page(builder, catalog, subpage); break;
    case 3: team_page(builder, subpage); break;
    case 4: select_page(builder, catalog, subpage, query, ticket_index); break;
    case 5: event_page(builder, catalog, subpage); break;
    case 6: shop_page(builder, catalog, subpage); break;
    case raw_section:
        builder.page.description = "编辑同一份全局配置。使用服务端规则校验后保存。";
        break;
    }
    return std::move(builder.page);
}

} // namespace labyrinth::gui
