// survivors_ui.cpp — see survivors_ui.hpp.
#include "survivors_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "buttons.hpp"

namespace survivorsUi {

using namespace w3n::offline::survivors;

namespace {

constexpr uint32_t kGold = hudRgba(255, 209, 0);
constexpr uint32_t kWhite = hudRgba(255, 255, 255);
constexpr uint32_t kDim = hudRgba(190, 190, 190);

// Card layout on the top screen (400 x 240).
constexpr int kCardW = 118, kCardH = 112, kCardY = 62, kCardGap = 9;
// Results buttons.
constexpr int kBtnW = 110, kBtnH = 24, kBtnY = 178;

int selected = 0;   // the highlighted card

int cardX(int i, int n) {
    const int total = n * kCardW + (n - 1) * kCardGap;
    return (kHudW - total) / 2 + i * (kCardW + kCardGap);
}

bool inside(int x, int y, int bx, int by, int bw, int bh) { return x >= bx && x < bx + bw && y >= by && y < by + bh; }

uint32_t schoolColor(Upgrade u, int a = 255) {
    switch (u) {
        case Upgrade::Fireball: case Upgrade::FireBlast: return hudRgba(255, 120, 40, a);
        case Upgrade::FrostNova: case Upgrade::Frostbolt: return hudRgba(90, 180, 255, a);
        case Upgrade::ArcaneExplosion: return hudRgba(200, 120, 255, a);
        case Upgrade::Potion: return hudRgba(255, 70, 70, a);
        default: return hudRgba(110, 220, 110, a);   // passives
    }
}

std::string clock(uint32_t ms) {
    char b[16];
    snprintf(b, sizeof b, "%u:%02u", ms / 60000, (ms / 1000) % 60);
    return b;
}

void button(HudList& l, int x, int y, int w, int h, const std::string& label, bool hot) {
    l.rect(x, y, w, h, hot ? hudRgba(120, 20, 10, 235) : hudRgba(60, 10, 6, 220));
    l.outline(x, y, w, h, hot ? kGold : hudRgba(150, 120, 60));
    l.textCentered(x + w / 2, y + (h - HudList::lineHeight(ui::Font::Normal)) / 2, label, kGold, true, ui::Font::Normal);
}

}  // namespace

bool modal(const Director& d) { return d.paused(); }

Action update(Director& d, const Input& in) {
    if (d.phase() == Phase::LevelUp) {
        const int n = (int)d.cards().size();
        if (n <= 0) return Action::None;
        if (selected >= n) selected = n - 1;
        if (selected < 0) selected = 0;
        if (in.down & KEY_DLEFT) selected = (selected + n - 1) % n;
        if (in.down & KEY_DRIGHT) selected = (selected + 1) % n;
        if (in.tap) {
            for (int i = 0; i < n; ++i)
                if (inside(in.tapX, in.tapY, cardX(i, n), kCardY, kCardW, kCardH)) {
                    if (selected == i) { d.choose(i); selected = 0; return Action::None; }   // a second tap takes it
                    selected = i;
                }
        }
        if (in.down & KEY_A) { d.choose(selected); selected = 0; }
        return Action::None;
    }
    if (d.phase() == Phase::Won || d.phase() == Phase::Lost) {
        const int x0 = kHudW / 2 - kBtnW - 6, x1 = kHudW / 2 + 6;
        if ((in.down & KEY_A) || (in.tap && inside(in.tapX, in.tapY, x0, kBtnY, kBtnW, kBtnH))) return Action::Restart;
        if ((in.down & KEY_B) || (in.tap && inside(in.tapX, in.tapY, x1, kBtnY, kBtnW, kBtnH))) return Action::MainMenu;
    }
    return Action::None;
}

void draw(HudList& l, const Director& d) {
    if (!d.active()) return;
    const RunStats& st = d.stats();

    // ---- while running: the run's progress bar along the top edge, the clock, kills
    if (d.phase() == Phase::Running) {
        const float fill = d.xpNext() ? (float)d.xp() / (float)d.xpNext() : 0.0f;
        l.rect(0, 0, kHudW, 4, hudRgba(0, 0, 0, 170));
        l.rect(0, 0, (int)(kHudW * std::min(1.0f, fill)), 4, hudRgba(80, 160, 255, 230));
        const int lh = HudList::lineHeight(ui::Font::Large);
        l.textCentered(kHudW / 2, 6, clock(st.timeMs), kWhite, true, ui::Font::Large);
        char b[64];
        snprintf(b, sizeof b, "Lv %u   Kills %u", st.level, st.kills);
        l.textCentered(kHudW / 2, 6 + lh, b, kDim, true, ui::Font::Small);
        if (d.bossAlive()) {
            const int bw = 200, bx = (kHudW - bw) / 2, by = 6 + lh + 13;
            l.bar(bx, by, bw, 7, d.bossHealth(), hudRgba(200, 30, 30));
            l.textCentered(kHudW / 2, by + 8, d.bossName(), hudRgba(255, 120, 90), true, ui::Font::Small);
        } else if (st.timeMs < d.bossAtMs()) {
            const uint32_t left = d.bossAtMs() - st.timeMs;
            if (left <= 60000) {
                snprintf(b, sizeof b, "Boss in %s", clock(left).c_str());
                l.textCentered(kHudW / 2, 6 + lh + 12, b, hudRgba(255, 120, 90), true, ui::Font::Small);
            }
        }
    }

    // ---- level-up: three cards
    if (d.phase() == Phase::LevelUp) {
        const auto& cards = d.cards();
        const int n = (int)cards.size();
        l.rect(0, 0, kHudW, kHudH, hudRgba(0, 0, 0, 140));
        char t[48];
        snprintf(t, sizeof t, "Level %u!", st.level);
        l.textCentered(kHudW / 2, 14, t, kGold, true, ui::Font::Large);
        l.textCentered(kHudW / 2, 40, d.pendingLevelUps() > 1 ? "Choose an upgrade (more to come)" : "Choose an upgrade", kWhite);
        for (int i = 0; i < n; ++i) {
            const Card& c = cards[(size_t)i];
            const int x = cardX(i, n), y = kCardY;
            const bool hot = i == selected;
            l.rect(x, y, kCardW, kCardH, hot ? hudRgba(40, 28, 16, 240) : hudRgba(20, 14, 10, 225));
            l.rect(x, y, kCardW, 4, schoolColor(c.id));
            l.outline(x, y, kCardW, kCardH, hot ? kGold : hudRgba(110, 90, 60));
            if (hot) l.outline(x - 1, y - 1, kCardW + 2, kCardH + 2, kGold);
            l.textCentered(x + kCardW / 2, y + 9, HudList::fit(upgradeName(c.id), kCardW - 8, ui::Font::Normal), schoolColor(c.id),
                           true, ui::Font::Normal);
            std::string lvl;
            if (c.id == Upgrade::Potion) lvl = "Consumable";
            else if (c.nextLevel <= 1) lvl = upgradeIsWeapon(c.id) ? "New spell!" : "New!";
            else { char b[24]; snprintf(b, sizeof b, "Level %u", c.nextLevel); lvl = b; }
            l.textCentered(x + kCardW / 2, y + 27, lvl, c.nextLevel <= 1 ? kGold : kDim);
            int ty = y + 46;
            for (const std::string& line : HudList::wrap(upgradeText(c.id, c.nextLevel), kCardW - 12)) {
                if (ty > y + kCardH - 12) break;
                l.text(x + 6, ty, line, kWhite);
                ty += HudList::lineHeight(ui::Font::Small);
            }
        }
        l.textCentered(kHudW / 2, kCardY + kCardH + 10, btnText("{DPAD}: choose   {A}: take   (or tap a card twice)"), kDim);
        return;
    }

    // ---- results
    if (d.phase() == Phase::Won || d.phase() == Phase::Lost) {
        const bool won = d.phase() == Phase::Won;
        l.rect(0, 0, kHudW, kHudH, hudRgba(0, 0, 0, 165));
        const int pw = 260, ph = 170, px = (kHudW - pw) / 2, py = 40;
        l.rect(px, py, pw, ph, hudRgba(20, 14, 10, 235));
        l.outline(px, py, pw, ph, kGold);
        l.textCentered(kHudW / 2, py + 10, won ? "Victory!" : "You have fallen", won ? kGold : hudRgba(255, 70, 70), true,
                       ui::Font::Large);
        struct Row { const char* k; std::string v; };
        char b[4][32];
        snprintf(b[0], sizeof b[0], "%u", st.level);
        snprintf(b[1], sizeof b[1], "%u", st.kills);
        snprintf(b[2], sizeof b[2], "%u", st.damageDone);
        snprintf(b[3], sizeof b[3], "%u", st.damageTaken);
        const Row rows[] = {{"Time survived", clock(st.timeMs)}, {"Level", b[0]}, {"Kills", b[1]},
                            {"Damage done", b[2]}, {"Damage taken", b[3]}};
        int y = py + 38;
        for (const Row& r : rows) {
            l.text(px + 30, y, r.k, kDim);
            l.textRight(px + pw - 30, y, r.v, kWhite);
            y += HudList::lineHeight(ui::Font::Small) + 2;
        }
        button(l, kHudW / 2 - kBtnW - 6, kBtnY, kBtnW, kBtnH, btnText("{A} Try again"), true);
        button(l, kHudW / 2 + 6, kBtnY, kBtnW, kBtnH, btnText("{B} Main menu"), false);
    }
}

}  // namespace survivorsUi
