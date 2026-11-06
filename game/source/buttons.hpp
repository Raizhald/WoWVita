// buttons.hpp - the button names the player sees (WoWVita: the PS Vita's).
//
// The game reads the 3DS buttons; platform/src/platform_vita.cpp maps the Vita's onto them:
//   Cross / Circle -> A / B (swapped when the system's enter button is Circle), Triangle -> X, Square -> Y,
//   L / R -> L / R, the L2 / R2 touch buttons over the hotbars (main.cpp) -> ZL / ZR,
//   left / right stick -> Circle Pad / C-Stick.
// So every prompt names the game's button through btnName / btnText, never as a literal "A".
#pragma once
#include <string>

enum class Btn { A, B, X, Y, L, R, ZL, ZR, Start, Select, CPad, CStick, DPad };

const char* btnName(Btn b);   // "✕", "○", "△", "□", "L", "R", "L2", "R2", "START", "SELECT", ...
const char* btnAscii(Btn b);  // the same in ASCII ("Cross", "Circle", ...): the text console has no other glyphs
// A text with {A} {B} {X} {Y} {L} {R} {ZL} {ZR} {START} {SELECT} {CPAD} {CSTICK} {DPAD} replaced by the names.
std::string btnText(const char* text);
// A pad slot's glyph (the hotbar cells): 0..3 the D-pad arrows, 4..7 the face buttons X A B Y.
const char* btnSlotGlyph(int b);
