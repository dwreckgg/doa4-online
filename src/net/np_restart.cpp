// See np_restart.h.
#include "np_restart.h"

#include <cstdio>
#include <cwctype>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>

#include <cstring>
#include <map>

extern char** environ;
#endif

namespace doa4::np::restart {
namespace fs = std::filesystem;

fs::path HandoffPath(const fs::path& user_data) { return user_data / "netplay" / "handoff.d4h"; }

fs::path SessionUserData(const fs::path& user_data) { return user_data / "netplay" / "session"; }

fs::path SessionDir(const fs::path& user_data, const fs::path& test_dir, bool host, std::time_t now) {
  if (!test_dir.empty()) return test_dir / "session";
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char name[48];
  std::snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d_%s", local.tm_year + 1900, local.tm_mon + 1,
                local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, host ? "host" : "joiner");
  return user_data / "netplay" / "sessions" / name;
}

bool RedirectHolds(const fs::path& runtime_user_data, const fs::path& session_user_data) {
  if (runtime_user_data.empty() || session_user_data.empty()) return false;
  auto norm = [](const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    std::wstring s = a.lexically_normal().generic_wstring();
    while (s.size() > 3 && s.back() == L'/') s.pop_back();
#if defined(_WIN32)
    for (wchar_t& c : s) c = wchar_t(towlower(c));  // Windows paths are case-insensitive
#endif
    return s;
  };
  return norm(runtime_user_data) == norm(session_user_data);
}

bool KnownUnregisteredSetting(std::string_view name) {
#if defined(_WIN32)
  return name == "kernel_pix" || name == "vulkan_readback_resolve" || name == "vulkan_readback_memexport";
#else
  // No D3D12 back end outside Windows (the SDK's CMakeLists.txt sets REXGLUE_USE_D3D12 OFF), so its two readback
  // settings are never registered there; the Vulkan pair is.
  return name == "kernel_pix" || name == "d3d12_readback_resolve" || name == "d3d12_readback_memexport";
#endif
}

bool TestNoRestart(bool test_dir_set, const char* value) {
  return test_dir_set && value != nullptr && std::string_view(value) == "1";
}

namespace {

std::string Text(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

#if defined(_WIN32)
// The attributes of the entry itself (a link is not followed: GetFileAttributesW reports the link's own attributes).
DWORD Attributes(const fs::path& p) { return GetFileAttributesW(p.c_str()); }

bool IsReparse(DWORD a) { return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT); }

// Removes everything inside `dir` (a real folder), depth first. A reparse point is removed as the link itself.
bool Empty(const fs::path& dir, std::string* error) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir / L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) return true;
    *error = "cannot list " + Text(dir) + " (Windows error " + std::to_string(e) + ")";
    return false;
  }
  bool ok = true;
  do {
    const std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    const fs::path p = dir / name;
    const DWORD a = fd.dwFileAttributes;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), a & ~DWORD(FILE_ATTRIBUTE_READONLY));
    BOOL removed;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
      // a junction or a directory link: RemoveDirectoryW removes the link, not the target; a file link: DeleteFileW
      removed = (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(p.c_str()) : DeleteFileW(p.c_str());
    } else if (a & FILE_ATTRIBUTE_DIRECTORY) {
      if (!Empty(p, error)) {
        ok = false;
        break;
      }
      removed = RemoveDirectoryW(p.c_str());
    } else {
      removed = DeleteFileW(p.c_str());
    }
    if (!removed) {
      *error = "cannot remove " + Text(p) + " (Windows error " + std::to_string(GetLastError()) + ")";
      ok = false;
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return ok;
}
#endif

}  // namespace

#if defined(_WIN32)
bool ClearFolder(const fs::path& dir, std::string* error) {
  error->clear();
  const DWORD a = Attributes(dir);
  if (a == INVALID_FILE_ATTRIBUTES) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
      *error = "cannot create " + Text(dir);
      return false;
    }
    // a parent of the folder may itself be a link the creation went through: refuse a folder that is not real now
    if (IsReparse(Attributes(dir))) {
      *error = Text(dir) + " is a link, not a folder";
      return false;
    }
    return true;
  }
  if (IsReparse(a)) {
    *error = Text(dir) + " is a link (reparse point), not a folder: it is not cleared or used";
    return false;
  }
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    *error = Text(dir) + " is a file, not a folder";
    return false;
  }
  return Empty(dir, error);
}
#else
bool ClearFolder(const fs::path& dir, std::string* error) {
  error->clear();
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(dir, ec);  // the entry itself: a link is not followed
  if (!fs::exists(st)) {
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
      *error = "cannot create " + Text(dir);
      return false;
    }
    // a parent of the folder may itself be a link the creation went through: refuse a folder that is not real now
    if (fs::is_symlink(fs::symlink_status(dir, ec))) {
      *error = Text(dir) + " is a link, not a folder";
      return false;
    }
    return true;
  }
  if (fs::is_symlink(st)) {
    *error = Text(dir) + " is a link (symbolic link), not a folder: it is not cleared or used";
    return false;
  }
  if (!fs::is_directory(st)) {
    *error = Text(dir) + " is a file, not a folder";
    return false;
  }
  // remove_all removes a link as the link itself, never its target
  for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
    fs::remove_all(entry.path(), ec);
    if (ec) {
      *error = "cannot remove " + Text(entry.path()) + " (" + ec.message() + ")";
      return false;
    }
  }
  if (ec) {
    *error = "cannot list " + Text(dir) + " (" + ec.message() + ")";
    return false;
  }
  return true;
}
#endif

bool WriteSaveFiles(const fs::path& dir, const std::vector<handoff::File>& files, std::string* error) {
  error->clear();
  bool ok = true;
  for (const handoff::File& f : files) {
    if (!handoff::NameOk(f.name)) {
      *error += (error->empty() ? "" : "; ") + std::string("refused save file name '") + f.name + "'";
      ok = false;
      continue;
    }
    const fs::path p = dir / fs::path(std::u8string(f.name.begin(), f.name.end()));
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out) out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
    if (!out) {
      *error += (error->empty() ? "" : "; ") + std::string("cannot write ") + Text(p);
      ok = false;
    }
  }
  return ok;
}

RelaunchPlan PlanRelaunch(const std::wstring& command_line, unsigned long parent_pid, const fs::path* handoff,
                          bool after_session, bool parent_use_show, unsigned short parent_show) {
  RelaunchPlan plan;
  plan.command_line = command_line;
  plan.env.push_back({L"DOA4_WAIT_PID", std::to_wstring(parent_pid)});
  plan.env.push_back({L"DOA4_NP_HANDOFF", handoff ? handoff->wstring() : std::wstring()});
  plan.env.push_back({L"DOA4_NP_AFTER_SESSION", after_session ? L"1" : L""});
  plan.use_show = parent_use_show;
  plan.show = parent_use_show ? parent_show : 0;
  return plan;
}

#if defined(_WIN32)
bool Relaunch(const fs::path* handoff, bool after_session, std::string* error) {
  wchar_t exe[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    *error = "cannot find the game's executable";
    return false;
  }
  STARTUPINFOW parent{};
  parent.cb = sizeof parent;
  GetStartupInfoW(&parent);
  const RelaunchPlan plan = PlanRelaunch(GetCommandLineW(), GetCurrentProcessId(), handoff, after_session,
                                         (parent.dwFlags & STARTF_USESHOWWINDOW) != 0, parent.wShowWindow);
  // set for the child, restored right after: this process keeps its own values
  std::vector<std::pair<std::wstring, std::wstring>> saved;  // name, old value ("" + flag below = unset)
  std::vector<bool> had;
  for (const EnvVar& v : plan.env) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(v.name.c_str(), buf, 32768);
    had.push_back(n > 0 && n < 32768);
    saved.push_back({v.name, had.back() ? std::wstring(buf, n) : std::wstring()});
    SetEnvironmentVariableW(v.name.c_str(), v.value.empty() ? nullptr : v.value.c_str());
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  if (plan.use_show) {
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = plan.show;
  }
  PROCESS_INFORMATION pi{};
  std::wstring command_line = plan.command_line;
  const BOOL created =
      CreateProcessW(exe, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  const DWORD e = GetLastError();
  for (size_t i = 0; i < saved.size(); ++i) {
    SetEnvironmentVariableW(saved[i].first.c_str(), had[i] ? saved[i].second.c_str() : nullptr);
  }
  if (!created) {
    *error = "CreateProcessW failed (Windows error " + std::to_string(e) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#else
namespace {

// This process's own arguments, from /proc/self/cmdline (NUL-separated).
std::vector<std::string> OwnArguments() {
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  std::vector<std::string> args;
  for (std::string arg; std::getline(in, arg, '\0');) args.push_back(arg);
  return args;
}

// This process's environment with the plan's variables set (an empty value removes one).
std::vector<std::string> ChildEnvironment(const std::vector<EnvVar>& changes) {
  std::map<std::string, std::string> env;
  for (char** e = environ; *e; ++e) {
    const char* eq = std::strchr(*e, '=');
    if (eq) env[std::string(*e, size_t(eq - *e))] = eq + 1;
  }
  for (const EnvVar& v : changes) {
    const std::string name = fs::path(v.name).string();  // wide to UTF-8 (a path converts both ways)
    if (v.value.empty()) env.erase(name);
    else env[name] = fs::path(v.value).string();
  }
  std::vector<std::string> out;
  for (const auto& [name, value] : env) out.push_back(name + "=" + value);
  return out;
}

std::vector<char*> PointerList(std::vector<std::string>& strings) {
  std::vector<char*> list;
  for (std::string& s : strings) list.push_back(s.data());
  list.push_back(nullptr);
  return list;
}

}  // namespace

bool Relaunch(const fs::path* handoff, bool after_session, std::string* error) {
  std::error_code ec;
  const std::string exe = fs::read_symlink("/proc/self/exe", ec).string();
  std::vector<std::string> args = OwnArguments();
  if (exe.empty() || args.empty()) {
    *error = "cannot find the game's executable";
    return false;
  }
  // The command line and window state of the plan are Windows things; the child gets this process's arguments.
  const RelaunchPlan plan = PlanRelaunch(std::wstring(), static_cast<unsigned long>(getpid()), handoff, after_session,
                                         false, 0);
  std::vector<std::string> env = ChildEnvironment(plan.env);
  std::vector<char*> argv = PointerList(args), envp = PointerList(env);
  // No descriptor of this process reaches the child (CreateProcessW with bInheritHandles FALSE): the child would
  // otherwise keep this game's UDP socket and its port open.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  pid_t child = 0;
  const int e = posix_spawn(&child, exe.c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (e != 0) {
    *error = "posix_spawn failed (" + std::string(std::strerror(e)) + ")";
    return false;
  }
  return true;
}
#endif

}  // namespace doa4::np::restart
