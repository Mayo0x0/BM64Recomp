// The level editor's pages (include/level_editor.h): the level list in the launcher (assets/level_editor.rml) and
// the editor HUD over the stage in an editor session (assets/level_editor_hud.rml).
#include <cmath>
#include "recomp_ui.h"
#include "level_editor.h"
#include "netplay.h"
#include "zelda_config.h"
#include "zelda_support.h"
#include "RmlUi/Core.h"

#include <chrono>

namespace {

// Level list.
struct LevelRow {
    std::string file;
    std::string name;
    std::string stage;
    bool active = false;
};

struct ListModel {
    std::vector<LevelRow> rows;
    std::vector<std::string> stages;
    std::string new_name = "Mein Level";
    int new_stage = 0;
    std::string message;
} list_model;

Rml::DataModelHandle list_handle;
recompui::ContextId list_context;

void refresh_list() {
    list_model.rows.clear();
    for (const levels::LevelFile& file : levels::list()) {
        list_model.rows.push_back({ file.file, file.name, levels::stage_names[file.stage], file.active });
    }
    if (list_handle) {
        list_handle.DirtyVariable("lv_rows");
        list_handle.DirtyVariable("lv_message");
    }
}

std::string row_file(const Rml::VariantList& inputs) {
    if (inputs.empty()) return "";
    size_t index = inputs.at(0).Get<size_t>();
    return index < list_model.rows.size() ? list_model.rows[index].file : "";
}

// Editor HUD.
struct LabelView {
    std::string text;
    std::string left;
    std::string top;
    std::string color;
};

struct RuleView {
    std::string name;
    std::string value;
};

struct HudModel {
    int tool = (int)levels::editor::Tool::Soft;
    int start_slot = 0;
    int item = ITEM_FIRE;
    int ramp = SHAPE_RAMP_NZ;
    int symmetry = 0;
    std::string name;
    std::string status;
    std::string message;
    bool can_undo = false;
    bool can_redo = false;
    bool dirty = false;
    bool ready = false;
    bool panel_hidden = false; // Tab or the panel's button
    std::vector<LabelView> labels;
    std::vector<RuleView> rules;
} hud;

Rml::DataModelHandle hud_handle;
recompui::ContextId hud_context;
bool painting = false;
bool erasing = false;
bool panning = false; // the middle mouse button moves a zoomed view
float pan_last_x = 0.0f, pan_last_y = 0.0f;
float mouse_x = 0.0f, mouse_y = 0.0f; // the last position over the view (the wheel zooms around it)

// Rules: the steps the - and + buttons go through; -1 is the game's own.
struct RuleSpec {
    const char* name;
    int min, max;
};
const RuleSpec rule_specs[6] = {
    { "Zeit (Minuten)", 1, 5 }, { "Sudden Death", 0, 1 }, { "Geister", 0, 1 },
    { "Bomben am Start", 1, 8 }, { "Feuer am Start", 1, 8 }, { "Zufallsblöcke", 0, 60 },
};

std::string rule_text(int rule, int value) {
    if (value < 0) return "Standard";
    if (rule == 1 || rule == 2) return value ? "An" : "Aus";
    return std::to_string(value);
}

void refresh_rules() {
    hud.rules.clear();
    const levels::Level& level = levels::editor::level();
    for (int i = 0; i < 6; i++) hud.rules.push_back({ rule_specs[i].name, rule_text(i, level.rules[i]) });
    if (hud_handle) hud_handle.DirtyVariable("hud_rules");
}

void step_rule(int rule, int direction) {
    if (rule < 0 || rule >= 6) return;
    const RuleSpec& spec = rule_specs[rule];
    int value = levels::editor::level().rules[rule];
    if (direction > 0) value = value < 0 ? spec.min : (value >= spec.max ? -1 : value + (rule == 5 ? 5 : 1));
    else value = value < 0 ? spec.max : (value <= spec.min ? -1 : value - (rule == 5 ? 5 : 1));
    if (value > spec.max) value = spec.max;
    levels::editor::set_rule(rule, value);
    refresh_rules();
}

std::string color_text(uint32_t rgba) {
    char text[16];
    snprintf(text, sizeof(text), "#%06X", rgba >> 8);
    return text;
}

int tool_value() {
    switch ((levels::editor::Tool)hud.tool) {
    case levels::editor::Tool::Start: return hud.start_slot;
    case levels::editor::Tool::Item: return hud.item;
    case levels::editor::Tool::Ramp: return hud.ramp;
    default: return 0;
    }
}

void dirty_hud() {
    for (const char* name : { "hud_tool", "hud_start_slot", "hud_item", "hud_ramp", "hud_symmetry", "hud_status", "hud_message",
                              "hud_can_undo", "hud_can_redo", "hud_dirty", "hud_ready", "hud_labels" }) {
        hud_handle.DirtyVariable(name);
    }
}

void quit_editor() {
    if (!levels::editor::status().dirty) {
        levels::leave_editor();
        return;
    }
    recompui::open_choice_prompt("Ungespeicherte Änderungen", "Das Level hat Änderungen, die noch nicht gespeichert sind.",
        "Verwerfen und beenden", "Zurück", []() { levels::leave_editor(); }, []() {},
        recompui::ButtonVariant::Error, recompui::ButtonVariant::Tertiary, true, "");
}

void save_level() {
    levels::editor::set_name(hud.name);
    hud.message = levels::editor::save() ? "Gespeichert: " + levels::editor::file() : "Speichern fehlgeschlagen.";
}

} // namespace

recompui::ContextId recompui::get_level_list_context_id() {
    return list_context;
}

recompui::ContextId recompui::get_level_hud_context_id() {
    return hud_context;
}

void recompui::open_level_list() {
    refresh_list();
    recompui::hide_all_contexts();
    recompui::show_context(list_context, "");
}

// Tab or the panel's button: the panel goes and the stage gets the whole window, or the other way round.
void toggle_panel() {
    hud.panel_hidden = !hud.panel_hidden;
    if (hud_handle) hud_handle.DirtyVariable("hud_panel_hidden");
}

// Every UI frame: the HUD follows the editor (cursor text, labels over the stage).
void recompui::update_level_editor() {
    if (!hud_handle || !levels::editor_session()) return;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(33)) return;
    last = now;
    levels::editor::Status status = levels::editor::status();
    hud.ready = status.ready;
    hud.status = status.ready ? (status.has_cursor ? status.tile_text : "Zeige mit der Maus auf ein Feld.") : "Die Stage lädt...";
    hud.dirty = status.dirty;
    hud.can_undo = levels::editor::can_undo();
    hud.can_redo = levels::editor::can_redo();
    hud.labels.clear();
    Rml::ElementDocument* document = hud_context.get_document();
    if (document != nullptr && document->GetContext() != nullptr) {
        Rml::Vector2i size = document->GetContext()->GetDimensions();
        // The stage is drawn beside the panel (patches/level.c level_projection), the whole window without it.
        Rml::Element* panel = document->GetElementById("panel");
        float covered = hud.panel_hidden || panel == nullptr ? 0.0f : panel->GetOffsetWidth();
        levels::editor::set_panel(covered, (float)size.x, (float)size.y);
        for (const levels::editor::Label& label : levels::editor::labels((float)size.x, (float)size.y)) {
            hud.labels.push_back({ label.text, std::to_string((int)label.x) + "px", std::to_string((int)label.y) + "px",
                                   color_text(label.color) });
        }
    }
    dirty_hud();
}

class LevelListMenu : public recompui::MenuController {
public:
    void load_document() override {
        list_context = recompui::create_context(zelda64::get_asset_path("level_editor.rml"));
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "levels_back", [](const std::string&, Rml::Event&) {
            recompui::hide_all_contexts();
            recompui::show_context(recompui::get_launcher_context_id(), "");
        });
        recompui::register_event(listener, "levels_create", [](const std::string&, Rml::Event&) {
            levels::Level level;
            level.name = list_model.new_name.empty() ? "Mein Level" : list_model.new_name;
            level.stage = std::clamp(list_model.new_stage, 0, LEVEL_STAGES - 1);
            std::string file = levels::new_file_name(level.name);
            if (levels::save(file, level)) {
                levels::start_editor(file);
            } else {
                list_model.message = "Konnte " + file + " nicht anlegen.";
                refresh_list();
            }
        });
    }
    void make_bindings(Rml::Context* context) override {
        Rml::DataModelConstructor constructor = context->CreateDataModel("levels_model");
        if (auto row = constructor.RegisterStruct<LevelRow>()) {
            row.RegisterMember("file", &LevelRow::file);
            row.RegisterMember("name", &LevelRow::name);
            row.RegisterMember("stage", &LevelRow::stage);
            row.RegisterMember("active", &LevelRow::active);
        }
        constructor.RegisterArray<std::vector<LevelRow>>();
        constructor.RegisterArray<std::vector<std::string>>();
        for (int i = 0; i < LEVEL_STAGES; i++) list_model.stages.push_back(levels::stage_names[i]);
        constructor.Bind("lv_rows", &list_model.rows);
        constructor.Bind("lv_stages", &list_model.stages);
        constructor.Bind("lv_new_name", &list_model.new_name);
        constructor.Bind("lv_new_stage", &list_model.new_stage);
        constructor.Bind("lv_message", &list_model.message);
        constructor.BindEventCallback("lv_edit", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& inputs) {
            std::string file = row_file(inputs);
            if (!file.empty()) levels::start_editor(file);
        });
        constructor.BindEventCallback("lv_toggle", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& inputs) {
            size_t index = inputs.empty() ? 0 : inputs.at(0).Get<size_t>();
            if (index >= list_model.rows.size()) return;
            if (netplay::in_game()) {
                list_model.message = "Während einer Netplay-Partie lassen sich die Level nicht ändern.";
            } else {
                levels::set_active(list_model.rows[index].file, !list_model.rows[index].active);
                list_model.message = "";
            }
            refresh_list();
        });
        constructor.BindEventCallback("lv_delete", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& inputs) {
            std::string file = row_file(inputs);
            if (file.empty()) return;
            recompui::open_choice_prompt("Level löschen?", "Die Datei " + file + " wird gelöscht.", "Löschen", "Abbrechen",
                [file]() { levels::remove(file); refresh_list(); }, []() {},
                recompui::ButtonVariant::Error, recompui::ButtonVariant::Tertiary, true, "");
        });
        list_handle = constructor.GetModelHandle();
    }
};

class LevelHudMenu : public recompui::MenuController {
public:
    void load_document() override {
        hud_context = recompui::create_context(zelda64::get_asset_path("level_editor_hud.rml"));
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        // The stage view: the mouse picks a tile; left paints the tool, right restores the stage's own.
        recompui::register_event(listener, "hud_view_move", [](const std::string&, Rml::Event& event) {
            Rml::Vector2i size = event.GetTargetElement()->GetContext()->GetDimensions();
            float mx = event.GetParameter<float>("mouse_x", 0), my = event.GetParameter<float>("mouse_y", 0);
            mouse_x = mx;
            mouse_y = my;
            if (panning) {
                levels::editor::pan_view(mx - pan_last_x, my - pan_last_y, (float)size.y);
                pan_last_x = mx;
                pan_last_y = my;
            }
            levels::editor::hover(mx, my, (float)size.x, (float)size.y);
            if (painting) levels::editor::apply((levels::editor::Tool)hud.tool, tool_value(), erasing);
        });
        recompui::register_event(listener, "hud_view_down", [](const std::string&, Rml::Event& event) {
            int button = event.GetParameter<int>("button", 0);
            if (button == 2) {
                panning = true;
                pan_last_x = event.GetParameter<float>("mouse_x", 0);
                pan_last_y = event.GetParameter<float>("mouse_y", 0);
                return;
            }
            if (button != 0 && button != 1) return;
            event.GetCurrentElement()->Focus(); // the view takes the keys (Ctrl+Z, Ctrl+Y, Ctrl+S)
            painting = true;
            erasing = button == 1;
            levels::editor::begin_stroke();
            levels::editor::apply((levels::editor::Tool)hud.tool, tool_value(), erasing);
            // A picked look is painted next.
            levels::editor::Status status = levels::editor::status();
            if ((levels::editor::Tool)hud.tool == levels::editor::Tool::LookPick && !erasing && status.has_cursor) {
                hud.tool = (int)levels::editor::Tool::LookPaint;
                hud.message = "Optik von Feld " + std::to_string(LEVEL_X(status.cursor)) + "/" +
                              std::to_string(LEVEL_Z(status.cursor)) + " aufgenommen. Male sie auf Felder.";
                hud_handle.DirtyVariable("hud_tool");
                hud_handle.DirtyVariable("hud_message");
            }
        });
        recompui::register_event(listener, "hud_view_up", [](const std::string&, Rml::Event&) {
            if (painting) levels::editor::end_stroke();
            painting = false;
            panning = false;
        });
        // The mouse wheel zooms in and out around the point under the mouse.
        recompui::register_event(listener, "hud_view_scroll", [](const std::string&, Rml::Event& event) {
            Rml::Vector2i size = event.GetTargetElement()->GetContext()->GetDimensions();
            float delta = event.GetParameter<float>("wheel_delta_y", 0);
            if (delta == 0.0f) return;
            levels::editor::zoom_view(std::pow(1.2f, -delta), mouse_x, mouse_y, (float)size.x, (float)size.y);
            levels::editor::hover(mouse_x, mouse_y, (float)size.x, (float)size.y);
            event.StopPropagation();
        });
        recompui::register_event(listener, "hud_view_out", [](const std::string&, Rml::Event& event) {
            if (event.GetTargetElement() != event.GetCurrentElement()) return;
            if (painting) levels::editor::end_stroke();
            painting = false;
            panning = false;
            levels::editor::leave_view();
        });
        recompui::register_event(listener, "hud_key", [](const std::string&, Rml::Event& event) {
            auto key = (Rml::Input::KeyIdentifier)event.GetParameter<int>("key_identifier", 0);
            bool ctrl = event.GetParameter<int>("ctrl_key", 0) != 0;
            if (ctrl && key == Rml::Input::KI_Z) levels::editor::undo();
            else if (ctrl && key == Rml::Input::KI_Y) levels::editor::redo();
            else if (ctrl && key == Rml::Input::KI_S) save_level();
            else if (!ctrl && key == Rml::Input::KI_TAB) toggle_panel();
            else if (!ctrl && (key == Rml::Input::KI_0 || key == Rml::Input::KI_NUMPAD0 || key == Rml::Input::KI_HOME))
                levels::editor::reset_view();
            else return;
            event.StopPropagation(); // the view and the body both listen
            refresh_rules();
        });
        recompui::register_event(listener, "hud_save", [](const std::string&, Rml::Event&) { save_level(); });
        recompui::register_event(listener, "hud_toggle_panel", [](const std::string&, Rml::Event&) { toggle_panel(); });
        recompui::register_event(listener, "hud_save_quit", [](const std::string&, Rml::Event&) {
            save_level();
            if (!levels::editor::status().dirty) levels::leave_editor();
        });
        recompui::register_event(listener, "hud_quit", [](const std::string&, Rml::Event&) { quit_editor(); });
        recompui::register_event(listener, "hud_undo", [](const std::string&, Rml::Event&) {
            levels::editor::undo();
            refresh_rules();
        });
        recompui::register_event(listener, "hud_redo", [](const std::string&, Rml::Event&) {
            levels::editor::redo();
            refresh_rules();
        });
    }
    void make_bindings(Rml::Context* context) override {
        Rml::DataModelConstructor constructor = context->CreateDataModel("level_hud_model");
        if (auto label = constructor.RegisterStruct<LabelView>()) {
            label.RegisterMember("text", &LabelView::text);
            label.RegisterMember("left", &LabelView::left);
            label.RegisterMember("top", &LabelView::top);
            label.RegisterMember("color", &LabelView::color);
        }
        if (auto rule = constructor.RegisterStruct<RuleView>()) {
            rule.RegisterMember("name", &RuleView::name);
            rule.RegisterMember("value", &RuleView::value);
        }
        constructor.RegisterArray<std::vector<LabelView>>();
        constructor.RegisterArray<std::vector<RuleView>>();
        constructor.Bind("hud_tool", &hud.tool);
        constructor.Bind("hud_start_slot", &hud.start_slot);
        constructor.Bind("hud_item", &hud.item);
        constructor.Bind("hud_ramp", &hud.ramp);
        constructor.Bind("hud_symmetry", &hud.symmetry);
        constructor.Bind("hud_name", &hud.name);
        constructor.Bind("hud_status", &hud.status);
        constructor.Bind("hud_message", &hud.message);
        constructor.Bind("hud_can_undo", &hud.can_undo);
        constructor.Bind("hud_can_redo", &hud.can_redo);
        constructor.Bind("hud_dirty", &hud.dirty);
        constructor.Bind("hud_ready", &hud.ready);
        constructor.Bind("hud_panel_hidden", &hud.panel_hidden);
        constructor.Bind("hud_labels", &hud.labels);
        constructor.Bind("hud_rules", &hud.rules);
        constructor.BindEventCallback("hud_set_tool", [](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList& inputs) {
            hud.tool = inputs.empty() ? 0 : inputs.at(0).Get<int>();
            if (inputs.size() > 1) {
                if ((levels::editor::Tool)hud.tool == levels::editor::Tool::Start) hud.start_slot = inputs.at(1).Get<int>();
                if ((levels::editor::Tool)hud.tool == levels::editor::Tool::Item) hud.item = inputs.at(1).Get<int>();
                if ((levels::editor::Tool)hud.tool == levels::editor::Tool::Ramp) hud.ramp = inputs.at(1).Get<int>();
            }
            handle.DirtyVariable("hud_tool");
            handle.DirtyVariable("hud_start_slot");
            handle.DirtyVariable("hud_item");
            handle.DirtyVariable("hud_ramp");
        });
        constructor.BindEventCallback("hud_set_symmetry", [](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList& inputs) {
            hud.symmetry = inputs.empty() ? 0 : inputs.at(0).Get<int>();
            levels::editor::set_symmetry(hud.symmetry);
            handle.DirtyVariable("hud_symmetry");
        });
        constructor.BindEventCallback("hud_rule", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& inputs) {
            if (inputs.size() < 2) return;
            step_rule((int)inputs.at(0).Get<size_t>(), inputs.at(1).Get<int>());
        });
        constructor.BindEventCallback("hud_name_changed", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            levels::editor::set_name(hud.name);
        });
        hud_handle = constructor.GetModelHandle();
        if (levels::editor_session()) {
            hud.name = levels::editor::level().name;
            refresh_rules();
        }
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_level_list_menu() {
    return std::make_unique<LevelListMenu>();
}

std::unique_ptr<recompui::MenuController> recompui::create_level_hud_menu() {
    return std::make_unique<LevelHudMenu>();
}
