// In-game updater: see updater.h for what it touches and what it never does.

#include "updater.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>

#include <sstream>

#include <nlohmann/json.hpp>
#include <rex/cvar.h>
#include <rex/logging.h>

// stb_image's zlib decoder unpacks the release zip (deflate); only that part is used.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

REXCVAR_DEFINE_BOOL(svr_check_updates, true, "SvR",
                    "At startup, check GitHub for a newer release of this port (it is only "
                    "installed when you choose to, from the settings menu)");

namespace svr {
namespace fs = std::filesystem;

namespace {

constexpr const char* kSetAsideSuffix = ".update-old";

std::string U8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

std::vector<int> ParseVersion(std::string v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
    v.erase(0, 1);
  std::vector<int> parts;
  int value = -1;
  for (char c : v) {
    if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value * 10) + (c - '0');
    } else if (c == '.') {
      parts.push_back(std::max(value, 0));
      value = -1;
    } else {
      break;  // "-rc1" and the like
    }
  }
  if (value >= 0)
    parts.push_back(value);
  return parts;
}

// a > b
bool IsNewer(const std::string& a, const std::string& b) {
  std::vector<int> va = ParseVersion(a), vb = ParseVersion(b);
  const size_t n = std::max(va.size(), vb.size());
  va.resize(n, 0);
  vb.resize(n, 0);
  return va > vb;
}

std::string StripV(std::string v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
    v.erase(0, 1);
  return v;
}

// Markdown inline formatting off: **bold**, `code`, [text](link) -> text.
std::string PlainText(std::string s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '*' || s[i] == '`')
      continue;
    if (s[i] == '[') {
      const size_t close = s.find("](", i);
      const size_t end = close == std::string::npos ? close : s.find(')', close);
      if (end != std::string::npos) {
        out += PlainText(s.substr(i + 1, close - i - 1));
        i = end;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// The part of a release's notes that says what changed: from its "## " heading up to the
// download / install instructions every release repeats. Code blocks are left out.
std::string ReleaseChanges(const std::string& markdown, const std::string& tag,
                           std::string* title) {
  std::string out, line;
  bool started = false, code = false;
  std::istringstream in(markdown);
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.rfind("```", 0) == 0) {
      code = !code;
      continue;
    }
    if (code)
      continue;
    if (line.rfind("## ", 0) == 0) {
      if (started)
        break;
      started = true;
      const std::string heading = PlainText(line.substr(3));
      out += "# " + heading + "\n";
      if (title) {
        const size_t colon = heading.find(": ");
        *title = colon == std::string::npos ? std::string() : heading.substr(colon + 2);
      }
      continue;
    }
    if (!started)
      continue;
    if (line.rfind("### ", 0) == 0 || line.rfind("**Updating", 0) == 0 ||
        line.rfind("**You need", 0) == 0 || line.rfind("Full step-by-step", 0) == 0)
      break;
    size_t lead = line.find_first_not_of(' ');
    if (lead == std::string::npos) {
      if (!out.empty() && out.back() != '\n')
        out += '\n';
      continue;
    }
    std::string text = line.substr(lead);
    if (text.rfind("- ", 0) == 0 || text.rfind("* ", 0) == 0)
      text = "- " + text.substr(2);
    out += PlainText(text) + "\n";
  }
  if (!started)  // notes without the usual heading: say which version at least
    out = "# Version " + StripV(tag) + "\n";
  return out;
}

uint32_t Crc32(const uint8_t* data, size_t size) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// A zip entry name as a safe path inside the game folder, or empty.
fs::path SafeRelativePath(std::string name) {
  std::replace(name.begin(), name.end(), '\\', '/');
  if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos)
    return {};
  fs::path rel = fs::path(std::u8string(name.begin(), name.end()));
  for (const auto& part : rel) {
    const std::string s = U8(part);
    if (s == ".." || s == ".")
      return {};
  }
  return rel;
}

#if defined(_WIN32)
std::wstring Widen(const std::string& s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

struct WinHttpHandle {
  HINTERNET h = nullptr;
  ~WinHttpHandle() {
    if (h)
      WinHttpCloseHandle(h);
  }
};

// GET `url` (HTTPS, redirects followed) into `body` or `file`.
bool HttpGet(const std::string& url, bool github_api, std::string* body, std::ofstream* file,
             uint64_t expected, std::atomic<float>* progress, const std::atomic<bool>& cancel,
             std::string& error) {
  const std::wstring wurl = Widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = DWORD(-1);
  uc.dwUrlPathLength = DWORD(-1);
  uc.dwExtraInfoLength = DWORD(-1);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    error = "bad address";
    return false;
  }
  const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.lpszExtraInfo)
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

  WinHttpHandle session;
  session.h = WinHttpOpen(L"SvR-RECOMP-updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h)  // older Windows and Wine: no automatic proxy
    session.h = WinHttpOpen(L"SvR-RECOMP-updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h) {
    error = "no internet access";
    return false;
  }
  WinHttpSetTimeouts(session.h, 15000, 15000, 15000, 30000);
  WinHttpHandle connection;
  connection.h = WinHttpConnect(session.h, host.c_str(), uc.nPort, 0);
  if (!connection.h) {
    error = "couldn't reach " + url.substr(0, url.find('/', 8));
    return false;
  }
  WinHttpHandle request;
  request.h = WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!request.h) {
    error = "couldn't start the request";
    return false;
  }
  const wchar_t* headers =
      github_api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
  if (!WinHttpSendRequest(request.h, headers, github_api ? DWORD(-1) : 0, WINHTTP_NO_REQUEST_DATA,
                          0, 0, 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    error = "no connection (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  DWORD code = 0, code_size = sizeof(code);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_size, WINHTTP_NO_HEADER_INDEX);
  if (code != 200) {
    error = "server answered " + std::to_string(code);
    return false;
  }
  std::vector<char> buffer(1 << 16);
  uint64_t received = 0;
  for (;;) {
    if (cancel.load()) {
      error = "cancelled";
      return false;
    }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.h, &available)) {
      error = "connection lost";
      return false;
    }
    if (available == 0)
      break;
    DWORD read = 0;
    if (!WinHttpReadData(request.h, buffer.data(), std::min<DWORD>(available, DWORD(buffer.size())),
                         &read)) {
      error = "connection lost";
      return false;
    }
    if (read == 0)
      break;
    if (body)
      body->append(buffer.data(), read);
    if (file) {
      file->write(buffer.data(), read);
      if (!*file) {
        error = "couldn't write the download (disk full?)";
        return false;
      }
    }
    received += read;
    if (progress && expected)
      progress->store(float(std::min<double>(1.0, double(received) / double(expected))));
  }
  return true;
}
#endif

}  // namespace

Updater::Updater(Config config) : config_(std::move(config)) {}

Updater::~Updater() {
  cancel_ = true;
  if (worker_.joinable())
    worker_.join();
}

void Updater::SetStatus(State state, std::string status) {
  {
    std::lock_guard lock(mutex_);
    status_ = std::move(status);
  }
  state_ = state;
  REXLOG_INFO("Updater: {}", this->status());
}

std::string Updater::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

std::string Updater::latest_version() const {
  std::lock_guard lock(mutex_);
  return latest_;
}

std::string Updater::notes() const {
  std::lock_guard lock(mutex_);
  return notes_;
}

std::string Updater::headline() const {
  std::lock_guard lock(mutex_);
  return headline_;
}

std::string Updater::backup_folder() const {
  std::lock_guard lock(mutex_);
  return backup_;
}

void Updater::StartCheck() {
  const State s = state_.load();
  if (s == State::kChecking || s == State::kWorking || s == State::kInstalled)
    return;
  if (worker_.joinable())
    worker_.join();
  SetStatus(State::kChecking, "Checking for updates...");
  worker_ = std::thread([this] { Check(); });
}

void Updater::StartInstall() {
  if (state_.load() != State::kAvailable)
    return;
  if (worker_.joinable())
    worker_.join();
  progress_ = 0.0f;
  SetStatus(State::kWorking, "Backing up your saves...");
  worker_ = std::thread([this] { Install(); });
}

void Updater::Check() {
#if defined(_WIN32)
  std::string body, error;
  // The recent releases (newest first): the newest published one, and its notes.
  const std::string url =
      "https://api.github.com/repos/" + config_.repo + "/releases?per_page=30";
  if (!HttpGet(url, true, &body, nullptr, 0, nullptr, cancel_, error)) {
    SetStatus(State::kCheckFailed, "Couldn't check for updates (" + error + ").");
    return;
  }
  const auto list = nlohmann::json::parse(body, nullptr, false);
  if (list.is_discarded() || !list.is_array()) {
    SetStatus(State::kCheckFailed, "Couldn't check for updates (unexpected reply).");
    return;
  }
  nlohmann::json json;  // the newest published release
  std::string notes, headline;
  int noted = 0;
  for (const auto& release : list) {
    if (!release.is_object() || release.value("draft", false) || release.value("prerelease", false))
      continue;
    const std::string release_tag = release.value("tag_name", std::string());
    if (json.is_null())
      json = release;
    if (noted < 1 && IsNewer(release_tag, config_.current_version)) {  // the newest version only
      std::string title;
      const std::string body_text =
          release.contains("body") && release["body"].is_string() ? release["body"].get<std::string>() : "";
      notes += ReleaseChanges(body_text, release_tag, noted == 0 ? &title : nullptr);
      notes += "\n";
      if (noted == 0)
        headline = title;
      ++noted;
    }
  }
  if (!json.is_object()) {
    SetStatus(State::kCheckFailed, "Couldn't check for updates (unexpected reply).");
    return;
  }
  const std::string tag = json.value("tag_name", std::string());
  std::string zip_url;
  uint64_t zip_size = 0;
  if (json.contains("assets") && json["assets"].is_array()) {
    for (const auto& asset : json["assets"]) {
      if (asset.is_object() && asset.value("name", std::string()) == config_.asset) {
        zip_url = asset.value("browser_download_url", std::string());
        zip_size = asset.value("size", uint64_t{0});
      }
    }
  }
  if (tag.empty() || zip_url.empty()) {
    SetStatus(State::kCheckFailed, "Couldn't check for updates (no download in the latest release).");
    return;
  }
  {
    std::lock_guard lock(mutex_);
    latest_ = StripV(tag);
    notes_ = notes;
    headline_ = headline;
    zip_url_ = zip_url;
    zip_size_ = zip_size;
  }
  if (IsNewer(tag, config_.current_version))
    SetStatus(State::kAvailable, "Version " + StripV(tag) + " is available.");
  else
    SetStatus(State::kUpToDate, "You have the latest version (" + config_.current_version + ").");
#else
  SetStatus(State::kCheckFailed, "Updates are checked on Windows (and Proton) only.");
#endif
}

void Updater::Install() {
  std::string error;
  const std::string version = latest_version();
  if (!BackUpSaves(error)) {
    SetStatus(State::kFailed, "Update stopped: your saves couldn't be backed up (" + error +
                                  "). Nothing was changed.");
    return;
  }
  SetStatus(State::kWorking, "Downloading version " + version + "...");
  if (!Download(error)) {
    SetStatus(State::kFailed, "Download failed (" + error + "). Nothing was changed.");
    return;
  }
  SetStatus(State::kWorking, "Checking the download...");
  if (!Unpack(error)) {
    SetStatus(State::kFailed, "The download is damaged (" + error + "). Nothing was changed.");
    return;
  }
  SetStatus(State::kWorking, "Installing version " + version + "...");
  if (!Swap(error)) {
    SetStatus(State::kFailed, "Install failed (" + error +
                                  "). The current version was put back; your saves weren't touched.");
    return;
  }
  std::error_code ec;
  fs::remove(config_.game_dir / "update" / config_.asset, ec);
  const std::string backup = backup_folder();
  SetStatus(State::kInstalled,
            "Version " + version + " is installed. Restart to play it." +
                (backup.empty() ? std::string() : " Your saves are also backed up in " + backup + "."));
}

bool Updater::BackUpSaves(std::string& error) {
  std::error_code ec;
  const fs::path src = config_.user_data;
  if (src.empty() || !fs::exists(src, ec)) {
    REXLOG_INFO("Updater: no userdata folder yet, nothing to back up");
    return true;
  }
  char stamp[32] = {};
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", &local);
  const fs::path rel_dir = fs::path("save_backups") / (config_.current_version + "_" + stamp);
  const fs::path dst = config_.game_dir / rel_dir;
  fs::create_directories(dst, ec);
  if (ec) {
    error = "couldn't create " + U8(rel_dir);
    return false;
  }
  uint64_t files = 0, bytes = 0;
  auto it = fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    error = "couldn't read the userdata folder";
    return false;
  }
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      error = "couldn't read the userdata folder";
      return false;
    }
    const fs::path rel = it->path().lexically_relative(src);
    // Not copied: the shader cache (rebuilt by itself), which updates don't touch; saves and
    // profiles are everything else.
    const bool skip = !rel.empty() && *rel.begin() == "cache";
    if (skip) {
      if (it->is_directory(ec))
        it.disable_recursion_pending();
      continue;
    }
    if (it->is_directory(ec)) {
      fs::create_directories(dst / rel, ec);
      continue;
    }
    if (!it->is_regular_file(ec))
      continue;
    fs::create_directories((dst / rel).parent_path(), ec);
    fs::copy_file(it->path(), dst / rel, fs::copy_options::overwrite_existing, ec);
    std::error_code e1, e2;
    const uintmax_t a = fs::file_size(it->path(), e1), b = fs::file_size(dst / rel, e2);
    if (ec || e1 || e2 || a != b) {
      error = "copying " + U8(rel) + " failed";
      return false;
    }
    ++files;
    bytes += a;
  }
  {
    std::lock_guard lock(mutex_);
    backup_ = U8(rel_dir);
  }
  REXLOG_INFO("Updater: backed up {} files ({} bytes) from {} to {}", files, bytes, U8(src),
              U8(dst));
  return true;
}

bool Updater::Download(std::string& error) {
#if defined(_WIN32)
  std::string url;
  uint64_t size = 0;
  {
    std::lock_guard lock(mutex_);
    url = zip_url_;
    size = zip_size_;
  }
  std::error_code ec;
  const fs::path dir = config_.game_dir / "update";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) {
    error = "couldn't create the update folder";
    return false;
  }
  const fs::path zip = dir / config_.asset;
  {
    std::ofstream out(zip, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "couldn't write to the game folder";
      return false;
    }
    if (!HttpGet(url, false, nullptr, &out, size, &progress_, cancel_, error))
      return false;
  }
  const uintmax_t got = fs::file_size(zip, ec);
  if (ec || (size && got != size)) {
    error = "incomplete (" + std::to_string(got) + " of " + std::to_string(size) + " bytes)";
    return false;
  }
  return true;
#else
  error = "not supported here";
  return false;
#endif
}

bool Updater::Unpack(std::string& error) {
  std::error_code ec;
  const fs::path dir = config_.game_dir / "update";
  std::vector<uint8_t> zip;
  {
    std::ifstream in(dir / config_.asset, std::ios::binary);
    zip.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  if (zip.size() < 22) {
    error = "not a zip";
    return false;
  }
  // End of central directory, within the last 64 KB + 22 bytes.
  size_t eocd = std::string::npos;
  for (size_t i = zip.size() - 22 + 1; i-- > 0 && zip.size() - i <= 65557;) {
    if (Le32(&zip[i]) == 0x06054b50u) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) {
    error = "not a zip";
    return false;
  }
  const uint16_t count = Le16(&zip[eocd + 10]);
  size_t p = Le32(&zip[eocd + 16]);
  const fs::path out_dir = dir / "files";
  fs::remove_all(out_dir, ec);
  staged_.clear();
  bool has_exe = false;
  for (uint16_t n = 0; n < count; ++n) {
    if (p + 46 > zip.size() || Le32(&zip[p]) != 0x02014b50u) {
      error = "bad directory";
      return false;
    }
    const uint16_t method = Le16(&zip[p + 10]);
    const uint32_t crc = Le32(&zip[p + 16]);
    const uint32_t packed = Le32(&zip[p + 20]);
    const uint32_t size = Le32(&zip[p + 24]);
    const uint16_t name_len = Le16(&zip[p + 28]);
    const size_t next = p + 46 + name_len + Le16(&zip[p + 30]) + Le16(&zip[p + 32]);
    const uint32_t local = Le32(&zip[p + 42]);
    if (p + 46 + name_len > zip.size()) {
      error = "bad directory";
      return false;
    }
    const std::string name(reinterpret_cast<const char*>(&zip[p + 46]), name_len);
    p = next;
    if (!name.empty() && (name.back() == '/' || name.back() == '\\'))
      continue;  // folders are made as needed
    const fs::path rel = SafeRelativePath(name);
    if (rel.empty()) {
      error = "unsafe file name " + name;
      return false;
    }
    if (local + 30 > zip.size() || Le32(&zip[local]) != 0x04034b50u) {
      error = "bad entry " + name;
      return false;
    }
    const size_t data = size_t(local) + 30 + Le16(&zip[local + 26]) + Le16(&zip[local + 28]);
    if (data + packed > zip.size()) {
      error = "truncated " + name;
      return false;
    }
    std::vector<uint8_t> bytes(size);
    if (method == 0 && packed == size) {
      std::memcpy(bytes.data(), &zip[data], size);
    } else if (method == 8) {
      if (size && stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(bytes.data()), int(size),
                                                   reinterpret_cast<const char*>(&zip[data]),
                                                   int(packed)) != int(size)) {
        error = "can't unpack " + name;
        return false;
      }
    } else {
      error = "unsupported compression in " + name;
      return false;
    }
    if (Crc32(bytes.data(), bytes.size()) != crc) {
      error = "checksum mismatch in " + name;
      return false;
    }
    const fs::path out = out_dir / rel;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!f) {
      error = "couldn't write " + name + " (disk full?)";
      return false;
    }
    staged_.push_back(rel);
    if (rel == fs::path(config_.exe))
      has_exe = true;
  }
  if (!has_exe) {
    error = "it doesn't contain " + config_.exe;
    return false;
  }
  return true;
}

bool Updater::Swap(std::string& error) {
  struct Replaced {
    fs::path dest, aside;
    bool had_old;
  };
  std::vector<Replaced> done;
  std::error_code ec;
  auto roll_back = [&] {
    for (auto r = done.rbegin(); r != done.rend(); ++r) {
      std::error_code e;
      fs::remove(r->dest, e);
      if (r->had_old)
        fs::rename(r->aside, r->dest, e);
    }
  };
  const fs::path staged_dir = config_.game_dir / "update" / "files";
  for (const fs::path& rel : staged_) {
    const std::string top = U8(*rel.begin());
    // The player's data stays as it is (saves, backups, the update itself).
    if (top == "userdata" || top == "save_backups" || top == "update")
      continue;
    const fs::path dest = config_.game_dir / rel;
    // Their settings, and the pipelines their own play recorded, are kept when they exist.
    if ((rel == fs::path(config_.settings_file) || rel == fs::path("pipelines.bin")) &&
        fs::exists(dest, ec))
      continue;
    fs::create_directories(dest.parent_path(), ec);
    Replaced r{dest, dest, fs::exists(dest, ec)};
    r.aside += kSetAsideSuffix;
    if (r.had_old) {
      std::error_code e;
      fs::remove(r.aside, e);  // left from an earlier update
      for (int i = 2; fs::exists(r.aside, e) && i < 100; ++i) {
        r.aside = dest;
        r.aside += kSetAsideSuffix + std::to_string(i);
      }
      fs::rename(dest, r.aside, ec);
      if (ec) {
        roll_back();
        error = "couldn't replace " + U8(rel) + ": " + ec.message();
        return false;
      }
    }
    fs::rename(staged_dir / rel, dest, ec);
    if (ec) {
      std::error_code e;
      if (r.had_old)
        fs::rename(r.aside, dest, e);
      roll_back();
      error = "couldn't install " + U8(rel) + ": " + ec.message();
      return false;
    }
    done.push_back(r);
  }
  REXLOG_INFO("Updater: installed {} files", done.size());
  return true;
}

bool Updater::Restart() {
#if defined(_WIN32)
  const std::wstring exe = (config_.game_dir / config_.exe).wstring();
  std::wstring command = GetCommandLineW();
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  // The new process waits for this one to finish exiting before it starts (CleanUp).
  SetEnvironmentVariableW(L"SVR_UPDATER_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const bool ok = CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                 nullptr, &si, &pi);
  SetEnvironmentVariableW(L"SVR_UPDATER_WAIT_PID", nullptr);
  if (!ok)
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  return false;
#endif
}

void Updater::CleanUp(const fs::path& game_dir) {
#if defined(_WIN32)
  // Started by Restart(): let the previous version finish exiting first.
  wchar_t pid_text[32] = {};
  if (GetEnvironmentVariableW(L"SVR_UPDATER_WAIT_PID", pid_text, 32)) {
    SetEnvironmentVariableW(L"SVR_UPDATER_WAIT_PID", nullptr);
    if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, DWORD(_wtoi(pid_text)))) {
      WaitForSingleObject(previous, 20000);
      CloseHandle(previous);
    }
  }
#endif
  std::error_code ec;
  fs::remove_all(game_dir / "update", ec);
  auto it = fs::recursive_directory_iterator(game_dir, fs::directory_options::skip_permission_denied,
                                             ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const std::string name = U8(it->path().filename());
    if (it->is_directory(ec)) {
      if (name == "userdata" || name == "save_backups")
        it.disable_recursion_pending();
      continue;
    }
    if (name.find(kSetAsideSuffix) != std::string::npos) {
      std::error_code e;
      fs::remove(it->path(), e);
    }
  }
}

}  // namespace svr
