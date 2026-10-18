// In-game updater (settings menu > Update). At startup (svr_check_updates) it asks GitHub whether
// this project has a newer release; it installs one only when the player chooses to. The player's
// data is never written:
//   - saves: before anything changes, the saves and profiles in userdata (everything except the
//     shader cache) are copied to save_backups\<version>_<date>, and the update stops there if
//     the copy fails or doesn't match;
//   - kept as they are: userdata, save_backups, the settings file (<game>.toml), disc images and
//     pipelines.bin;
//   - the release zip is downloaded, its size checked, and every file unpacked and CRC-checked
//     into the update folder before any game file is touched; each file it replaces is renamed
//     aside first and everything is put back if a step fails;
//   - Restart() starts the new version; the files set aside are deleted at the next start.
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace svr {

class Updater {
 public:
  enum class State {
    kIdle,         // not checked (svr_check_updates off)
    kChecking,
    kUpToDate,
    kAvailable,    // a newer release: latest_version()
    kCheckFailed,  // no connection, GitHub unreachable, ...
    kWorking,      // backing up, downloading, installing: progress()
    kInstalled,    // restart to finish
    kFailed,       // the update stopped; nothing of the player's was changed
  };

  struct Config {
    std::string repo;             // "owner/name" on GitHub
    std::string asset;            // the release zip, e.g. "SVR2009-NATIVE.zip"
    std::string exe;              // e.g. "svr2009.exe"
    std::string settings_file;    // e.g. "svr2009.toml"
    std::string current_version;  // e.g. "0.4"
    std::filesystem::path game_dir;
    std::filesystem::path user_data;
  };

  explicit Updater(Config config);
  ~Updater();
  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;

  void StartCheck();    // in the background
  void StartInstall();  // in the background; only from kAvailable
  bool Restart();       // starts the installed version; the caller then quits the game

  State state() const { return state_.load(); }
  float progress() const { return progress_.load(); }  // 0..1 while kWorking
  std::string latest_version() const;
  // What the newest version changes, from its GitHub release notes, as plain
  // text: "# " starts a version's heading, "- " a point, other lines are paragraphs.
  std::string notes() const;
  // The newest version's title, e.g. "your own entrance music (User Playlist)"; may be empty.
  std::string headline() const;
  std::string status() const;  // one line for the menu
  std::string backup_folder() const;

  // Removes what a finished update leaves behind (the files it set aside, the download).
  static void CleanUp(const std::filesystem::path& game_dir);

 private:
  void Check();
  void Install();
  void SetStatus(State state, std::string status);
  bool BackUpSaves(std::string& error);
  bool Download(std::string& error);
  bool Unpack(std::string& error);
  bool Swap(std::string& error);

  Config config_;
  std::atomic<State> state_{State::kIdle};
  std::atomic<float> progress_{0.0f};
  std::atomic<bool> cancel_{false};
  mutable std::mutex mutex_;
  std::string status_;
  std::string latest_;      // "0.5"
  std::string notes_;
  std::string headline_;
  std::string zip_url_;
  uint64_t zip_size_ = 0;
  std::string backup_;      // save_backups\... (relative to the game folder)
  std::vector<std::filesystem::path> staged_;  // files unpacked into update\files (relative)
  std::thread worker_;
};

}  // namespace svr
