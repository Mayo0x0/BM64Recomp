#ifndef __LEVEL_EDITOR_H__
#define __LEVEL_EDITOR_H__

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "level_layout.h"

// Custom battle levels: files in the levels folder, the one played on each stage, the RDRAM data the game side
// (patches/level.c) reads, and the level editor's session (a battle on the level's stage, src/ui/ui_level_editor.cpp).
namespace levels {
    struct Level {
        std::string name = "New level";
        int stage = 0;
        // Rules, -1 keeps the game's: minutes, Sudden Death, ghosts, start bombs, start fire, random soft blocks.
        std::array<int, 6> rules{ -1, -1, -1, -1, -1, -1 };
        std::array<uint32_t, 8> starts; // packed tiles (LEVEL_PACK), LEVEL_NONE = the stage's own
        std::map<uint32_t, int> tiles;  // packed tile (value 0) -> TILE_* kind
        std::map<uint32_t, int> items;  // packed tile (value 0) -> ITEM_*
        // Changed columns of the stage's shape: packed tile (layer 0, value 0) -> the SHAPE_* kind of every layer.
        std::map<uint32_t, std::vector<uint8_t>> shape;
        Level() { starts.fill(LEVEL_NONE); }
    };

    struct LevelFile {
        std::string file;  // name in the levels folder
        std::string name;
        int stage;
        bool active;       // played on its stage
    };

    extern const char* const stage_names[LEVEL_STAGES];

    std::filesystem::path folder();
    std::vector<LevelFile> list();
    bool load(const std::string& file, Level& level);
    bool save(const std::string& file, const Level& level);
    std::string new_file_name(const std::string& name);
    bool remove(const std::string& file);
    // The level played on its stage (one per stage), or none.
    void set_active(const std::string& file, bool active);

    // RDRAM data: the active levels, for netplay's host to send; a client (and the host) plays the session's.
    std::vector<uint8_t> session_data();
    void set_session_data(const uint8_t* data, size_t size);

    // Called by the game at every controller poll (recomp_level_frame): copies new level data into RDRAM.
    void game_frame(uint8_t* rdram);
    // Called by the game once the grid has a level's shape (recomp_level_shape): rebuilds the stage model's display
    // lists to match it, or gives the stage its own back (src/game/level_shape.cpp).
    void build_shape(uint8_t* rdram);
    void shape_frame(uint8_t* rdram); // at every controller poll: builds again when the stage model moved

    // The editor session: started with BM64_LEVEL_EDIT=<file> (restart.h), the level is edited on its stage.
    bool editor_session();
    void start_editor(const std::string& file); // starts the program again into the editor
    void leave_editor();                         // starts the program again into the game

    namespace editor {
        // Raise / Lower move a column's top floor a layer, Ramp (value: SHAPE_RAMP_*) makes the top a ramp up to the
        // next layer, Flat makes it a floor again, Hole takes the whole column away.
        enum class Tool { Free, Random, Soft, Hard, Start, Item, Erase, Raise, Lower, Ramp, Flat, Hole };
        bool shape_tool(Tool tool);
        struct Status {
            bool ready = false;       // the stage is set up
            bool has_cursor = false;
            uint32_t cursor = LEVEL_NONE;
            std::string tile_text;    // what the tile under the cursor is
            bool dirty = false;       // changed since the last save
        };
        struct Label {
            std::string text;
            float x, y;              // window pixels
            uint32_t color;
        };

        Level& level();
        const std::string& file();
        Status status();
        // Mouse at window pixels (window width x height): the tile under it becomes the cursor.
        void hover(float x, float y, float width, float height);
        void leave_view();
        // Applies a tool at the cursor (stroke: one undo step for a drag).
        void begin_stroke();
        void apply(Tool tool, int value, bool erase);
        void end_stroke();
        void set_symmetry(int symmetry); // bits: 1 mirror x, 2 mirror z
        int symmetry();
        void undo();
        void redo();
        bool can_undo();
        bool can_redo();
        void set_rule(int rule, int value);
        void set_name(const std::string& name);
        bool save();
        // Start numbers and items as text over the view.
        std::vector<Label> labels(float width, float height);
    }
}

#endif
