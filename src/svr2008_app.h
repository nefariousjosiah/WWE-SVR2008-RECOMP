// svr2008 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/input/input_system.h>
#include <rex/ui/keybinds.h>

#include <atomic>
#include <filesystem>
#include <memory>

#include "fps_counter.h"
#include "game_locator.h"
#include "settings_menu.h"
#include "graphics_api.h"
#if defined(SVR_NATIVE_RENDERER)
#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"
#endif
#include "frame_dump.h"
#include "frame_stats.h"
#include "image_dump.h"

REXCVAR_DECLARE(std::string, gpu_backend);
REXCVAR_DECLARE(bool, svr_fps_counter);
REXCVAR_DECLARE(bool, svr_check_updates);
REXCVAR_DECLARE(bool, svr_controllers_shared);
REXCVAR_DECLARE(bool, svr_high_priority);

void SvrTextEntryAttach(rex::ui::Window* window);  // text_entry.cpp: typing names on a keyboard

#ifndef SVR_VERSION
#define SVR_VERSION "dev"  // set by CMakeLists.txt (project VERSION)
#endif

inline constexpr const char* kWindowTitle = "WWE SmackDown vs. Raw 2008";

// Set while the settings menu reads the pad through the game's input system (UI thread only), so
// the input gate lets that read through.
inline thread_local bool t_overlay_reads_pad = false;

#if defined(SVR_NATIVE_RENDERER)
namespace bd::gpu {
uint32_t SvrAutoRenderScale();  // src/native/svr_resources.h
void SvrDrawLogNextFrame();     // src/native/svr_draw_log.h
}
#endif

// Above-normal process priority (settings menu > CPU priority): the game's threads go first when
// other programs want the processor, which keeps entrances smooth on busy or weak PCs.
inline void SvrSetHighPriority(bool on) {
#if defined(_WIN32)
  SetPriorityClass(GetCurrentProcess(), on ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
#endif
  REXLOG_INFO("CPU priority: {}", on ? "high (above normal)" : "normal");
}

class Svr2008App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Svr2008App>(new Svr2008App(ctx, "svr2008",
        PPCImageConfig));
  }

  // Render through the Xenos GPU plugin unless --gpu_plugin says otherwise.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Files a finished update set aside (updater.h); after a restart from the updater, also waits
    // for the previous version to exit.
    svr::Updater::CleanUp(rex::filesystem::GetExecutableFolder());
    if (config.gpu_plugin.empty())
      config.gpu_plugin = "xenos";
#if defined(SVR_NATIVE_RENDERER)
    // Native renderer: the Xenos plugin's null backend keeps the game's D3D command stream moving
    // (fences, interrupts, vblank) without drawing; the hooks in src/reblue draw through plume.
    if (!config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "null");
      REXLOG_INFO("Native renderer: guest GPU {}", config.graphics ? "null (sync only)" : "MISSING");
    }
    return;
#endif
    // ReXApp only asks the plugin for "any" backend (D3D12 first on Windows); --gpu_backend picks one.
    const std::string& backend = REXCVAR_GET(gpu_backend);
    if (!config.graphics && !backend.empty()) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (config.graphics)
        REXLOG_INFO("GPU backend: {} (from --gpu_backend)", backend);
    }
  }

#if defined(SVR_NATIVE_RENDERER)
  // The plume device draws the ImGui overlays (FPS counter etc.) at present time.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    auto drawer = std::make_unique<bd::gpu::ImGuiOverlayDrawer>();
    overlay_drawer_ = drawer.get();
    return drawer;
  }

  void OnPreLaunchModule() override {
    if (!bd::gpu::Video::CreateHostDevice(window())) {
      REXLOG_ERROR("Native renderer: host device creation failed");
#if defined(_WIN32)
      // No working device on this API (an old graphics card or driver): try the other program.
      DWORD unused = 0;
      if (svr::StartOtherGraphicsApi(false, unused))
        REXLOG_INFO("Graphics API: started the {} program instead",
                    svr::kThisIsD3D12 ? "Vulkan" : "Direct3D 12");
#endif
      app_context().QuitFromUIThread();
      return;
    }
    // Present runs on the guest thread and ImGui on the UI thread. Waiting for the UI thread to
    // draw the overlay (FPS counter, settings) cost the game thread a few ms every frame, so
    // the UI thread records the overlay in the background and Present replays the latest
    // recording (ImGuiOverlayDrawer::Capture). Only the first frame, a resize or a new font
    // texture still draws directly on the UI thread.
    bd::gpu::Video::SetOverlayDrawHook([this](plume::RenderCommandList* cmd,
                                              plume::RenderFramebuffer* fb, uint32_t w,
                                              uint32_t h) {
      auto* drawer = overlay_drawer_;
      if (!imgui_drawer() || !imgui_drawer()->HasDialogs()) {
        if (drawer)
          drawer->ClearCapture();
        return;
      }
      if (!drawer || drawer->NeedsDirectDraw() || !drawer->ReplayLatest(cmd, fb, w, h)) {
        app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
          bd::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
          imgui_drawer()->Draw(ctx);
        });
      }
      if (drawer && !overlay_capture_pending_.exchange(true)) {
        app_context().CallInUIThread([this, w, h] {
          if (imgui_drawer() && overlay_drawer_)
            overlay_drawer_->Capture(
                w, h, [this](rex::ui::UIDrawContext& ctx) { imgui_drawer()->Draw(ctx); });
          overlay_capture_pending_.store(false);
        });
      }
    });
  }

  void OnWindowPixelSizeChanged(uint32_t, uint32_t) override { bd::gpu::Video::RequestResize(); }
#endif

  void OnPostSetup() override {
    // Replaces the SDK's "svr2008 [rexglue-<build>]" title set during window creation.
    if (window()) {
      window()->SetTitle(kWindowTitle);
      SvrTextEntryAttach(window());
    }
#if defined(_WIN32)
    REXLOG_INFO("Graphics API: {}", svr::GraphicsApiDescription());
#endif
    frame_dumper_.Start(runtime()->graphics_system());
    if (REXCVAR_GET(svr_high_priority))
      SvrSetHighPriority(true);
    // The game gets no input while the settings menu is open (or while the mouse is over an
    // overlay). Set here: the runtime (and ReXApp's own callback) exists only from now on.
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
      input->SetActiveCallback([this] {
        // The settings menu reading the pad (hooks.read_pad) sees it even while the game can't.
        if (t_overlay_reads_pad)
          return true;
        if (settings_menu_ && settings_menu_->BlocksGameInput())
          return false;
        return !(imgui_drawer_ && imgui_drawer_->GetIO().WantCaptureMouse);
      });
    }
    SetControllersShared(REXCVAR_GET(svr_controllers_shared));
    // Updates (settings menu > Update): a check at startup, installed only when the player says.
    svr::Updater::Config update;
    update.repo = "nefariousjosiah/WWE-SVR2008-RECOMP";
    update.asset = "SVR2008-NATIVE.zip";
    update.exe = "svr2008.exe";
    update.settings_file = "svr2008.toml";
    update.current_version = SVR_VERSION;
    update.game_dir = rex::filesystem::GetExecutableFolder();
    update.user_data = runtime()->user_data_root();
    updater_ = std::make_unique<svr::Updater>(std::move(update));
    if (REXCVAR_GET(svr_check_updates))
      updater_->StartCheck();
  }
  // Settings menu > Controllers. All on player 1: every controller (and the keyboard) drives
  // player 1 and players 2-4 read as not connected, so a controller that reconnects, or shows up
  // twice (Steam Input, DS4Windows...), never ends up as a player 2 the game waits for.
  void SetControllersShared(bool shared) {
    auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system())
                            : nullptr;
    if (!input)
      return;
    if (shared)
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
    else
      input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
    REXLOG_INFO("Input: controllers {}", shared ? "all on player 1" : "separate players");
  }

  void OnShutdown() override {
#if defined(SVR_NATIVE_RENDERER)
    overlay_drawer_ = nullptr;
#endif
    rex::ui::UnregisterBind("bind_fps_counter");
    rex::ui::UnregisterBind("bind_svr_settings");
    rex::ui::UnregisterBind("bind_svr_drawlog");
    settings_menu_.reset();
    updater_.reset();  // stops a download in progress
    fps_counter_.reset();
    frame_dumper_.Stop();
  }

  // Press Start 2P for the FPS counter (copied to <exe>/fonts by CMake); pixel font, so no
  // oversampling and pixel-snapped glyphs.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    auto path = rex::filesystem::GetExecutableFolder() / "fonts" / "PressStart2P-Regular.ttf";
    g_fps_counter_font = nullptr;
    g_menu_font = nullptr;
    if (!std::filesystem::exists(path)) {
      REXLOG_WARN("FPS counter font not found: {}", path.string());
      return;
    }
    ImFontConfig config;
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    g_fps_counter_font = atlas->AddFontFromFileTTF(path.string().c_str(), 20.0f, &config);
    // The settings menu uses the same pixel font (sized per use; ImGui 1.92 scales fonts
    // dynamically).
    g_menu_font = atlas->AddFontFromFileTTF(path.string().c_str(), 16.0f, &config);
  }

  // FPS counter in the corner (on by default, F2 toggles), and the same numbers for the F3 overlay.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    imgui_drawer_ = drawer;
    if (REXCVAR_GET(svr_fps_counter))
      fps_counter_ = std::make_unique<FpsCounterDialog>(drawer);
    rex::ui::RegisterBind("bind_fps_counter", "F2", "Toggle FPS counter", [this] {
      if (fps_counter_)
        fps_counter_.reset();
      else if (imgui_drawer_)
        fps_counter_ = std::make_unique<FpsCounterDialog>(imgui_drawer_);
    });
#if defined(SVR_NATIVE_RENDERER)
    // Developer aid: F9 writes every draw of the next frame to game.log ([drawlog] lines).
    rex::ui::RegisterBind("bind_svr_drawlog", "F9", "Log the next frame's draws",
                          [] { bd::gpu::SvrDrawLogNextFrame(); });
#endif
    // In-game settings menu: F1, or Back + Start on a controller (settings_menu.h).
    SettingsMenuDialog::Hooks hooks;
    hooks.is_fullscreen = [this] { return window() && window()->IsFullscreen(); };
    hooks.set_fullscreen = [this](bool on) {
      if (window())
        window()->SetFullscreen(on);
    };
    hooks.fps_counter_on = [this] { return fps_counter_ != nullptr; };
    hooks.set_fps_counter = [this](bool on) {
      if (on && !fps_counter_ && imgui_drawer_)
        fps_counter_ = std::make_unique<FpsCounterDialog>(imgui_drawer_);
      else if (!on)
        fps_counter_.reset();
    };
    hooks.read_pad = [this](uint16_t& buttons, int16_t& lx, int16_t& ly) {
      auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system())
                              : nullptr;
      if (!input)
        return false;
      bool any = false;
      t_overlay_reads_pad = true;
      for (uint32_t user = 0; user < 4; ++user) {
        rex::input::X_INPUT_STATE state{};
        if (input->GetState(user, &state) != 0)  // X_ERROR_SUCCESS
          continue;
        any = true;
        buttons |= uint16_t(state.gamepad.buttons);
        const int16_t x = state.gamepad.thumb_lx, y = state.gamepad.thumb_ly;
        if (std::abs(x) > std::abs(lx))
          lx = x;
        if (std::abs(y) > std::abs(ly))
          ly = y;
      }
      t_overlay_reads_pad = false;
      return any;
    };
    hooks.set_controllers_shared = [this](bool on) { SetControllersShared(on); };
    hooks.set_high_priority = [](bool on) { SvrSetHighPriority(on); };
#if defined(SVR_NATIVE_RENDERER)
    hooks.auto_resolution = [] {
      static constexpr const char* kNames[] = {"720p", "720p", "1440p", "4K", "2880p"};
      return std::string(kNames[std::min(bd::gpu::SvrAutoRenderScale(), 4u)]);
    };
#endif
#if defined(_WIN32)
    hooks.auto_graphics_api = [] {
      return std::string(svr::WantsVulkan("auto") ? "Vulkan" : "Direct3D 12");
    };
#endif
    hooks.updater = [this] { return updater_.get(); };
    hooks.quit = [this] { app_context().QuitFromUIThread(); };
    settings_menu_ = std::make_unique<SettingsMenuDialog>(
        drawer, "WWE SmackDown vs. Raw 2008   v" SVR_VERSION,
        rex::filesystem::GetExecutableFolder() / "svr2008.toml", std::move(hooks));
    rex::ui::RegisterBind("bind_svr_settings", "F1", "Open the settings menu", [this] {
      if (settings_menu_)
        settings_menu_->Toggle();
    });
    SetGuestFrameStats([] {
      SvrFrameStats s = GetSvrFrameStats();
      rex::ui::FrameStats stats;
      stats.fps = s.fps;
      stats.frame_time_ms = s.frame_time_ms;
      stats.frame_count = s.frames;
      return stats;
    });
  }

  // Developer image dump for static analysis, only with SVR_DUMP_IMAGE set (image_dump.h).
  void OnPostLoadXexImage() override { DumpGuestImageIfRequested(runtime(), PPCImageConfig); }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // Portable layout: without --game_data_root the game is found next to the executable (an
  // "assets" folder or a disc image), or picked once with a file dialog (game_locator.h);
  // without --user_data_root, saves live in "userdata" beside the executable. A copied folder
  // runs with no arguments, also on Steam Deck through Proton. Settings: svr2008.toml there too.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (paths.game_data_root.empty())
      paths.game_data_root = svr::LocateGameData(exe_dir);
    if (REXCVAR_GET(user_data_root).empty()) {
      std::error_code ec;
      std::filesystem::create_directories(exe_dir / "userdata", ec);
      if (!ec) {
        paths.user_data_root = exe_dir / "userdata";
        if (REXCVAR_GET(cache_root).empty())
          paths.cache_root = paths.user_data_root / "cache";
      }
    }
  }

 private:
  FrameDumper frame_dumper_;
  rex::ui::ImGuiDrawer* imgui_drawer_ = nullptr;
  std::unique_ptr<FpsCounterDialog> fps_counter_;
  std::unique_ptr<SettingsMenuDialog> settings_menu_;
  std::unique_ptr<svr::Updater> updater_;
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::ImGuiOverlayDrawer* overlay_drawer_ = nullptr;  // owned by the ImGui drawer
  std::atomic<bool> overlay_capture_pending_{false};
#endif
};
