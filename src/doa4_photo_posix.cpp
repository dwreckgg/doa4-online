// Full-quality photos: the platform parts outside Windows (doa4_photo_win.h), through SDL3: PNG files, the Pictures
// folder, the folder dialog, the file manager and the display size.

#include "doa4_photo_win.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>
#include <system_error>

#include "doa4_sdl_dialog.h"
#include "doa4_utf8.h"

namespace doa4::photo {

namespace {

// A file:// URL for xdg-open: every byte outside RFC 3986's unreserved set and '/' percent-encoded.
std::string FileUrl(const std::filesystem::path& path) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string url = "file://";
  for (const unsigned char c : Utf8(path)) {
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '.' || c == '_' || c == '~' || c == '/';
    if (plain) {
      url += char(c);
    } else {
      url += '%';
      url += kHex[c >> 4];
      url += kHex[c & 15];
    }
  }
  return url;
}

}  // namespace

// SDL3 writes PNG only (SDL_SavePNG); the build links no JPEG encoder outside Windows.
std::string WriteImage(const Image& image, const std::filesystem::path& path, FileType type) {
  if (image.width == 0 || image.height == 0 || image.rgbx.size() != size_t(image.width) * image.height * 4) {
    return "there is no image to save";
  }
  if (type != FileType::kPng) return "JPG photos are not available in the Linux build; choose PNG in the Photo tab";
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) return "cannot create " + Utf8(path.parent_path()) + ": " + ec.message();
  // R G B X bytes: SDL_PIXELFORMAT_RGBX32 is that byte order on every host.
  SDL_Surface* surface = SDL_CreateSurfaceFrom(int(image.width), int(image.height), SDL_PIXELFORMAT_RGBX32,
                                               const_cast<uint8_t*>(image.rgbx.data()), int(image.width * 4));
  if (!surface) return std::string("setting up the image failed: ") + SDL_GetError();
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::string error;
  if (!SDL_SavePNG(surface, Utf8(tmp).c_str())) error = std::string("writing the image failed: ") + SDL_GetError();
  SDL_DestroySurface(surface);
  if (error.empty()) {
    std::filesystem::rename(tmp, path, ec);
    if (ec) error = "cannot write " + Utf8(path) + ": " + ec.message();
  }
  if (!error.empty()) std::filesystem::remove(tmp, ec);
  return error;
}

bool ReadImage(const std::filesystem::path& path, Image* image) {
  SDL_Surface* loaded = SDL_LoadPNG(Utf8(path).c_str());
  if (!loaded) return false;
  SDL_Surface* rgbx = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBX32);
  SDL_DestroySurface(loaded);
  if (!rgbx) return false;
  image->width = uint32_t(rgbx->w);
  image->height = uint32_t(rgbx->h);
  image->rgbx.resize(size_t(rgbx->w) * rgbx->h * 4);
  for (int y = 0; y < rgbx->h; ++y) {
    std::memcpy(image->rgbx.data() + size_t(y) * rgbx->w * 4, static_cast<const uint8_t*>(rgbx->pixels) + y * rgbx->pitch,
                size_t(rgbx->w) * 4);
  }
  for (size_t i = 3; i < image->rgbx.size(); i += 4) image->rgbx[i] = 0;  // the X byte, as the Windows reader leaves it
  SDL_DestroySurface(rgbx);
  return true;
}

std::string PathText(const std::filesystem::path& path) { return Utf8(path); }

std::filesystem::path DefaultPhotoDir() {
  const char* pictures = SDL_GetUserFolder(SDL_FOLDER_PICTURES);
  return pictures ? PathFromUtf8(pictures) / "DOA4" : std::filesystem::path();
}

std::optional<std::filesystem::path> PickFolder(void* owner_hwnd, const std::filesystem::path& start,
                                                const wchar_t* title) {
  return sdl_dialog::PickFolder(static_cast<SDL_Window*>(owner_hwnd), Utf8(std::filesystem::path(title)), start);
}

void OpenInExplorer(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  SDL_OpenURL(FileUrl(dir).c_str());
}

Size MonitorSize(void* hwnd) {
  const SDL_DisplayID display =
      hwnd ? SDL_GetDisplayForWindow(static_cast<SDL_Window*>(hwnd)) : SDL_GetPrimaryDisplay();
  const SDL_DisplayMode* mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
  if (!mode) return {};
  return Size{uint32_t(std::lround(mode->w * mode->pixel_density)), uint32_t(std::lround(mode->h * mode->pixel_density))};
}

}  // namespace doa4::photo
