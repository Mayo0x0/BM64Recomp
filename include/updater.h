#ifndef __UPDATER_H__
#define __UPDATER_H__

#include <string>

// Updates from the GitHub releases of this fork (src/game/updater.cpp): the launcher checks for a newer release
// (Settings > General > Check for Updates), downloads its Windows zip and replaces the program's files with it.
// Files in use cannot be overwritten on Windows, so the old ones are renamed and removed at the next start.
namespace updater {
    enum class Phase { Idle, Checking, UpToDate, Available, Downloading, Installing, Done, Failed };

    struct Status {
        Phase phase = Phase::Idle;
        std::string latest;  // newest release's version, "1.5.0"
        std::string title;   // its name on GitHub
        std::string error;   // Failed: why
        int percent = 0;     // Downloading
    };

    // At the start of main, after restart::init: removes the files the last update renamed.
    void cleanup();
    // False where there is no release to install (only Windows builds are released).
    bool supported();
    // Asks GitHub for the newest release in the background, unless a check or an install runs.
    void check();
    // Downloads and installs the newest release in the background (after check() found one).
    void install();
    Status status();
}

#endif
