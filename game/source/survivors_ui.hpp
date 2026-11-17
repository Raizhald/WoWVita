// survivors_ui.hpp — the Survivors mode's screens on top of the game HUD: the run timer, kills and the
// level's progress, the boss bar, the three level-up cards and the end-of-run results.
// Buttons: D-pad left / right picks a card, A takes it; on the results A = Try again, B = Main menu.
// A tap on the top screen (the Vita's front touch over the scene) works on the cards and the buttons too.
#pragma once
#include <3ds.h>

#include "hud.hpp"
#include "offline/survivors.hpp"

namespace survivorsUi {

struct Input {
    u32 down = 0;             // buttons pressed this frame
    bool tap = false;         // a tap on the top screen this frame
    int tapX = 0, tapY = 0;   // in top-screen pixels (400 x 240)
};

enum class Action { None, Restart, MainMenu };

// True while a screen of the mode is up (the cards, the results): the game must not move or act.
bool modal(const w3n::offline::survivors::Director& d);
// Reads the buttons / tap for the mode's screens. Call before the game's own UI sees the input.
Action update(w3n::offline::survivors::Director& d, const Input& in);
// Adds the mode's overlay to the top-screen HUD list (after the game HUD).
void draw(HudList& hud, const w3n::offline::survivors::Director& d);

}  // namespace survivorsUi
