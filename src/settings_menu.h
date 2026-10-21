// In-game settings menu: F1, or Back + Start on a controller. Only the options players need, applied
// at once where the game allows it and saved to the settings file next to the executable. While it
// is open the game gets no input (Svr*App routes the input system's active callback through it).

#pragma once

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "updater.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#endif

// Set by the app's OnConfigureFonts; nullptr falls back to ImGui's default font.
inline ImFont* g_menu_font = nullptr;

class SettingsMenuDialog : public rex::ui::ImGuiDialog {
 public:
  struct Hooks {
    std::function<bool()> is_fullscreen;
    std::function<void(bool)> set_fullscreen;
    std::function<bool()> fps_counter_on;
    std::function<void(bool)> set_fps_counter;
    std::function<svr::Updater*()> updater;  // nullptr until the game has started
    std::function<void()> quit;               // after the updater started the new version
    // Every controller the game sees (SDL: PlayStation, Switch, Xbox...), in XInput button bits,
    // read even while the game's own input is off. XInput alone only sees Xbox-style pads.
    std::function<bool(uint16_t& buttons, int16_t& lx, int16_t& ly)> read_pad;
    std::function<void(bool)> set_controllers_shared;  // true: every controller is player 1
    std::function<void(bool)> set_high_priority;  // above-normal process priority
    // What the Auto choices pick on this PC ("1440p", "Direct3D 12"), shown as "Auto (1440p)".
    std::function<std::string()> auto_resolution;
    std::function<std::string()> auto_graphics_api;
  };

  SettingsMenuDialog(rex::ui::ImGuiDrawer* drawer, std::string title, std::filesystem::path toml,
                     Hooks hooks)
      : ImGuiDialog(drawer), title_(std::move(title)), toml_(std::move(toml)), hooks_(std::move(hooks)) {
#if defined(_WIN32)
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryA(dll)) {
        xinput_get_state_ = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
        if (xinput_get_state_)
          break;
      }
    }
#endif
  }

  void Toggle() { open_ ? Close() : Open(); }
  // Game input stays off while the menu is open, and until the buttons used to close it are released.
  bool BlocksGameInput() const { return open_ || hold_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Pad pad = ReadPad();
    // Developer aid: SVR_OPEN_MENU_AT_MS=<ms> opens the menu once that long after start (screenshots
    // and tests without keyboard focus).
    static const long long open_at = [] {
      const char* v = std::getenv("SVR_OPEN_MENU_AT_MS");
      return v ? std::atoll(v) : -1LL;
    }();
    static const auto start = Clock::now();
    static bool auto_opened = false;
    if (open_at >= 0 && !auto_opened &&
        Clock::now() - start >= std::chrono::milliseconds(open_at)) {
      auto_opened = true;
      Open();
    }
    if (!open_) {
      if (hold_ && !pad.any)
        hold_ = false;
      if (pad.combo && !last_.combo)
        Open();
      last_ = pad;
      if (!open_) {
        DrawUpdateNotice(io);
        return;
      }
    }
    HandleInput(pad);
    last_ = pad;
    if (open_)
      Draw(io);
  }

 private:
  enum RowId {
    kResolution,
    kGraphicsApi,
    kDisplay,
    kFrameRate,
    kShape,
    kEdgeBlur,
    kDepthOfField,
    kCounter,
    kSound,
    kLanguage,
    kKeyboard,
    kControllers,
    kPriority,
    kCheckUpdates,
    kUpdate,
    kClose,
    kRows
  };
  struct Row {
    const char* name;
    std::vector<const char*> labels;
    const char* hint;
  };
  struct Pad {
    bool up = false, down = false, left = false, right = false, a = false, b = false, combo = false,
         any = false;
  };
  using Clock = std::chrono::steady_clock;

  const std::vector<Row>& Rows() const {
    static const std::vector<Row> rows = {
        {"Resolution",
         {"Auto (recommended)", "480p (low-end PCs)", "720p (original)", "1080p", "1440p", "4K",
          "8K"},
         "The game renders at this resolution and scales it to your screen: higher is smoother and "
         "sharper. Auto picks 1440p on 1080p/1440p screens, 4K on 4K screens, 720p on Steam Deck and "
         "laptops' built-in graphics. 480p helps weak graphics cards; 8K needs a top-end one with "
         "lots of video memory. Applies the next time you start the game."},
        {"Graphics API", {"Auto (recommended)", "Direct3D 12", "Vulkan"},
         "Auto draws with Direct3D 12 on Windows and Vulkan on Steam Deck / Linux. If the game "
         "looks wrong or runs badly, try the other one. Applies the next time you start the game."},
        {"Display", {"Fullscreen", "Window"}, "Switch between fullscreen and a window."},
        {"Frame rate", {"60 fps", "30 fps (original)"},
         "60 fps uses the game's own 60 fps mode, so everything plays at the right speed. Takes "
         "effect from the next screen or match."},
        {"Screen shape", {"16:9 (correct)", "Stretch to fill"},
         "Matters on 16:10 screens like the Steam Deck: 16:9 keeps thin bars, stretch fills the "
         "screen."},
        {"Edge blur", {"Off (sharp)", "On (original)"},
         "The game's own anti-aliasing blurs every edge, and more so above 720p. Off keeps the "
         "picture sharp; On is how the console looked."},
        {"Background blur", {"Off (sharp)", "On (original)"},
         "Depth of field: the game blurs the crowd and the arena behind the wrestlers. Off keeps "
         "everything sharp; On is how the console looked."},
        {"FPS counter", {"Show", "Hide"}, "The frame rate in the corner. F2 also shows or hides it."},
        {"Sound", {"On", "Muted"}, "Mute all of the game's sound."},
        // The languages on the disc (no French on this one; src/language.cpp). UTF-8 escapes: the
        // source isn't compiled as UTF-8 everywhere.
        {"Language", {"English", "Espa\xC3\xB1ol", "Deutsch", "Italiano"},
         "The language of the game's text and menus (the commentary stays in English). It "
         "changes when the game restarts: pick one, then press A to restart now."},
        {"Keyboard controls", {"On", "Off"},
         "The keyboard works as a controller (rebind keys with F4). Not tested yet: a controller is "
         "recommended."},
        {"Controllers", {"Separate players", "All on player 1"},
         "Separate players: each controller is its own player, for local multiplayer. All on "
         "player 1: every controller controls player 1. If your controller stops responding "
         "or the game says player 2 disconnected while you play alone, choose this."},
        {"CPU priority", {"Normal", "High (busy or weak PCs)"},
         "High lets the game go first when other programs (browser, recording, downloads) want "
         "the processor too: fewer frame drops on busy or weak PCs. Those programs get less."},
        {"Check for updates", {"At startup", "Off"},
         "At startup the game asks GitHub whether a newer version of this port is out. Nothing is "
         "downloaded unless you choose Update."},
        {"Update", {}, ""},  // value and hint from the updater
        {"Close", {}, "Back to the game. Your settings are saved."},
    };
    return rows;
  }

  void Open() {
    open_ = true;
    selected_ = 0;
    values_[kResolution] = ResolutionRow(std::atoi(Get("svr_render_scale").c_str()));
    std::string api = Get("svr_graphics_api");
    std::erase(api, '"');
    values_[kGraphicsApi] = api == "d3d12" ? 1 : api == "vulkan" ? 2 : 0;
    values_[kDisplay] = hooks_.is_fullscreen && hooks_.is_fullscreen() ? 0 : 1;
    values_[kFrameRate] = Get("svr_60fps") == "true" ? 0 : 1;
    values_[kShape] = Get("bd_aspect_ratio") == "6" ? 1 : 0;
    values_[kEdgeBlur] = Get("svr_edge_blur") == "true" ? 1 : 0;
    values_[kDepthOfField] = Get("svr_depth_of_field") == "true" ? 1 : 0;
    values_[kCounter] = hooks_.fps_counter_on && hooks_.fps_counter_on() ? 0 : 1;
    values_[kSound] = Get("audio_mute") == "true" ? 1 : 0;
    values_[kLanguage] = LanguageRow(std::atoi(Get("user_language").c_str()));
    values_[kKeyboard] = Get("mnk_mode") == "true" ? 0 : 1;
    values_[kControllers] = Get("svr_controllers_shared") == "true" ? 1 : 0;
    values_[kPriority] = Get("svr_high_priority") == "true" ? 1 : 0;
    values_[kCheckUpdates] = Get("svr_check_updates") == "false" ? 1 : 0;
    confirm_update_ = false;
  }
  void Close() {
    open_ = false;
    hold_ = true;
  }

  static std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }

  // svr_render_scale for each Resolution choice: a multiple of 720p, or a height (480, 1080).
  static constexpr int kResolutionValues[] = {0, 480, 1, 1080, 2, 3, 6};
  static int ResolutionRow(int value) {
    const int height = value <= 0 ? 0 : value <= 8 ? 720 * value : value;
    int best = 0;
    for (int i = 1; i < int(std::size(kResolutionValues)); ++i) {
      const int v = kResolutionValues[i];
      const int h = v <= 8 ? 720 * v : v;
      const int b = kResolutionValues[best] <= 8 ? 720 * kResolutionValues[best]
                                                 : kResolutionValues[best];
      if (height && (best == 0 || std::abs(h - height) < std::abs(b - height)))
        best = i;
    }
    return best;  // 0 (Auto) when unset
  }

  // Xbox 360 language IDs, in the order of the Language row: English, Spanish, German, Italian.
  static constexpr int kLanguageIds[] = {1, 5, 3, 6};
  static int LanguageRow(int id) {
    for (int i = 0; i < int(std::size(kLanguageIds)); ++i)
      if (kLanguageIds[i] == id)
        return i;
    return 0;  // anything else (set by hand): shown as English until changed here
  }

  void Apply(int row) {
    const int v = values_[row];
    switch (row) {
      case kResolution:
        Set("svr_render_scale", std::to_string(kResolutionValues[v]));
        break;
      case kEdgeBlur:
        Set("svr_edge_blur", v == 1 ? "true" : "false");
        break;
      case kDepthOfField:
        Set("svr_depth_of_field", v == 1 ? "true" : "false");
        break;
      case kGraphicsApi: {
        // A string setting: quoted in the toml file.
        const std::string api = v == 1 ? "d3d12" : v == 2 ? "vulkan" : "auto";
        rex::cvar::SetFlagByName("svr_graphics_api", api);
        Save("svr_graphics_api", "\"" + api + "\"");
        break;
      }
      case kDisplay:
        if (hooks_.set_fullscreen)
          hooks_.set_fullscreen(v == 0);
        Save("fullscreen", v == 0 ? "true" : "false");
        break;
      case kFrameRate:
        Set("svr_60fps", v == 0 ? "true" : "false");
        break;
      case kShape:
        Set("bd_aspect_ratio", v == 1 ? "6" : "5");
        break;
      case kCounter:
        if (hooks_.set_fps_counter)
          hooks_.set_fps_counter(v == 0);
        Save("svr_fps_counter", v == 0 ? "true" : "false");
        break;
      case kSound:
        Set("audio_mute", v == 1 ? "true" : "false");
        break;
      case kLanguage:
        // The console's language setting: the game picks its text by it when it starts.
        Set("user_language", std::to_string(kLanguageIds[v]));
        break;
      case kKeyboard:
        Set("mnk_mode", v == 0 ? "true" : "false");
        break;
      case kControllers:
        Set("svr_controllers_shared", v == 1 ? "true" : "false");
        if (hooks_.set_controllers_shared)
          hooks_.set_controllers_shared(v == 1);
        break;
      case kPriority:
        Set("svr_high_priority", v == 1 ? "true" : "false");
        if (hooks_.set_high_priority)
          hooks_.set_high_priority(v == 1);
        break;
      case kCheckUpdates:
        Set("svr_check_updates", v == 0 ? "true" : "false");
        if (v == 0)
          if (svr::Updater* u = Updater(); u && u->state() == svr::Updater::State::kIdle)
            u->StartCheck();
        break;
    }
  }

  svr::Updater* Updater() const { return hooks_.updater ? hooks_.updater() : nullptr; }

  // The Update row: check, confirm, install, restart.
  void UpdateAction() {
    svr::Updater* u = Updater();
    if (!u)
      return;
    using S = svr::Updater::State;
    switch (u->state()) {
      case S::kIdle:
      case S::kUpToDate:
      case S::kCheckFailed:
      case S::kFailed:
        u->StartCheck();
        break;
      case S::kAvailable:
        if (!confirm_update_) {
          confirm_update_ = true;
        } else {
          confirm_update_ = false;
          u->StartInstall();
        }
        break;
      case S::kInstalled:
        if (u->Restart()) {
          if (hooks_.quit)
            hooks_.quit();
        } else {
          restart_failed_ = true;
        }
        break;
      default:
        break;
    }
  }

  // The language takes effect when the game starts: once a different one is picked, A asks to
  // restart and a second A restarts the game (with the updater's restart: same command line).
  bool LanguagePending() const {
    return std::atoi(Get("user_language").c_str()) != start_language_;
  }
  void LanguageAction() {
    if (!LanguagePending())
      return;
    if (!confirm_restart_) {
      confirm_restart_ = true;
      return;
    }
    confirm_restart_ = false;
    svr::Updater* u = Updater();
    if (u && u->Restart()) {
      if (hooks_.quit)
        hooks_.quit();
    } else {
      language_restart_failed_ = true;
    }
  }
  // The restart prompts in the language being switched to, so players can read them.
  struct LanguageText {
    int id;  // Xbox language id
    const char* restart_now;
    const char* confirm_value;  // short: it replaces the row's value
    const char* confirm_hint;
    const char* failed;
  };
  static const LanguageText& TextFor(int id) {
    static const LanguageText texts[] = {
        {1,
         "Press A to restart the game now in English (anything you haven't saved is lost), or "
         "it changes the next time you start the game.",
         "Press A again to restart",
         "Press A again to restart the game now in English. Anything you haven't saved in the "
         "game is lost.",
         "The game couldn't restart itself: close it and start it again to play in English."},
        {5,
         "Pulsa A para reiniciar el juego ahora en espa\xC3\xB1ol (se perder\xC3\xA1 lo que no "
         "hayas guardado), o el cambio se aplicar\xC3\xA1 la pr\xC3\xB3xima vez que inicies el "
         "juego.",
         "Pulsa A otra vez",
         "Pulsa A otra vez para reiniciar el juego ahora en espa\xC3\xB1ol. Se perder\xC3\xA1 "
         "lo que no hayas guardado.",
         "El juego no pudo reiniciarse: ci\xC3\xA9rralo y vuelve a abrirlo para jugar en espa\xC3\xB1ol."},
        {4,
         "Appuyez sur A pour red\xC3\xA9marrer le jeu maintenant en fran\xC3\xA7"
         "ais (tout ce qui n'est pas sauvegard\xC3\xA9 sera perdu), ou le changement s'appliquera "
         "au prochain lancement du jeu.",
         "Appuyez encore sur A",
         "Appuyez encore sur A pour red\xC3\xA9marrer le jeu maintenant en fran\xC3\xA7"
         "ais. Tout ce qui n'est pas sauvegard\xC3\xA9 sera perdu.",
         "Le jeu n'a pas pu red\xC3\xA9marrer : fermez-le et relancez-le pour jouer en fran\xC3\xA7"
         "ais."},
        {3,
         "Dr\xC3\xBC"
         "cke A, um das Spiel jetzt auf Deutsch neu zu starten (nicht Gespeichertes geht verloren), "
         "oder die Sprache wechselt beim n\xC3\xA4"
         "chsten Spielstart.",
         "Erneut A dr\xC3\xBC"
         "cken",
         "Dr\xC3\xBC"
         "cke erneut A, um das Spiel jetzt auf Deutsch neu zu starten. Nicht Gespeichertes geht "
         "verloren.",
         "Das Spiel konnte nicht neu starten: Schlie\xC3\x9F"
         "e es und starte es erneut, um auf Deutsch zu spielen."},
        {6,
         "Premi A per riavviare subito il gioco in italiano (ci\xC3\xB2 che non hai salvato andr\xC3\xA0 "
         "perso), oppure la lingua cambier\xC3\xA0 al prossimo avvio del gioco.",
         "Premi di nuovo A",
         "Premi di nuovo A per riavviare subito il gioco in italiano. Ci\xC3\xB2 che non hai salvato "
         "andr\xC3\xA0 perso.",
         "Il gioco non \xC3\xA8 riuscito a riavviarsi: chiudilo e riaprilo per giocare in italiano."},
    };
    for (const LanguageText& t : texts)
      if (t.id == id)
        return t;
    return texts[0];
  }

  const LanguageText& PickedLanguageText() const { return TextFor(kLanguageIds[values_[kLanguage]]); }
  std::string LanguageHint() const {
    if (!LanguagePending())
      return Rows()[kLanguage].hint;
    const LanguageText& t = PickedLanguageText();
    return language_restart_failed_ ? t.failed : confirm_restart_ ? t.confirm_hint : t.restart_now;
  }

  // A row's current choice; Auto says what it picked here ("Auto (1440p)").
  std::string ValueText(int row) const {
    const std::string label = Rows()[row].labels[values_[row]];
    if (values_[row] == 0) {
      if (row == kResolution && hooks_.auto_resolution)
        return "Auto (" + hooks_.auto_resolution() + ")";
      if (row == kGraphicsApi && hooks_.auto_graphics_api)
        return "Auto (" + hooks_.auto_graphics_api() + ")";
    }
    return label;
  }

  std::string UpdateValue() const {
    svr::Updater* u = Updater();
    if (!u)
      return "Not available";
    using S = svr::Updater::State;
    switch (u->state()) {
      case S::kIdle: return "Check now";
      case S::kChecking: return "Checking...";
      case S::kUpToDate: return "Up to date";
      case S::kAvailable:
        return confirm_update_ ? "Press again to install" : "Install version " + u->latest_version();
      case S::kCheckFailed: return "Couldn't check: try again";
      case S::kWorking: return "Updating... " + std::to_string(int(u->progress() * 100)) + "%";
      case S::kInstalled: return restart_failed_ ? "Close and restart the game" : "Restart now";
      case S::kFailed: return "Update stopped: try again";
    }
    return "";
  }

  std::string UpdateHint() const {
    svr::Updater* u = Updater();
    if (!u)
      return "Updates can be checked once the game has started.";
    using S = svr::Updater::State;
    const std::string status = u->status();
    switch (u->state()) {
      case S::kAvailable:
        return confirm_update_
                   ? "Press again to download and install version " + u->latest_version() +
                         " now. Your saves are copied to the save_backups folder first, and your "
                         "saves, settings and disc image are kept."
                   : status + " You have " SVR_VERSION ". Updating keeps your saves, settings "
                              "and disc image, and backs up your saves first.";
      case S::kWorking:
        return status + " Please don't close the game.";
      case S::kInstalled:
        return restart_failed_ ? status + " The game couldn't restart itself: close it and start "
                                          "it again."
                               : status;
      default:
        return status.empty() ? "Check GitHub for a newer version of this port." : status;
    }
  }

  // A short notice when a newer version is found, while the menu is closed.
  void DrawUpdateNotice(ImGuiIO& io) {
    svr::Updater* u = Updater();
    // Developer aid: SVR_UPDATE_AUTO=1 installs an available update without the menu (tests of
    // the updater), =2 also restarts into it.
    static const int auto_update = [] {
      const char* v = std::getenv("SVR_UPDATE_AUTO");
      return v ? std::atoi(v) : 0;
    }();
    if (u && auto_update > 0) {
      if (u->state() == svr::Updater::State::kAvailable)
        u->StartInstall();
      else if (auto_update > 1 && u->state() == svr::Updater::State::kInstalled && u->Restart() &&
               hooks_.quit)
        hooks_.quit();
    }
    if (!u || u->state() != svr::Updater::State::kAvailable)
      return;
    const auto now = Clock::now();
    if (!notice_shown_) {
      notice_shown_ = true;
      notice_until_ = now + std::chrono::seconds(7);
    }
    if (now >= notice_until_)
      return;
    const float s = std::max(0.6f, io.DisplaySize.y / 1080.0f);
    std::string line1 = "Update available: version " + u->latest_version();
    if (const std::string headline = u->headline(); !headline.empty())
      line1 += " - " + headline;
    const char* line2 = "Open Settings (F1, or Back + Start) to see what's new and install it";
    ImFont* font = g_menu_font ? g_menu_font : ImGui::GetFont();
    const float size1 = 18 * s, size2 = 13 * s;
    const ImVec2 t1 = font->CalcTextSizeA(size1, FLT_MAX, 0.0f, line1.c_str());
    const ImVec2 t2 = font->CalcTextSizeA(size2, FLT_MAX, 0.0f, line2);
    const float pad = 16 * s;
    const ImVec2 p0(24 * s, io.DisplaySize.y - 24 * s - (t1.y + t2.y + pad * 2 + 6 * s));
    const ImVec2 p1(p0.x + std::max(t1.x, t2.x) + pad * 2, io.DisplaySize.y - 24 * s);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(18, 21, 30, 235), 10 * s);
    dl->AddRectFilled(p0, ImVec2(p0.x + 5 * s, p1.y), IM_COL32(214, 38, 38, 255), 3 * s);
    dl->AddText(font, size1, ImVec2(p0.x + pad, p0.y + pad), IM_COL32(236, 238, 244, 255),
                line1.c_str());
    dl->AddText(font, size2, ImVec2(p0.x + pad, p0.y + pad + t1.y + 6 * s),
                IM_COL32(178, 184, 198, 255), line2);
  }
  void Set(const char* name, const std::string& value) {
    rex::cvar::SetFlagByName(name, value);
    Save(name, value);
  }

  // Replaces (or appends) one "key = value" line, keeping everything else in the file.
  void Save(const char* key, const std::string& value) {
    std::vector<std::string> lines;
    {
      std::ifstream in(toml_);
      std::string line;
      while (std::getline(in, line))
        lines.push_back(line);
    }
    const std::string assignment = std::string(key) + " = " + value;
    bool replaced = false;
    for (std::string& line : lines) {
      const size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line.compare(start, std::strlen(key), key) != 0)
        continue;
      const size_t after = line.find_first_not_of(" \t", start + std::strlen(key));
      if (after != std::string::npos && line[after] == '=') {
        line = assignment;
        replaced = true;
      }
    }
    if (!replaced)
      lines.push_back(assignment);
    std::ofstream out(toml_, std::ios::trunc);
    for (const std::string& line : lines)
      out << line << "\n";
  }

  void Change(int delta) {
    if (selected_ >= kUpdate)
      return;
    confirm_restart_ = false;
    const int count = int(Rows()[selected_].labels.size());
    values_[selected_] = (values_[selected_] + delta + count) % count;
    Apply(selected_);
  }

  // Controller state from the game's own input (every pad SDL sees: PlayStation, Switch, Xbox),
  // and from XInput (also what Proton exposes on the Steam Deck).
  Pad ReadPad() {
    Pad p;
    constexpr int kDead = 16000;
    auto add = [&p](uint16_t b, int lx, int ly) {
      // XInput's button bits: DPAD up/down/left/right 0x1/0x2/0x4/0x8, START 0x10, BACK 0x20,
      // A 0x1000, B 0x2000.
      p.up |= (b & 0x0001) || ly > kDead;
      p.down |= (b & 0x0002) || ly < -kDead;
      p.left |= (b & 0x0004) || lx < -kDead;
      p.right |= (b & 0x0008) || lx > kDead;
      p.a |= (b & 0x1000) != 0;
      p.b |= (b & 0x2000) != 0;
      p.combo |= (b & 0x0020) && (b & 0x0010);
      p.any |= b != 0 || ly > kDead || ly < -kDead || lx > kDead || lx < -kDead;
    };
    if (hooks_.read_pad) {
      uint16_t b = 0;
      int16_t lx = 0, ly = 0;
      if (hooks_.read_pad(b, lx, ly))
        add(b, lx, ly);
    }
#if defined(_WIN32)
    if (xinput_get_state_) {
      for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st{};
        if (xinput_get_state_(i, &st) == ERROR_SUCCESS)
          add(st.Gamepad.wButtons, st.Gamepad.sThumbLX, st.Gamepad.sThumbLY);
      }
    }
#endif
    return p;
  }

  // A held direction fires once, then repeats after a short delay.
  bool Fire(bool now, bool before, Clock::time_point& next) {
    const auto t = Clock::now();
    if (now && !before) {
      next = t + std::chrono::milliseconds(380);
      return true;
    }
    if (now && t >= next) {
      next = t + std::chrono::milliseconds(120);
      return true;
    }
    return false;
  }

  void HandleInput(const Pad& pad) {
    const bool up = Fire(pad.up, last_.up, rep_[0]) || ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    const bool down = Fire(pad.down, last_.down, rep_[1]) || ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    const bool left = Fire(pad.left, last_.left, rep_[2]) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    const bool right = Fire(pad.right, last_.right, rep_[3]) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    const bool accept = (pad.a && !last_.a) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_Space, false);
    const bool back = (pad.b && !last_.b) || (pad.combo && !last_.combo) ||
                      ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (up || down || left || right)
      confirm_restart_ = false;
    if (up || down)
      confirm_update_ = false;
    if (up)
      selected_ = (selected_ + kRows - 1) % kRows;
    if (down)
      selected_ = (selected_ + 1) % kRows;
    if (left)
      Change(-1);
    if (right)
      Change(+1);
    if (accept) {
      if (selected_ == kClose)
        Close();
      else if (selected_ == kUpdate)
        UpdateAction();
      else if (selected_ == kLanguage)
        LanguageAction();
      else
        Change(+1);
    }
    if (back)
      Close();
  }

  // The update's notes (Updater::notes), as much as fits in `height`.
  void DrawUpdateNotes(ImDrawList* dl, float inner, float height, float s, ImU32 text, ImU32 dim) {
    const std::string notes = Updater()->notes();
    const float top = ImGui::GetCursorPosY();
    ImGui::PushFont(g_menu_font, 18 * s);
    ImGui::TextColored(ImColor(text), "WHAT'S NEW");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6 * s));
    ImGui::PushFont(g_menu_font, 13 * s);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    std::istringstream in(notes);
    std::string line;
    bool cut = false;
    while (std::getline(in, line)) {
      if (ImGui::GetCursorPosY() - top > height - 3 * ImGui::GetTextLineHeightWithSpacing()) {
        cut = true;
        break;
      }
      if (line.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * s));
      } else if (line.rfind("# ", 0) == 0) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextColored(ImColor(IM_COL32(255, 255, 255, 255)), "%s", line.c_str() + 2);
      } else if (line.rfind("- ", 0) == 0) {
        ImGui::Indent(16 * s);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(pos.x - 9 * s, pos.y + ImGui::GetTextLineHeight() * 0.5f),
                            2.5f * s, dim);
        ImGui::PushStyleColor(ImGuiCol_Text, dim);
        ImGui::TextWrapped("%s", line.c_str() + 2);
        ImGui::PopStyleColor();
        ImGui::Unindent(16 * s);
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, dim);
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PopStyleColor();
      }
    }
    if (cut)
      ImGui::TextColored(ImColor(text), "...and more: see the releases on GitHub.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 10 * s));
  }

  void Draw(ImGuiIO& io) {
    const float s = std::max(0.6f, io.DisplaySize.y / 1080.0f);
    const ImU32 accent = IM_COL32(214, 38, 38, 255);
    const ImU32 text = IM_COL32(236, 238, 244, 255);
    const ImU32 dim = IM_COL32(150, 156, 172, 255);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));

    const float width = 900 * s;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30 * s, 26 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 21, 30, 245));
    ImGui::PushFont(g_menu_font, 16 * s);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##svr_settings_menu", nullptr, kFlags)) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImGui::PushFont(g_menu_font, 30 * s);
      ImGui::TextColored(ImColor(IM_COL32(255, 255, 255, 255)), "SETTINGS");
      ImGui::PopFont();
      ImGui::PushFont(g_menu_font, 13 * s);
      ImGui::TextColored(ImColor(dim), "%s", title_.c_str());
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(0, 12 * s));

      const float row_h = 40 * s;
      const float inner = ImGui::GetContentRegionAvail().x;
      const float value_x = 340 * s;
      // After the first press on Update: what the update changes, in place of the other rows,
      // with the Update row (press again to install) under it.
      const bool notes_page = selected_ == kUpdate && confirm_update_ && Updater() &&
                              Updater()->state() == svr::Updater::State::kAvailable &&
                              !Updater()->notes().empty();
      if (notes_page)
        DrawUpdateNotes(dl, inner, row_h * (kRows - 2), s, text, dim);
      for (int i = 0; i < kRows; ++i) {
        if (notes_page && i != kUpdate)
          continue;
        const Row& row = Rows()[i];
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + inner, p0.y + row_h);
        ImGui::PushID(i);
        ImGui::InvisibleButton("row", ImVec2(inner, row_h));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          if (selected_ != i)
            confirm_update_ = confirm_restart_ = false;
          selected_ = i;
          if (i == kClose)
            Close();
          else if (i == kUpdate)
            UpdateAction();
          else
            Change(io.MousePos.x < p0.x + value_x + (inner - value_x) * 0.5f ? -1 : +1);
        } else if (hovered && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
          if (selected_ != i)
            confirm_update_ = confirm_restart_ = false;
          selected_ = i;
        }
        ImGui::PopID();
        const bool sel = i == selected_;
        if (sel) {
          dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 16), 8 * s);
          dl->AddRectFilled(p0, ImVec2(p0.x + 5 * s, p1.y), accent, 3 * s);
        }
        const float cy = p0.y + row_h * 0.5f;
        const float fh = ImGui::GetFontSize();
        if (i == kClose) {
          const char* label = "Close";
          const float tw = ImGui::CalcTextSize(label).x;
          dl->AddText(ImVec2(p0.x + (inner - tw) * 0.5f, cy - fh * 0.5f), sel ? text : dim, label);
          continue;
        }
        dl->AddText(ImVec2(p0.x + 20 * s, cy - fh * 0.5f), sel ? text : dim, row.name);
        if (i == kUpdate) {  // a button, not a choice: no arrows
          const std::string value = UpdateValue();
          const float vw = ImGui::CalcTextSize(value.c_str()).x;
          const float mid = p0.x + value_x + (inner - value_x) * 0.5f;
          const bool ready = Updater() && (Updater()->state() == svr::Updater::State::kAvailable ||
                                           Updater()->state() == svr::Updater::State::kInstalled);
          dl->AddText(ImVec2(mid - vw * 0.5f, cy - fh * 0.5f),
                      ready ? accent : (sel ? IM_COL32(255, 255, 255, 255) : text), value.c_str());
          continue;
        }
        const bool restart = i == kLanguage && LanguagePending();
        const std::string value =
            restart && confirm_restart_ ? std::string(PickedLanguageText().confirm_value) : ValueText(i);
        const float vw = ImGui::CalcTextSize(value.c_str()).x;
        const float mid = p0.x + value_x + (inner - value_x) * 0.5f;
        dl->AddText(ImVec2(mid - vw * 0.5f, cy - fh * 0.5f),
                    restart ? accent : (sel ? IM_COL32(255, 255, 255, 255) : text), value.c_str());
        const ImU32 arrow = sel ? accent : IM_COL32(90, 96, 112, 255);
        const float ax = 9 * s, ay = 8 * s;
        const float lx = p0.x + value_x + 10 * s, rx = p1.x - 18 * s;
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + ax, cy - ay), ImVec2(lx + ax, cy + ay), arrow);
        dl->AddTriangleFilled(ImVec2(rx + ax, cy), ImVec2(rx, cy - ay), ImVec2(rx, cy + ay), arrow);
      }

      ImGui::Dummy(ImVec2(0, 10 * s));
      ImGui::PushFont(g_menu_font, 13 * s);
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(178, 184, 198, 255));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
      const float hint_y = ImGui::GetCursorPosY();
      if (selected_ == kUpdate)
        ImGui::TextWrapped("%s", UpdateHint().c_str());
      else
        ImGui::TextWrapped("%s", selected_ == kLanguage ? LanguageHint().c_str()
                                                        : Rows()[selected_].hint);
      ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), hint_y + 3 * ImGui::GetTextLineHeightWithSpacing()));
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
      ImGui::Dummy(ImVec2(0, 6 * s));
      ImGui::TextColored(ImColor(dim), "Up/Down choose   Left/Right change   B/Esc close");
      ImGui::PopFont();
      // The credit: a divider, then the line larger and with a drop shadow, centred.
      ImGui::Dummy(ImVec2(0, 12 * s));
      const ImVec2 line0 = ImGui::GetCursorScreenPos();
      dl->AddLine(line0, ImVec2(line0.x + inner, line0.y), IM_COL32(255, 255, 255, 40), 1.5f * s);
      ImGui::Dummy(ImVec2(0, 14 * s));
      ImGui::PushFont(g_menu_font, 20 * s);
      const char* credit = "PC port made by nefariousjosiah";
      const ImVec2 cs = ImGui::CalcTextSize(credit);
      const ImVec2 cp(ImGui::GetCursorScreenPos().x + (inner - cs.x) * 0.5f, ImGui::GetCursorScreenPos().y);
      dl->AddText(ImVec2(cp.x + 2 * s, cp.y + 2 * s), IM_COL32(0, 0, 0, 200), credit);
      dl->AddText(cp, IM_COL32(255, 255, 255, 255), credit);
      ImGui::Dummy(ImVec2(inner, cs.y + 2 * s));
      ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
  }

#if defined(_WIN32)
  using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
  XInputGetStateFn xinput_get_state_ = nullptr;
#endif
  std::string title_;
  std::filesystem::path toml_;
  Hooks hooks_;
  bool open_ = false, hold_ = false;
  bool confirm_update_ = false, restart_failed_ = false, notice_shown_ = false;
  bool confirm_restart_ = false, language_restart_failed_ = false;
  // The language the game started in (the setting is read when the game starts).
  const int start_language_ = std::atoi(rex::cvar::GetFlagByName("user_language").c_str());
  Clock::time_point notice_until_;
  int selected_ = 0;
  int values_[kRows] = {};
  Pad last_;
  Clock::time_point rep_[4];
};
