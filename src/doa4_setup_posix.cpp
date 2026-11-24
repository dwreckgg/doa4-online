// The first-run setup's platform parts outside Windows (doa4_setup_win.h): the SDL3 file dialog for the disc image and
// the free space of a folder's file system.

#include "doa4_setup_win.h"

#include <sys/statvfs.h>

#include <system_error>

#include "doa4_sdl_dialog.h"

namespace doa4::setup {

namespace fs = std::filesystem;

std::optional<fs::path> PickIsoFile(void* owner_hwnd, const fs::path& start) {
  static constexpr sdl_dialog::Filter kTypes[] = {{"Disc images (*.iso)", "iso"}, {"All files", "*"}};
  return sdl_dialog::PickFile(static_cast<SDL_Window*>(owner_hwnd), "Choose your Dead or Alive 4 disc image", start,
                              kTypes, 2);
}

std::optional<uint64_t> FreeBytes(const fs::path& target) {
  std::error_code ec;
  fs::path dir = target;
  while (!dir.empty() && !fs::is_directory(dir, ec)) {
    const fs::path parent = dir.parent_path();
    if (parent == dir) return std::nullopt;
    dir = parent;
  }
  if (dir.empty()) return std::nullopt;
  struct statvfs st;
  if (statvfs(dir.c_str(), &st) != 0) return std::nullopt;
  return uint64_t(st.f_bavail) * st.f_frsize;  // the space an unprivileged user may fill, as GetDiskFreeSpaceExW's
}

}  // namespace doa4::setup
