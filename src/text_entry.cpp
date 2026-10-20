// Name entry on the Steam Deck (issue #7).
//
// SvR 2008's text entry (Create-a-Superstar name and the like) is the game's own on-screen
// keyboard, played with the controller. On the Steam Deck, typing with Steam's keyboard sends
// key presses, and keyboard controls (mnk_mode, on by default) turn keys into controller buttons:
// Backspace is B and X / Enter are Start, so the entry closed while the player was typing.
//
// On the Deck, while the entry is open, key presses don't reach keyboard controls: names are
// entered with the Deck's controls on the in-game keyboard. On PC nothing changes (players
// without a controller enter names through keyboard controls).

#include <rex/logging.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

using rex::ui::VirtualKey;

constexpr int64_t kClosedAfterMs = 250;  // the entry stopped polling for this long: closed

bool g_deck = false;
std::atomic<bool> g_open{false};
std::atomic<int64_t> g_last_poll_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Above keyboard controls (z 0) and below the ImGui overlays (z 64, the settings menu).
class TextEntryListener : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!g_open.load())
      return;
    const VirtualKey vk = e.virtual_key();
    // Function keys (F1 settings menu, F2 FPS counter...) and Alt shortcuts stay with their binds.
    if ((vk >= VirtualKey::kF1 && vk <= VirtualKey::kF24) || e.is_alt_pressed())
      return;
    e.set_handled(true);  // not a controller button while the entry is open
  }
  // Key releases still reach keyboard controls, so a key held when the entry opened is let go.
};

TextEntryListener g_listener;

}  // namespace

void SvrTextEntryAttach(rex::ui::Window* window) {
  // Steam sets SteamDeck=1 on the Deck, and Proton passes it through to the game.
  const char* deck = std::getenv("SteamDeck");
  g_deck = deck && std::strcmp(deck, "1") == 0;
  if (window && g_deck)
    window->AddInputListener(&g_listener, 32);
}

// sub_824C66D0 polls the keyboard every frame the text entry is open (0x824C6708, after the
// XInputGetKeystroke call).
void SvrTextEntryPollHook() {
  if (!g_deck)
    return;
  g_last_poll_ms.store(NowMs());
  if (!g_open.exchange(true))
    REXLOG_INFO("[text entry] opened: keys are not controller buttons until it closes");
}

// Once per presented frame (SvrFrameSwapHook): the entry stopped polling, so it was closed.
void SvrTextEntryFrame() {
  if (g_open.load() && NowMs() - g_last_poll_ms.load() >= kClosedAfterMs &&
      g_open.exchange(false))
    REXLOG_INFO("[text entry] closed: keyboard controls back");
}
