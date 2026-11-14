// The SDL3 file and folder dialogs, waited for. See doa4_sdl_dialog.h.

#include "doa4_sdl_dialog.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <mutex>
#include <system_error>
#include <vector>

#include <rex/logging.h>

#include "doa4_utf8.h"

namespace doa4::sdl_dialog {

namespace {

// The callback's answer. The portal answers on the UI thread (inside SDL_PumpEvents), zenity on a thread of its own.
struct Answer {
  std::mutex mu;
  std::optional<std::filesystem::path> path;
  std::atomic<bool> done{false};
};

void SDLCALL OnChosen(void* userdata, const char* const* filelist, int) {
  auto* answer = static_cast<Answer*>(userdata);
  if (!filelist) {
    REXLOG_WARN("[doa4][DIALOG] the file dialog failed: {}", SDL_GetError());
  } else if (filelist[0]) {
    std::lock_guard<std::mutex> lock(answer->mu);
    answer->path = PathFromUtf8(filelist[0]);
  }
  answer->done.store(true, std::memory_order_release);
}

std::optional<std::filesystem::path> Show(SDL_FileDialogType type, SDL_Window* owner, const std::string& title,
                                          const std::filesystem::path& start, const Filter* filters, int filter_count) {
  std::vector<SDL_DialogFileFilter> sdl_filters;
  for (int i = 0; i < filter_count; ++i) sdl_filters.push_back({filters[i].name, filters[i].pattern});
  std::error_code ec;
  const std::string location = !start.empty() && std::filesystem::is_directory(start, ec) ? Utf8(start) : std::string();
  Answer answer;
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, owner);
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
  if (!location.empty()) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
  if (!sdl_filters.empty()) {
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, sdl_filters.data());
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, Sint64(sdl_filters.size()));
  }
  SDL_ShowFileDialogWithProperties(type, OnChosen, &answer, props);
  SDL_DestroyProperties(props);
  // The modal wait: the window's own events stay queued for the game's loop.
  while (!answer.done.load(std::memory_order_acquire)) {
    SDL_PumpEvents();
    SDL_Delay(10);
  }
  std::lock_guard<std::mutex> lock(answer.mu);
  return answer.path;
}

}  // namespace

std::optional<std::filesystem::path> PickFolder(SDL_Window* owner, const std::string& title,
                                                const std::filesystem::path& start) {
  return Show(SDL_FILEDIALOG_OPENFOLDER, owner, title, start, nullptr, 0);
}

std::optional<std::filesystem::path> PickFile(SDL_Window* owner, const std::string& title,
                                              const std::filesystem::path& start, const Filter* filters,
                                              int filter_count) {
  return Show(SDL_FILEDIALOG_OPENFILE, owner, title, start, filters, filter_count);
}

}  // namespace doa4::sdl_dialog
