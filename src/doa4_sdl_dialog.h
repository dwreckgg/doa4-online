// The file and folder dialogs outside Windows: SDL3's (xdg-desktop-portal, or zenity), waited for like the Windows
// dialogs, which are modal: the call returns when the player has chosen or cancelled.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct SDL_Window;

namespace doa4::sdl_dialog {

// A file filter: its name and its extensions without dots, separated by ';' ("iso", "*" for any file).
struct Filter {
  const char* name;
  const char* pattern;
};

// A folder, starting in `start` when it is one. nullopt when cancelled or on error. UI thread only: the wait pumps the
// SDL events, which is how a portal's answer arrives.
std::optional<std::filesystem::path> PickFolder(SDL_Window* owner, const std::string& title,
                                                const std::filesystem::path& start);

// One existing file, offered through `filters` (the first is selected), starting in `start`. As PickFolder.
std::optional<std::filesystem::path> PickFile(SDL_Window* owner, const std::string& title,
                                              const std::filesystem::path& start, const Filter* filters,
                                              int filter_count);

}  // namespace doa4::sdl_dialog
