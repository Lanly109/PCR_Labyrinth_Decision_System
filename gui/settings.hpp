#pragma once

#include "labyrinth.hpp"

#include <functional>
#include <map>
#include <string_view>

namespace labyrinth::gui {

enum class FieldKind { heading, toggle, input, choice, note, event_heading, event_option, button_group };
enum class PathKind { none, file, folder };

struct ToggleOption {
    std::string key, label;
    bool checked;
    Id unit_id = 0;
};

struct Field {
    std::string key;
    FieldKind kind;
    std::string title;
    std::string hint;
    std::string value;
    bool checked = false;
    std::vector<std::string> options;
    int selected = 0;
    bool numeric = false;
    PathKind path_kind = PathKind::none;
    std::string order_group;
    int order_index = 0;
    int order_count = 0;
    std::vector<ToggleOption> toggle_options;
    int toggle_minimum = 0;
    bool toggle_exclusive = false;
};

struct Cell {
    std::string key;
    std::string value;
    bool is_boolean = false;
    bool checked = false;
};

struct Table {
    std::string title;
    std::vector<std::string> headers;
    std::vector<std::pair<std::string, std::vector<Cell>>> rows;
};

struct Edit {
    std::string title;
    std::function<void(Json&, std::string_view)> apply;
    std::function<void(Json&, int)> move;
    bool redraw = false;
};

struct RankedOption {
    std::string key, label;
    int rank;
    Id unit_id;
};

struct TicketPicker {
    std::vector<std::string> labels;
    int index = 0;
    std::size_t total = 0;
    std::vector<RankedOption> candidates, priority;
};

struct SettingsPage {
    std::string title;
    std::string description;
    std::vector<std::string> subpages;
    std::vector<Field> fields;
    std::vector<Table> matrices;
    std::vector<std::string> opening_map_columns;
    std::map<std::string, Edit> edits;
    TicketPicker ticket_picker;
    std::vector<RankedOption> priority_options;
};

constexpr int raw_section = 7;
const std::vector<std::string>& section_names();
std::vector<std::string> section_subpages(const Json& catalog, int section);
SettingsPage make_settings_page(const Json& draft, const Json& catalog, int section, int subpage,
                                std::string_view query, int ticket_index = 0);
std::string tidy(std::string text);
std::string catalog_name(const Json& catalog, std::string_view table, std::string_view key,
                         std::string_view name, int id);
} // namespace labyrinth::gui
