#ifndef __RESTART_H__
#define __RESTART_H__

#include <string>
#include <utility>
#include <vector>

// Resetting the game: the runtime cannot start a game twice in one process, so a reset starts this program again
// (same command line) and quits. The new process waits for the old one to end (it writes the save on its way
// out) and goes straight into the game.
namespace restart {
    // At the start of main: remembers the command line and waits for the process that started this one.
    void init(int argc, char** argv);
    // True when this process was started by a reset and should start the game at once.
    bool start_game_at_once();
    // Starts this program again with these environment variables (an empty value removes one) and quits.
    void relaunch(const std::vector<std::pair<std::string, std::string>>& environment);
}

#endif
