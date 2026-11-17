#include "login.hpp"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>

#include "glue_data.hpp"
#include "perflog.hpp"
#include "ui_style.hpp"
#include "world.h"

static const char* kCfgPath = "sdmc:/3ds/w3dproto/login.cfg";
static const char* kGluePath = "sdmc:/3ds/w3dproto/glue.txt";
static const char* kOfflineDir = "sdmc:/3ds/w3dproto/offline";
static const char* kOfflinePackPath = "sdmc:/3ds/w3dproto/offline.w3do";
static const char* kOfflineSamplePath = "sdmc:/3ds/w3dproto/offline_sample.w3do";
static const char* kOfflineSavePath = "sdmc:/3ds/w3dproto/offline/profile.w3d";
static const char* kOfflineModelPath = "sdmc:/3ds/w3dproto/offline/party.w3nn";
static const char* kOfflineTracePath = "sdmc:/3ds/w3dproto/offline_trace.txt";
static const char* kOfflineAutoPath = "sdmc:/3ds/w3dproto/offline_start.txt";
static const char* kOfflineBuildTag = "offline-2026-11-17-b";
static const char* kSurvivorsSavePath = "sdmc:/3ds/w3dproto/offline/survivors.w3d";   // its own profile
static const char* kSurvivorsConfigPath = "sdmc:/3ds/w3dproto/survivors.txt";        // optional tuning (key=value)

// Survivors spawns creatures anywhere around the player: put them on the streamed ground.
static bool survivorsGround(float x, float y, float zRef, float* out) {
    return streamerFloorHeight(x, y, zRef + 6.0f, out);
}

static bool fileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// A crash that leaves no frame of evidence is expensive: every offline step appends one line to a
// small file and closes it at once, so even a hard fault leaves a trace on the SD card.
static void offlineTrace(const char* fmt, ...) {
    FILE* f = fopen(kOfflineTracePath, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void LoginFlow::init() {
    remove(kOfflineTracePath);
    offlineTrace("boot %s", kOfflineBuildTag);
    FILE* f = fopen(kCfgPath, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            if (k == "server") server_ = v;
            else if (k == "account") account_ = v;
            else if (k == "protocol") protocol_ = atoi(v.c_str()) == 8 ? 8 : 3;
            else if (k == "client") turtle_ = v == "turtle";   // 6.58
            else if (k == "log") logOn_ = atoi(v.c_str()) != 0;
            else if (k == "realm") lastRealm_ = v;
            else if (k == "character") lastChar_ = v;
        }
        fclose(f);
    }
    texts_.load(kGluePath);   // optional: the game's own texts, clothes of new characters, zone names
    phase_ = Phase::Menu;
    offlineTrace("init done");
    // Debug hook (also used by the emulator tests): jump straight into offline mode, optionally world.
    if (fileExists(kOfflineAutoPath)) {
        char buf[64] = {0};
        FILE* a = fopen(kOfflineAutoPath, "r");
        if (a) { if (!fgets(buf, sizeof buf, a)) buf[0] = 0; fclose(a); }
        offlineTrace("auto-start: %s", buf);
        if (strstr(buf, "survivors")) {
            startSurvivors();
        } else if (startOffline()) {
            ui_.character = 0;
            if (strstr(buf, "world")) enterWorldOffline();
        }
    }
}

void LoginFlow::saveConfig() {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/w3dproto", 0777);
    FILE* f = fopen(kCfgPath, "w");
    if (!f) return;
    // The password is never stored.
    fprintf(f, "server=%s\naccount=%s\nprotocol=%d\nclient=%s\nlog=%d\nrealm=%s\ncharacter=%s\n", server_.c_str(), account_.c_str(),
            protocol_, turtle_ ? "turtle" : "vanilla", logOn_ ? 1 : 0, lastRealm_.c_str(), lastChar_.c_str());
    fclose(f);
}

bool LoginFlow::editText(const char* hint, std::string& value, bool password, int maxLen) {
    SwkbdState kb;
    char buf[128];
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, std::min<int>(maxLen, (int)sizeof(buf) - 1));
    swkbdSetHintText(&kb, hint);
    if (!password) swkbdSetInitialText(&kb, value.c_str());
    swkbdSetPasswordMode(&kb, password ? SWKBD_PASSWORD_HIDE_DELAY : SWKBD_PASSWORD_NONE);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
    ui_.touched();
    if (b != SWKBD_BUTTON_RIGHT) return false;
    value = buf;
    memset(buf, 0, sizeof buf);
    return true;
}

void LoginFlow::message(const std::string& text) {
    model_.message = text;
    wait_.clear();
    waitFixed_ = false;
    busy_ = Busy::None;
    ui_.touched();
}

// ----------------------------------------------------------------- what the screens ask for
void LoginFlow::act(GlueAction a) {
    using ui::tr;
    switch (a) {
        case GlueAction::None: break;
        case GlueAction::EditServer:
            if (editText(tr("Server: host or host:port (3724 by default)", "Сервер: адрес или адрес:порт (по умолчанию 3724)"), server_, false))
                saveConfig();
            break;
        case GlueAction::EditAccount:
            if (editText(tr("Account name", "Учётная запись"), account_, false, 32)) saveConfig();
            break;
        case GlueAction::EditPassword: {
            std::string p;
            if (editText(tr("Password", "Пароль"), p, true, 32)) { std::fill(password_.begin(), password_.end(), '\0'); password_ = p; }
            std::fill(p.begin(), p.end(), '\0');
            break;
        }
        case GlueAction::Login:
            if (account_.empty() && !editText(tr("Account name", "Учётная запись"), account_, false, 32)) break;
            saveConfig();
            if (password_.empty()) {
                std::string p;
                if (!editText(tr("Password", "Пароль"), p, true, 32)) break;
                password_ = p;
                std::fill(p.begin(), p.end(), '\0');
            }
            wait_ = tr("Connecting...", "Соединение...");
            waitFixed_ = false;
            pending_ = Pending::Auth;   // next frame (the dialog is on the screen): the connect starts; polled from then on (6.55)
            break;
        case GlueAction::Offline:
            survivors_ = false;
            startOffline();
            break;
        case GlueAction::Survivors:
            startSurvivors();
            break;
        case GlueAction::Explore:
            phase_ = Phase::Offline;   // the developer's free-camera flight of the current map
            break;
        case GlueAction::Quit:
            quit_ = true;
            break;
        case GlueAction::ToggleProtocol:
            // 6.58: 1.12 protocol 3 -> 1.12 protocol 8 -> Turtle 1.18 (Turtle WoW clones: OctoWoW ...) -> back
            if (turtle_) { turtle_ = false; protocol_ = 3; }
            else if (protocol_ == 3) protocol_ = 8;
            else { turtle_ = true; protocol_ = 3; }
            saveConfig();
            break;
        case GlueAction::ToggleLog:
            logOn_ = !logOn_;
            saveConfig();
            break;
        case GlueAction::DismissMessage:
            model_.message.clear();
            break;
        case GlueAction::CancelWait:
            pending_ = Pending::None;
            wait_.clear();
            if (phase_ == Phase::Authenticating || phase_ == Phase::Menu) { auth_.close(); phase_ = Phase::Menu; }
            else if (phase_ == Phase::WorldConnecting || phase_ == Phase::RealmSelect) { world_.close(); phase_ = Phase::RealmSelect; }
            break;
        case GlueAction::RealmAccept: {
            const auto& rs = auth_.realms();
            if (ui_.realm < 0 || ui_.realm >= (int)rs.size()) break;
            const w3n::Realm& r = rs[(size_t)ui_.realm];
            if (r.flags & 0x02) { message(tr("That realm is offline.", "Этот мир сейчас не в сети.")); break; }
            lastRealm_ = r.name;
            saveConfig();
            wait_ = std::string(tr("Connecting to ", "Соединение с ")) + r.name + "...";
            waitFixed_ = false;
            pending_ = Pending::World;
            break;
        }
        case GlueAction::RealmCancel:
            auth_.close();
            phase_ = Phase::Menu;
            break;
        case GlueAction::EnterWorld: {
            if (offline_) { enterWorldOffline(); break; }
            const auto& cs = world_.characters();
            if (ui_.character < 0 || ui_.character >= (int)cs.size() || busy_ != Busy::None) break;
            enterWorld();
            break;
        }
        case GlueAction::CreateNew: {
            const auto& cs = world_.characters();
            uint8_t race = 1, sex = 0;
            if (ui_.character >= 0 && ui_.character < (int)cs.size()) { race = cs[(size_t)ui_.character].race; sex = cs[(size_t)ui_.character].gender; }
            phase_ = Phase::CharCreate;
            model_.appearanceKnown = false;
            ui_.startCreate(race, sex, model_);
            lookPending_ = true;
            break;
        }
        case GlueAction::DeleteCharacter: {
            const auto& cs = world_.characters();
            if (ui_.character < 0 || ui_.character >= (int)cs.size()) break;
            if (offline_) {
                const int r = offlineSession_.deleteCharacter(cs[(size_t)ui_.character].guid);
                world_.offlineSetDeleteResult(r);
                if (r == w3n::CHAR_DELETE_SUCCESS) {
                    world_.offlineSetCharacters(offlineSession_.characters());
                    ui_.character = 0;
                    phase_ = Phase::CharSelect;
                    ui_.touched();
                } else {
                    message(w3n::charDeleteResultText((uint8_t)r));
                }
                break;
            }
            world_.deleteCharacter(cs[(size_t)ui_.character].guid);
            busy_ = Busy::Delete;
            wait_ = tr("Deleting character...", "Удаление персонажа...");
            waitFixed_ = true;
            waitSince_ = w3n::nowMs();
            break;
        }
        case GlueAction::ChangeRealm:
            if (offline_) { leaveOffline(); break; }
            world_.close();
            phase_ = Phase::RealmSelect;
            break;
        case GlueAction::Back:
            if (offline_) { leaveOffline(); break; }
            world_.close();
            auth_.close();
            phase_ = Phase::Menu;
            break;
        case GlueAction::EditName: {
            std::string n = ui_.create.name;
            if (editText(tr("Name (2 to 12 letters)", "Имя (от 2 до 12 букв)"), n, false, 12)) {
                // the game's form: the first letter capital, the rest small (Latin)
                for (size_t i = 0; i < n.size(); ++i)
                    if ((unsigned char)n[i] < 128) n[i] = (char)(i ? tolower((unsigned char)n[i]) : toupper((unsigned char)n[i]));
                ui_.create.name = n;
            }
            break;
        }
        case GlueAction::CreateAccept: {
            if (ui_.create.name.empty()) { act(GlueAction::EditName); if (ui_.create.name.empty()) break; }
            w3n::CharCreateInfo ci;
            ci.name = ui_.create.name;
            ci.race = ui_.create.race;
            ci.cls = ui_.create.cls;
            ci.gender = ui_.create.sex;
            ci.skin = ui_.create.skin;
            ci.face = ui_.create.face;
            ci.hairStyle = ui_.create.hairStyle;
            ci.hairColor = ui_.create.hairColor;
            ci.facialHair = ui_.create.facial;
            if (offline_) {
                const int r = offlineSession_.createCharacter(ci);
                world_.offlineSetCreateResult(r);
                if (r == w3n::CHAR_CREATE_SUCCESS) {
                    world_.offlineSetCharacters(offlineSession_.characters());
                    selectAfterList_ = ci.name;
                    selectByName(ci.name);
                    phase_ = Phase::CharSelect;
                    ui_.touched();
                } else {
                    message(w3n::charCreateResultText((uint8_t)r));
                }
                break;
            }
            world_.createCharacter(ci);
            busy_ = Busy::Create;
            wait_ = tr("Creating character...", "Создание персонажа...");
            waitFixed_ = true;
            waitSince_ = w3n::nowMs();
            break;
        }
        case GlueAction::CreateBack:
            phase_ = Phase::CharSelect;
            break;
    }
}

void LoginFlow::selectByName(const std::string& name) {
    const auto& cs = world_.characters();
    for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i].name == name) { ui_.character = (int)i; ui_.modelYaw = 0; ui_.touched(); return; }
}

void LoginFlow::enterWorld() {
    const w3n::Character& c = world_.characters()[(size_t)ui_.character];
    charName_ = c.name;
    lastChar_ = c.name;
    saveConfig();
    world_.login(c.guid);
    phase_ = Phase::EnteringWorld;
    wait_ = ui::tr("Entering world...", "Вход в мир...");
    waitFixed_ = true;
}

// ----------------------------------------------------------------- offline
bool LoginFlow::startOffline() {
    using ui::tr;
    offlineTrace("startOffline: entry");
    perf::event("offline: build %s", kOfflineBuildTag);
    mkdir(kOfflineDir, 0777);
    std::string err;
    offlineData_ = w3n::offline::WorldData();
    bool loaded = false;
    if (fileExists(kOfflinePackPath)) {
        loaded = w3n::offline::loadWorldData(kOfflinePackPath, offlineData_, err);
    } else if (fileExists(kOfflineSamplePath)) {
        loaded = w3n::offline::loadWorldData(kOfflineSamplePath, offlineData_, err);
    } else {
        loaded = w3n::offline::buildSampleWorldData(offlineData_);
        if (!loaded) err = "the built-in offline sample is invalid";
    }
    offlineTrace("startOffline: pack=%d creatures=%u spawns=%u err=%s", loaded ? 1 : 0,
                 (unsigned)offlineData_.creatures.size(), (unsigned)offlineData_.spawns.size(), err.c_str());
    if (!loaded) {
        message(std::string(tr("Cannot open the offline world: ", "Не удалось открыть офлайн-мир: ")) + err);
        phase_ = Phase::Offline;   // fall back to the free-camera map flight
        return false;
    }
    if (survivors_) {
        w3n::offline::survivors::Config cfg;
        if (FILE* cf = fopen(kSurvivorsConfigPath, "rb")) {
            std::string text;
            char b[256];
            size_t n;
            while ((n = fread(b, 1, sizeof b, cf)) > 0) text.append(b, n);
            fclose(cf);
            offlineTrace("survivors: %d settings from survivors.txt", cfg.parse(text));
        }
        survivorsDirector_.setConfig(cfg);
        if (!survivorsDirector_.prepareData(offlineData_, &err)) {
            message("Survivors: " + err);
            survivors_ = false;
            return false;
        }
        offlineTrace("survivors: boss '%s'", survivorsDirector_.bossName().c_str());
    }
    offlineSession_.setup(&offlineData_, survivors_ ? kSurvivorsSavePath : kOfflineSavePath);
    offlineSession_.setSurvivors(survivors_ ? &survivorsDirector_ : nullptr);
    offlineTrace("startOffline: setup done");
    if (!offlineSession_.open(err)) {
        // A corrupt save must not lock the player out: move it aside and start a fresh profile.
        const std::string first = err;
        rename(kOfflineSavePath, "sdmc:/3ds/w3dproto/offline/profile.w3d.corrupt");
        rename("sdmc:/3ds/w3dproto/offline/profile.w3d.bak", "sdmc:/3ds/w3dproto/offline/profile.w3d.corrupt.bak");
        if (!offlineSession_.open(err)) {
            offlineTrace("startOffline: open failed %s / %s", first.c_str(), err.c_str());
            message(std::string(tr("Offline profile: ", "Офлайн-профиль: ")) + first + " / " + err);
            phase_ = Phase::Offline;
            return false;
        }
        message(tr("The offline save was unreadable; a new profile was started (profile.w3d.corrupt kept).",
                   "Файл сохранения повреждён: создан новый профиль (profile.w3d.corrupt)."));
    }
    offlineTrace("startOffline: open ok chars=%u", (unsigned)offlineSession_.characters().size());
    offlinePolicyLoaded_ = offlinePolicy_.load(kOfflineModelPath);
    offlineTrace("startOffline: model=%d", offlinePolicyLoaded_ ? 1 : 0);
    w3n::offline::setNeuralPolicy(offlinePolicyLoaded_ ? &offlinePolicy_ : nullptr);
    offline_ = true;
    phase_ = Phase::CharSelect;
    busy_ = Busy::None;
    pending_ = Pending::None;
    wait_.clear();
    waitFixed_ = false;
    world_.offlineSetCharacters(offlineSession_.characters());
    ui_.character = 0;
    selectByName(lastChar_);
    offlineTrace("startOffline: mirrored=%u", (unsigned)world_.characters().size());
    ui_.touched();
    offlineTrace("startOffline: done");
    return true;
}

void LoginFlow::enterWorldOffline() {
    const auto& cs = world_.characters();
    if (ui_.character < 0 || ui_.character >= (int)cs.size()) return;
    offlineTrace("enterWorldOffline: %s", cs[(size_t)ui_.character].name.c_str());
    charName_ = cs[(size_t)ui_.character].name;
    lastChar_ = charName_;
    saveConfig();
    phase_ = Phase::EnteringWorld;
    wait_ = ui::tr("Entering world...", "Вход в мир...");
    waitFixed_ = true;
}

bool LoginFlow::startSurvivors() {
    offlineTrace("startSurvivors");
    survivors_ = true;
    if (!startOffline()) { survivors_ = false; return false; }
    // Straight into the world with the first local character (Survivors has its own profile).
    ui_.character = 0;
    enterWorldOffline();
    return true;
}

void LoginFlow::leaveOffline() {
    if (offline_ && phase_ == Phase::InWorld) {
        offlineSession_.leaveWorld();
        game_.detach();
        world_.packetSink = nullptr;
        offlineSession_.setSink(nullptr);
    }
    w3n::offline::setNeuralPolicy(nullptr);
    offlinePolicyLoaded_ = false;
    offline_ = false;
    survivors_ = false;
    quitSurvivorsPending_ = false;
    survivorsDirector_.stop();
    offlineSession_.setSurvivors(nullptr);
    offlineSession_.setGroundQuery(nullptr);
    world_.offlineSetCharacters(std::vector<w3n::Character>());
    phase_ = Phase::Menu;
    wait_.clear();
    waitFixed_ = false;
    busy_ = Busy::None;
}

void LoginFlow::loadDataTables() {
    game_.spellNames.load("sdmc:/3ds/w3dproto/spells.txt");   // optional, from w3dconv
    if (!game_.factions.loaded()) game_.factions.load("sdmc:/3ds/w3dproto/factions.txt");
    if (!game_.talents.loaded()) game_.talents.load("sdmc:/3ds/w3dproto/talents.txt");
    if (!game_.spellDesc.loaded()) game_.spellDesc.load("sdmc:/3ds/w3dproto/spelldesc.bin");
    if (!game_.skills.loaded()) game_.skills.load("sdmc:/3ds/w3dproto/skills.txt");
    if (!game_.recipes.loaded()) game_.recipes.load("sdmc:/3ds/w3dproto/recipes.txt");
    if (!game_.taxiTable.loaded()) game_.taxiTable.load("sdmc:/3ds/w3dproto/taxi.txt");
    if (!game_.locks.loaded()) game_.locks.load("sdmc:/3ds/w3dproto/locks.txt");
    if (!game_.areaTriggers.loaded()) game_.areaTriggers.load("sdmc:/3ds/w3dproto/areatriggers.txt");
    if (!game_.emotes.loaded()) game_.emotes.load("sdmc:/3ds/w3dproto/emotes.txt");
    game_.objects.setTransportsFile("sdmc:/3ds/w3dproto/transports.txt");
}

// ----------------------------------------------------------------- the network
void LoginFlow::network() {
    if (offline_) { networkOffline(); return; }
    using ui::tr;
    w3n::ClientVersion ver = turtle_ ? w3n::ClientVersion::turtle() : w3n::ClientVersion();
    ver.authProtocol = (uint8_t)protocol_;
    // 7.18: the game language the server localizes quests, NPC names and gossip by. The converter's glue.txt carries the
    // one it was built with; an older pack of a vanilla client falls back to the interface language (Settings > Interface
    // > Language). A Turtle client keeps its own enUS unless glue.txt names a language.
    const std::string& gameLocale = texts_.gameLocale();
    ver.locale = !gameLocale.empty() ? gameLocale : (turtle_ ? std::string("enUS") : std::string(ui::languageCode(ui::g_lang)));
    if (pending_ == Pending::Auth) {   // the "Connecting..." dialog was drawn last frame
        pending_ = Pending::None;
        bool ok = auth_.start(server_, account_, password_, ver);
        std::fill(password_.begin(), password_.end(), '\0');
        password_.clear();
        if (!ok) { message(auth_.error()); phase_ = Phase::Menu; return; }
        phase_ = Phase::Authenticating;   // (6.55: "Connecting..." stays up until the server answers; Cancel works meanwhile)
    } else if (pending_ == Pending::World) {
        pending_ = Pending::None;
        const w3n::Realm& r = auth_.realms()[(size_t)ui_.realm];
        if (!world_.start(r.address, auth_.account(), auth_.sessionKey(), ver)) {
            message(world_.error());
            phase_ = Phase::RealmSelect;
            return;
        }
        phase_ = Phase::WorldConnecting;
        model_.realmName = r.name;
    }
    switch (phase_) {
        case Phase::Authenticating:
            auth_.update();
            if (auth_.state() == w3n::AuthClient::State::Failed) { message(auth_.error()); phase_ = Phase::Menu; }
            else if (auth_.state() == w3n::AuthClient::State::Connecting) {}
            else if (auth_.state() != w3n::AuthClient::State::Done) wait_ = tr("Authenticating...", "Проверка учётной записи...");
            else {
                wait_.clear();
                phase_ = Phase::RealmSelect;
                ui_.realm = 0;
                const auto& rs = auth_.realms();
                int online = -1, onlineCount = 0;
                for (size_t i = 0; i < rs.size(); ++i) {
                    if (rs[i].name == lastRealm_) ui_.realm = (int)i;
                    if (!(rs[i].flags & 0x02)) { online = (int)i; ++onlineCount; }
                }
                // as the game does: straight to the realm played last (or the only one); the list otherwise
                bool known = false;
                for (const auto& r : rs) known |= r.name == lastRealm_ && !(r.flags & 0x02);
                if (!known && onlineCount == 1) { ui_.realm = online; known = true; }
                if (known) act(GlueAction::RealmAccept);
            }
            break;
        case Phase::WorldConnecting:
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) { message(world_.error()); phase_ = Phase::RealmSelect; }
            else if (world_.state() == w3n::WorldClient::State::InQueue) {
                char b[96];
                snprintf(b, sizeof b, "%s %u", tr("Position in queue:", "Место в очереди:"), (unsigned)world_.queuePosition());
                wait_ = b;
            } else if (world_.state() == w3n::WorldClient::State::CharSelect) {
                wait_.clear();
                phase_ = Phase::CharSelect;
                ui_.character = 0;
                selectByName(lastChar_);
            } else if (world_.state() != w3n::WorldClient::State::Connecting) {   // (6.55: "Connecting to ..." until then)
                wait_ = tr("Retrieving character list...", "Получение списка персонажей...");
            }
            break;
        case Phase::CharSelect:
        case Phase::CharCreate: {
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) {
                message(std::string(tr("Disconnected: ", "Соединение потеряно: ")) + world_.error());
                phase_ = Phase::Menu;
                break;
            }
            int r = world_.takeCreateResult();
            if (r >= 0 && busy_ == Busy::Create) {
                if (r == w3n::CHAR_CREATE_SUCCESS) {
                    selectAfterList_ = ui_.create.name;
                    busy_ = Busy::List;
                    wait_ = tr("Retrieving character list...", "Получение списка персонажей...");
                } else {
                    message(w3n::charCreateResultText((uint8_t)r));
                }
            }
            r = world_.takeDeleteResult();
            if (r >= 0 && busy_ == Busy::Delete) {
                if (r == w3n::CHAR_DELETE_SUCCESS) {
                    busy_ = Busy::List;
                    selectAfterList_.clear();
                    ui_.character = 0;
                } else {
                    message(w3n::charDeleteResultText((uint8_t)r));
                }
            }
            if (busy_ == Busy::List && world_.state() == w3n::WorldClient::State::CharSelect) {
                busy_ = Busy::None;
                wait_.clear();
                waitFixed_ = false;
                phase_ = Phase::CharSelect;
                if (!selectAfterList_.empty()) selectByName(selectAfterList_);
                selectAfterList_.clear();
                ui_.touched();
            }
            if ((busy_ == Busy::Create || busy_ == Busy::Delete) && w3n::nowMs() - waitSince_ > 15000)
                message(tr("The server did not answer.", "Сервер не ответил."));
            break;
        }
        case Phase::EnteringWorld:
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) {
                message(world_.error());
                phase_ = Phase::Menu;
            } else if (world_.state() == w3n::WorldClient::State::InWorld) {
                wait_.clear();
                sessionT0_ = w3n::nowMs();
                if (logOn_ && log_.open("sdmc:/3ds/w3dproto/session.w3dl")) {
                    // LOGIN_VERIFY_WORLD is consumed by WorldClient: keep a copy for replays.
                    w3n::Writer w;
                    const float* p = world_.position();
                    w.u32(world_.mapId());
                    w.f32(p[0]); w.f32(p[1]); w.f32(p[2]); w.f32(world_.orientation());
                    log_.write(0, 0, 0x236, w.b);
                    game_.tap = [this](uint32_t ms, int dir, uint16_t op, const w3n::Bytes& d) { log_.write(ms, dir, op, d); };
                } else {
                    game_.tap = nullptr;
                }
                loadDataTables();
                game_.attach(world_, sessionMs());
                phase_ = Phase::InWorld;
                entered_ = true;
            }
            break;
        case Phase::InWorld:
            world_.update();   // keep-alive pings, drain server packets
            if (world_.state() == w3n::WorldClient::State::Failed) {
                game_.detach();
                log_.close();
                logoutPending_ = false;
                message(std::string(tr("Disconnected: ", "Соединение потеряно: ")) + world_.error());
                phase_ = Phase::Menu;
            } else if (logoutPending_) {
                const auto st = world_.state();
                if (world_.takeLogoutRefused()) {   // in combat, for example
                    logoutPending_ = false;
                    game_.addSystem(tr("You can't log out now.", "Сейчас нельзя выйти."), sessionMs());
                } else if (st == w3n::WorldClient::State::CharSelect) {   // the list is here: the character screen
                    logoutPending_ = false;
                    game_.detach();
                    log_.close();
                    wait_.clear();
                    waitFixed_ = false;
                    busy_ = Busy::None;
                    pending_ = Pending::None;
                    phase_ = Phase::CharSelect;
                    ui_.character = 0;
                    selectByName(charName_);
                    ui_.touched();
                } else if (st == w3n::WorldClient::State::Done) {   // the server did not answer in time
                    logoutPending_ = false;
                    game_.detach();
                    log_.close();
                    message(tr("The server did not answer.", "Сервер не ответил."));
                    phase_ = Phase::Menu;
                }
            }
            break;
        default:
            break;
    }
}

// ----------------------------------------------------------------- offline session driver
void LoginFlow::networkOffline() {
    using ui::tr;
    switch (phase_) {
        case Phase::EnteringWorld: {
            offlineTrace("networkOffline: entering world");
            const auto& cs = world_.characters();
            if (ui_.character < 0 || ui_.character >= (int)cs.size()) {
                message(tr("No character selected.", "Персонаж не выбран."));
                phase_ = Phase::CharSelect;
                break;
            }
            const w3n::Character& c = cs[(size_t)ui_.character];
            sessionT0_ = w3n::nowMs();
            float pos[3] = {0, 0, 0};
            float o = 0;
            if (!offlineSession_.enterWorld(c.guid, 0, pos, &o)) {
                offlineTrace("networkOffline: enterWorld failed");
                message(tr("The offline character could not enter the world.", "Персонаж не смог войти в мир."));
                phase_ = Phase::CharSelect;
                break;
            }
            offlineTrace("networkOffline: world reset at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
            if (survivors_) offlineSession_.setGroundQuery(&survivorsGround);
            // The same logical message boundary as a network session: CMSG out, SMSG in.
            world_.packetSink = [this](uint16_t op, const w3n::Bytes& d) {
                offlineSession_.onClientPacket(op, d, sessionMs());
            };
            offlineSession_.setSink([this](uint16_t op, const w3n::Bytes& d) {
                game_.handle(op, d, sessionMs());
            });
            world_.beginOffline(c.guid, offlineSession_.characters(), offlineSession_.mapId(), pos, o);
            offlineTrace("networkOffline: beginOffline map=%u", offlineSession_.mapId());
            loadDataTables();
            offlineTrace("networkOffline: tables loaded");
            game_.tap = nullptr;
            game_.attach(world_, 0);
            offlineTrace("networkOffline: attached");
            offlineSimTicks_ = offlineSimMaxTicks_ = 0;
            offlineFrames_ = 0;
            offlineReportMs_ = sessionMs();
            {
                const w3n::offline::WorldData* d = offlineSession_.world().data();
                perf::event("offline: pack '%s', map %u | %u creatures, %u spawns, %u items, %u spells | policy %d",
                            d ? d->name.c_str() : "?", offlineSession_.mapId(),
                            d ? (unsigned)d->creatures.size() : 0u, d ? (unsigned)d->spawns.size() : 0u,
                            d ? (unsigned)d->items.size() : 0u, d ? (unsigned)d->spells.size() : 0u,
                            offlinePolicyLoaded_ ? 1 : 0);
            }
            phase_ = Phase::InWorld;
            wait_.clear();
            waitFixed_ = false;
            entered_ = true;
            break;
        }
        case Phase::InWorld: {
            // Fixed-step simulation, bot director, packets and (optionally) the tiny policy.
            const uint64_t t0 = svcGetSystemTick();
            offlineSession_.update(sessionMs());
            const uint64_t dt = svcGetSystemTick() - t0;
            offlineSimTicks_ += dt;
            if (dt > offlineSimMaxTicks_) offlineSimMaxTicks_ = dt;
            ++offlineFrames_;
            if (sessionMs() - offlineReportMs_ >= 1000) {
                const double avg = offlineFrames_ ? (double)offlineSimTicks_ * 1000.0 / (double)SYSCLOCK_ARM11 / (double)offlineFrames_ : 0.0;
                const double mx = (double)offlineSimMaxTicks_ * 1000.0 / (double)SYSCLOCK_ARM11;
                int used = 0;
                for (int i = 0; i < w3n::offline::kMaxEntities; ++i) {
                    const w3n::offline::Entity* e = offlineSession_.world().byIndex(i);
                    if (e && e->used) ++used;
                }
                perf::event("offline: %u frames, sim avg %.2f ms max %.2f ms | %u packets in / %u out | %d entities | policy %d",
                            offlineFrames_, avg, mx, offlineSession_.stats.packetsIn, offlineSession_.stats.packetsOut, used,
                            offlinePolicyLoaded_ ? 1 : 0);
                if (survivors_)
                    perf::event("survivors: %u:%02u, %d alive, level %u, %u kills, phase %d", survivorsDirector_.runMs() / 60000,
                                (survivorsDirector_.runMs() / 1000) % 60, survivorsDirector_.alive(),
                                survivorsDirector_.stats().level, survivorsDirector_.stats().kills, (int)survivorsDirector_.phase());
                offlineSimTicks_ = offlineSimMaxTicks_ = 0;
                offlineFrames_ = 0;
                offlineReportMs_ = sessionMs();
            }
            break;
        }
        default:
            break;
    }
}

// ----------------------------------------------------------------- the screens
void LoginFlow::fillModel() {
    switch (phase_) {
        case Phase::Menu: case Phase::Authenticating: model_.screen = GlueScreen::Login; break;
        case Phase::RealmSelect: model_.screen = GlueScreen::RealmList; break;
        case Phase::WorldConnecting:
            model_.screen = world_.state() == w3n::WorldClient::State::CharSelect ? GlueScreen::CharSelect : GlueScreen::RealmList;
            break;
        case Phase::CharSelect: case Phase::EnteringWorld: model_.screen = GlueScreen::CharSelect; break;
        case Phase::CharCreate: model_.screen = GlueScreen::CharCreate; break;
        default: break;
    }
    model_.server = server_;
    model_.account = account_;
    model_.havePassword = !password_.empty();
    model_.protocol = protocol_;
    model_.turtle = turtle_;
    glue::setTurtle(turtle_);
    model_.packetLog = logOn_;
    model_.wait = wait_;
    model_.waitFixed = waitFixed_;
    model_.realms = &auth_.realms();
    model_.chars = &world_.characters();
    model_.texts = &texts_;
    model_.offline = offline_;
    if (phase_ == Phase::CharCreate) {   // the choices of the chosen race / sex, from its CHRS entry
        const uint8_t* blob = nullptr;
        uint32_t size = 0;
        const int st = streamerCharInfo(ui_.create.race, ui_.create.sex, &blob, &size);
        model_.appearanceKnown = st == 1;
        if (st == 1) {
            glue::appearanceFrom(blob, size, ui_.create.skin, ui_.create.hairStyle, model_.appearance);
            if (lookPending_) {   // as the game: a new character starts with a random look
                ui_.randomize(model_);
                glue::appearanceFrom(blob, size, ui_.create.skin, ui_.create.hairStyle, model_.appearance);
                lookPending_ = false;
            }
            ui_.clampLook(model_.appearance);
            glue::appearanceFrom(blob, size, ui_.create.skin, ui_.create.hairStyle, model_.appearance);   // after the clamp
        }
    }
}

bool LoginFlow::update(u32 down, u32 held, bool touching, int tx, int ty, float dt) {
    if (phase_ == Phase::Offline) return false;
    if (quitSurvivorsPending_) {
        offlineTrace("survivors: back to the main menu");
        leaveOffline();
        ui_.touched();
    }
    network();
    if (phase_ == Phase::InWorld) return false;
    fillModel();
    GlueInput in;
    in.down = down;
    in.held = held;
    in.touching = touching;
    in.touchDown = touching && !touchWas_;
    touchWas_ = touching;
    in.tx = tx;
    in.ty = ty;
    in.dt = dt;
    const GlueAction a = ui_.update(model_, in);
    if (a != GlueAction::None) {
        act(a);
        fillModel();
    }
    if (phase_ == Phase::Offline || phase_ == Phase::InWorld) return false;
    ui_.buildTop(top_, model_);
    ui_.buildBottom(bottom_, model_);
    return true;
}

LoginFlow::Scene LoginFlow::scene() const {
    Scene s;
    const auto& cs = world_.characters();
    if (phase_ == Phase::CharCreate) {
        const GlueCreate& c = ui_.create;
        s.id = c.race;
        s.character = true;
        s.look = W3dLook{c.race, c.sex, c.skin, c.face, c.hairStyle, c.hairColor, c.facial};
        uint32_t disp[19];
        uint8_t inv[19];
        if (texts_.outfit(c.race, c.cls, c.sex, disp, inv))
            for (int k = 0; k < W3D_EQ_SLOTS && k < 19; ++k) { s.equip[k] = disp[k]; s.equipInv[k] = inv[k]; }
        s.yaw = ui_.modelYaw;
    } else if ((phase_ == Phase::CharSelect || phase_ == Phase::EnteringWorld) && model_.screen == GlueScreen::CharSelect) {
        if (ui_.character >= 0 && ui_.character < (int)cs.size()) {
            const w3n::Character& c = cs[(size_t)ui_.character];
            s.id = c.race;
            s.character = true;
            s.look = W3dLook{c.race, c.gender, c.skin, c.face, c.hairStyle, c.hairColor, c.facialHair};
            for (int k = 0; k < W3D_EQ_SLOTS; ++k) { s.equip[k] = c.equipDisplay[k]; s.equipInv[k] = c.equipInv[k]; }
            s.yaw = ui_.modelYaw;
        } else {
            s.id = 1;   // an empty list: the human scene without anyone
        }
    }
    return s;
}

bool LoginFlow::requestCharacterList() {
    if (offline_ && survivors_) {   // Survivors has no character list: back to the main menu
        if (phase_ == Phase::InWorld) quitSurvivorsPending_ = true;
        return false;
    }
    if (offline_) {
        if (phase_ != Phase::InWorld) return false;
        offlineSession_.leaveWorld();
        game_.detach();
        world_.packetSink = nullptr;
        offlineSession_.setSink(nullptr);
        world_.offlineSetCharacters(offlineSession_.characters());
        phase_ = Phase::CharSelect;
        ui_.character = 0;
        selectByName(charName_);
        ui_.touched();
        return false;   // no server-side logout timer in offline mode
    }
    if (phase_ != Phase::InWorld || logoutPending_ || world_.state() != w3n::WorldClient::State::InWorld) return false;
    game_.mover.stopAll(sessionMs());
    game_.update(sessionMs());   // the stop packets leave before the request
    world_.logout(true);
    logoutPending_ = true;
    return true;
}

void LoginFlow::cancelLogout() {
    if (logoutPending_ && world_.cancelLogout()) logoutPending_ = false;
}

void LoginFlow::shutdown() {
    w3n::offline::setNeuralPolicy(nullptr);
    if (offline_) {
        if (phase_ == Phase::InWorld) {
            offlineSession_.leaveWorld();
            game_.detach();
            world_.packetSink = nullptr;
        }
        offline_ = false;
        log_.close();
        world_.close();
        auth_.close();
        w3n::netShutdown();
        return;
    }
    if (world_.state() == w3n::WorldClient::State::InWorld) {
        game_.mover.stopAll(sessionMs());
        game_.update(sessionMs());
        game_.detach();
        world_.logout();
        u64 until = osGetTime() + 1000;   // give the request a moment to leave
        while (osGetTime() < until) { world_.update(); svcSleepThread(10 * 1000000LL); }
    }
    log_.close();
    world_.close();
    auth_.close();
    w3n::netShutdown();
}
