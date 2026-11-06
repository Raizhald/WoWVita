#include "buttons.hpp"

#include <cstring>

// platform_vita.cpp / platform_pc.cpp: the system's enter button is Circle (Japanese consoles): Circle is A then
extern "C" int compatEnterIsCircle(void);

const char* btnName(Btn b) {
    const bool circle = compatEnterIsCircle() != 0;
    switch (b) {
        case Btn::A: return circle ? "○" : "✕";
        case Btn::B: return circle ? "✕" : "○";
        case Btn::X: return "△";
        case Btn::Y: return "□";
        case Btn::L: return "L";
        case Btn::R: return "R";
        case Btn::ZL: return "L2";
        case Btn::ZR: return "R2";
        case Btn::Start: return "START";
        case Btn::Select: return "SELECT";
        case Btn::CPad: return "Left stick";
        case Btn::CStick: return "Right stick";
        case Btn::DPad: return "D-pad";
    }
    return "?";
}

const char* btnAscii(Btn b) {
    const bool circle = compatEnterIsCircle() != 0;
    switch (b) {
        case Btn::A: return circle ? "Circle" : "Cross";
        case Btn::B: return circle ? "Cross" : "Circle";
        case Btn::X: return "Triangle";
        case Btn::Y: return "Square";
        default: return btnName(b);
    }
}

std::string btnText(const char* text) {
    static const struct { const char* key; Btn b; } kKeys[] = {
        {"{A}", Btn::A}, {"{B}", Btn::B}, {"{X}", Btn::X}, {"{Y}", Btn::Y}, {"{L}", Btn::L}, {"{R}", Btn::R},
        {"{ZL}", Btn::ZL}, {"{ZR}", Btn::ZR}, {"{START}", Btn::Start}, {"{SELECT}", Btn::Select},
        {"{CPAD}", Btn::CPad}, {"{CSTICK}", Btn::CStick}, {"{DPAD}", Btn::DPad},
    };
    std::string out;
    for (const char* p = text ? text : ""; *p;) {
        bool hit = false;
        if (*p == '{')
            for (const auto& k : kKeys) {
                const size_t n = strlen(k.key);
                if (!strncmp(p, k.key, n)) {
                    out += btnName(k.b);
                    p += n;
                    hit = true;
                    break;
                }
            }
        if (!hit) out += *p++;
    }
    return out;
}

const char* btnSlotGlyph(int b) {
    static const char* kArrow[4] = {"↑", "→", "↓", "←"};
    static const Btn kFace[4] = {Btn::X, Btn::A, Btn::B, Btn::Y};
    if (b >= 0 && b < 4) return kArrow[b];
    if (b >= 4 && b < 8) return btnName(kFace[b - 4]);
    return "?";
}
