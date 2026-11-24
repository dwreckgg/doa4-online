// Host test for src/doa4_config.cpp (UpsertTomlKey, TomlString, SaveTomlKey, SaveTomlKeys).
// Built and run by run_toml_upsert_test.bat.

#include "../../src/doa4_config.h"
#include "../../src/doa4_utf8.h"

#include <toml++/toml.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using doa4::TomlString;
using doa4::UpsertTomlKey;

// The config a session leaves behind (Task 2b): the session-start snapshot with the kept controller saves replayed in
// the order they were made, so a key the snapshot lacked lands where the menu's save put it.
static void TestConfigAfterSession() {
  using Kept = std::vector<std::pair<std::string, std::string>>;
  const std::string snap = "a = 1\nb = 2\n[t]\nx = 1\n";
  // replace, insert before the table, and a later save of the same key wins in place
  CHECK(doa4::ConfigAfterSession(snap, Kept{{"pad_remap_p1", "\"A=B\""}, {"b", "3"}, {"pad_remap_p1", "\"\""}}) ==
        "a = 1\nb = 3\npad_remap_p1 = \"\"\n[t]\nx = 1\n");
  // two missing keys keep their save order (a replay in another order would swap the two new lines)
  CHECK(doa4::ConfigAfterSession(snap, Kept{{"z", "1"}, {"y", "2"}}) == "a = 1\nb = 2\nz = 1\ny = 2\n[t]\nx = 1\n");
  // nothing kept: the snapshot, byte for byte (CRLF kept)
  CHECK(doa4::ConfigAfterSession("a = 1\r\n", Kept{}) == "a = 1\r\n");
  // no config at session start: only the kept lines
  CHECK(doa4::ConfigAfterSession("", Kept{{"mnk_mode", "true"}}) == "mnk_mode = true\n");
  // the last of three entries is applied too
  CHECK(doa4::ConfigAfterSession("", Kept{{"a", "1"}, {"b", "2"}, {"c", "3"}}) == "a = 1\nb = 2\nc = 3\n");
}

static void TestUpsert() {
  // Empty file.
  CHECK(UpsertTomlKey("", "fullscreen", "true") == "fullscreen = true\n");
  // Replace in place, every other line byte for byte.
  CHECK(UpsertTomlKey("a = 1\nfullscreen = false\nb = 2\n", "fullscreen", "true") ==
        "a = 1\nfullscreen = true\nb = 2\n");
  // Whole-key match only: resolution_scale_x is not resolution_scale.
  CHECK(UpsertTomlKey("resolution_scale_x = 1\n", "resolution_scale", "2") ==
        "resolution_scale_x = 1\nresolution_scale = 2\n");
  // Comments are not keys.
  CHECK(UpsertTomlKey("# fullscreen = false\n", "fullscreen", "true") ==
        "# fullscreen = false\nfullscreen = true\n");
  // A missing key goes before the first table; keys inside tables are not top-level.
  CHECK(UpsertTomlKey("a = 1\n[GPU]\nfullscreen = false\n", "fullscreen", "true") ==
        "a = 1\nfullscreen = true\n[GPU]\nfullscreen = false\n");
  // CRLF files get CRLF lines; a replaced line keeps its own ending.
  CHECK(UpsertTomlKey("a = 1\r\nb = 2\r\n", "c", "3") == "a = 1\r\nb = 2\r\nc = 3\r\n");
  CHECK(UpsertTomlKey("a = 1\r\nb = 2\r\n", "a", "9") == "a = 9\r\nb = 2\r\n");
  // No final newline.
  CHECK(UpsertTomlKey("a = 1", "c", "3") == "a = 1\nc = 3\n");
  CHECK(UpsertTomlKey("a = 1\nc = 2", "c", "3") == "a = 1\nc = 3");
  // Spacing around the key does not matter; the line is rewritten canonically.
  CHECK(UpsertTomlKey("  fullscreen=false  \n", "fullscreen", "true") == "fullscreen = true\n");
  // Only the first match is replaced (a duplicate key is left for the SDK's
  // parser to reject, as it would have before).
  CHECK(UpsertTomlKey("a = 1\na = 2\n", "a", "3") == "a = 3\na = 2\n");
}

static void TestTomlString() {
  CHECK(TomlString("Space") == "\"Space\"");
  CHECK(TomlString("A=B,B=A") == "\"A=B,B=A\"");
  CHECK(TomlString("a\"b\\c") == "\"a\\\"b\\\\c\"");
  CHECK(TomlString("") == "\"\"");
}

static std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void TestSave() {
  auto dir = std::filesystem::temp_directory_path() / "doa4_toml_upsert_test_data";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  auto path = dir / "dead_or_alive_4.toml";
  CHECK(doa4::SaveTomlKey(path, "resolution_scale", "2").empty());
  CHECK(doa4::SaveTomlKey(path, "keybind_a", TomlString("Space")).empty());
  CHECK(doa4::SaveTomlKey(path, "resolution_scale", "3").empty());
  CHECK(ReadAll(path) == "resolution_scale = 3\nkeybind_a = \"Space\"\n");
  auto tmp = path;
  tmp += ".tmp";
  CHECK(!std::filesystem::exists(tmp));
  // A directory in the file's place cannot be written: an error, not a crash.
  auto blocked = dir / "blocked.toml";
  std::filesystem::create_directories(blocked);
  CHECK(!doa4::SaveTomlKey(blocked, "a", "1").empty());
  std::filesystem::remove_all(dir);
}

// The SDK loads the file with toml++ and drops ALL of it on a parse error, so a
// save must never leave a file that toml++ rejects or reads differently.
static std::string SaveInto(const std::filesystem::path& path, const std::string& before,
                            const char* key, const char* value) {
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << before;
  }
  return doa4::SaveTomlKey(path, key, value);
}

static void TestSaveKeepsFileLoadable() {
  auto dir = std::filesystem::temp_directory_path() / "doa4_toml_upsert_test_data";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  auto path = dir / "dead_or_alive_4.toml";
  // A UTF-8 BOM (some editors add one) is kept and the key after it is found.
  const std::string bom = "\xEF\xBB\xBF";
  CHECK(SaveInto(path, bom + "fullscreen = false\n", "fullscreen", "true").empty());
  CHECK(ReadAll(path) == bom + "fullscreen = true\n");
  // Shapes the line editor cannot handle are refused and the file is left untouched.
  const std::string quoted = "\"fullscreen\" = false\n";
  CHECK(!SaveInto(path, quoted, "fullscreen", "true").empty());
  CHECK(ReadAll(path) == quoted);
  const std::string array_with_bracket_line = "list = [\n  [1, 2],\n]\n";
  CHECK(!SaveInto(path, array_with_bracket_line, "fullscreen", "true").empty());
  CHECK(ReadAll(path) == array_with_bracket_line);
  const std::string string_with_key_line = "note = \"\"\"\nfullscreen = 1\n\"\"\"\nfullscreen = false\n";
  CHECK(!SaveInto(path, string_with_key_line, "fullscreen", "true").empty());
  CHECK(ReadAll(path) == string_with_key_line);
  // A file that is already broken is not made worse.
  const std::string broken = "fullscreen = \n";
  CHECK(!SaveInto(path, broken, "resolution_scale", "2").empty());
  CHECK(ReadAll(path) == broken);
  auto tmp = path;
  tmp += ".tmp";
  CHECK(!std::filesystem::exists(tmp));
  std::filesystem::remove_all(dir);
}

// The "Age:" box and the age are saved together: one read, both keys upserted into the same text, each key checked
// against the final text, one write. A refusal of either key leaves the file's bytes as they were.
static void TestSaveTwoKeys() {
  auto dir = std::filesystem::temp_directory_path() / "doa4_toml_two_keys_test_data";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  auto path = dir / "dead_or_alive_4.toml";
  auto tmp = path;
  tmp += ".tmp";
  const auto parse = [&](toml::table& table) {
    try {
      table = toml::parse_file(path.u8string());
      return true;
    } catch (const toml::parse_error& e) {
      std::printf("FAIL toml++ rejected the two-key file: %s\n", e.what());
      ++g_failures;
      return false;
    }
  };
  const auto put_file = [&](const std::string& before) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << before;
  };
  toml::table table;

  // (a) No file: both keys land, in list order, each upserted into the text the previous one produced.
  CHECK(doa4::SaveTomlKeys(path, {{"doa4_bust_age_on", "true"}, {"doa4_bust_age", "42"}}).empty());
  if (parse(table)) {
    CHECK(table["doa4_bust_age_on"].value<bool>() == true);
    CHECK(table["doa4_bust_age"].value<int64_t>() == 42);
  }
  CHECK(ReadAll(path) == "doa4_bust_age_on = true\ndoa4_bust_age = 42\n");

  // (b) A CRLF file stays CRLF; the other key is kept.
  put_file("fullscreen = true\r\ndoa4_bust_age = 18\r\n");
  CHECK(doa4::SaveTomlKeys(path, {{"doa4_bust_age_on", "false"}, {"doa4_bust_age", "60"}}).empty());
  if (parse(table)) {
    CHECK(table["fullscreen"].value<bool>() == true);
    CHECK(table["doa4_bust_age_on"].value<bool>() == false);
    CHECK(table["doa4_bust_age"].value<int64_t>() == 60);
  }
  {
    const std::string text = ReadAll(path);
    bool crlf = true;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf = false;
    }
    CHECK(crlf);
  }

  // (c) The SECOND key refused: the editor rewrites the line inside the multi-line string, the file still parses and
  // the real key keeps 5, so only the per-key check can refuse. Nothing is written, the box included.
  // The error names the refused key; the closing parenthesis keeps doa4_bust_age apart from doa4_bust_age_on.
  const std::string second_refused = "note = \"\"\"\ndoa4_bust_age = 1\n\"\"\"\ndoa4_bust_age = 5\n";
  put_file(second_refused);
  const std::string second_error =
      doa4::SaveTomlKeys(path, {{"doa4_bust_age_on", "true"}, {"doa4_bust_age", "60"}});
  CHECK(!second_error.empty());
  CHECK(second_error.find("(doa4_bust_age)") != std::string::npos);
  CHECK(ReadAll(path) == second_refused);

  // (d) The FIRST key refused the same way.
  const std::string first_refused = "note = \"\"\"\ndoa4_bust_age_on = 1\n\"\"\"\ndoa4_bust_age_on = false\n";
  put_file(first_refused);
  const std::string first_error =
      doa4::SaveTomlKeys(path, {{"doa4_bust_age_on", "true"}, {"doa4_bust_age", "60"}});
  CHECK(!first_error.empty());
  CHECK(first_error.find("(doa4_bust_age_on)") != std::string::npos);
  CHECK(ReadAll(path) == first_refused);

  // (e) An empty list is an error and creates no file.
  auto empty_path = dir / "empty.toml";
  CHECK(!doa4::SaveTomlKeys(empty_path, {}).empty());
  CHECK(!std::filesystem::exists(empty_path));

  // (f) Every later save: both lines replaced in place, box line first, no third line. doa4_bust_age is a prefix of
  // doa4_bust_age_on; the whole trimmed key is compared.
  put_file("doa4_bust_age_on = true\ndoa4_bust_age = 7\n");
  CHECK(doa4::SaveTomlKeys(path, {{"doa4_bust_age_on", "false"}, {"doa4_bust_age", "60"}}).empty());
  CHECK(ReadAll(path) == "doa4_bust_age_on = false\ndoa4_bust_age = 60\n");

  CHECK(!std::filesystem::exists(tmp));
  auto empty_tmp = empty_path;
  empty_tmp += ".tmp";
  CHECK(!std::filesystem::exists(empty_tmp));
  std::filesystem::remove_all(dir);
}

// The game-files folder saved by the first-run setup: written with
// TomlString(Utf8(path)), read back by toml++ (as the SDK does) and turned
// into a path with PathFromUtf8 - the same folder, backslashes and non-ANSI
// names included. The folders are fictitious (only stored and compared, never
// opened); the first keeps \U, \f, \d, \x and \D, each of which a TOML basic
// string rejects or decodes as an escape unless the backslash is escaped.
static void TestGameFilesPathRoundTrip() {
  auto dir = std::filesystem::temp_directory_path() / "doa4_toml_upsert_test_data";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  auto path = dir / "dead_or_alive_4.toml";
  const std::filesystem::path folders[] = {
      std::filesystem::path(L"C:\\Users\\fred\\doa4\\xex\\DOA4"),
      std::filesystem::path(u8"D:\\\u30b2\u30fc\u30e0\\DOA4 \"copy\""),
  };
  for (const auto& folder : folders) {
    CHECK(SaveInto(path, "fullscreen = true\n", "doa4_game_files",
                   doa4::TomlString(doa4::Utf8(folder)).c_str())
              .empty());
    toml::table table;
    try {
      table = toml::parse_file(path.u8string());
    } catch (const toml::parse_error& e) {
      std::printf("FAIL toml++ rejected the file: %s\n", e.what());
      ++g_failures;
      continue;
    }
    const auto value = table["doa4_game_files"].value<std::string>();
    CHECK(value.has_value());
    if (value) CHECK(doa4::PathFromUtf8(*value) == folder);
    CHECK(table["fullscreen"].value<bool>() == true);
  }
  std::filesystem::remove_all(dir);
}

// The SDK's F4 overlay "Save to config" writes string cvars as
// `name = "<raw value>"` without escaping (rexglue-sdk cvar.cpp), and the
// loader drops the WHOLE file on a parse error (final review M1). The game
// folder is therefore saved in generic form (forward slashes): that raw line
// must parse back to the same folder.
static void TestGameFilesSurvivesTheSdkWriter() {
#if defined(_WIN32)
  const std::filesystem::path folders[] = {
      std::filesystem::path(L"C:\\Users\\fred\\doa4\\xex\\DOA4"),
      std::filesystem::path(u8"D:\\\u30b2\u30fc\u30e0\\DOA4"),
  };
#else  // a backslash is an ordinary name character in a POSIX path: the folders use '/' only
  const std::filesystem::path folders[] = {
      std::filesystem::path("/home/fred/doa4/xex/DOA4"),
      std::filesystem::path(u8"/media/\u30b2\u30fc\u30e0/DOA4"),
  };
#endif
  for (const auto& folder : folders) {
    const std::string text = doa4::Utf8Generic(folder);
    CHECK(text.find('\\') == std::string::npos);
    const std::string sdk_line = "doa4_game_files = \"" + text + "\"\n";
    try {
      toml::table table = toml::parse(sdk_line);
      const auto value = table["doa4_game_files"].value<std::string>();
      CHECK(value == text);
      if (value) CHECK(doa4::PathFromUtf8(*value) == folder);
    } catch (const toml::parse_error& e) {
      std::printf("FAIL the SDK writer's line does not parse: %s\n", e.what());
      ++g_failures;
    }
  }
}

// Task 7: the vanilla box's writer (SetPresentationSetting -> SaveTomlKey, as doa4_skip_intro's) turned on and then off,
// on a start file the writer would never produce itself (a comment, a key without spaces, keys out of order, a blank line
// at the end). Afterwards every start line is there unchanged and in order, and the one other line is the box's own.
// Excludes: a writer that rewrites (canonicalises, reorders or drops) any other line, and one that adds a second line on
// the second save instead of replacing its own.
static void TestVanillaKeySave() {
  std::printf("-- TestVanillaKeySave\n");
  auto dir = std::filesystem::temp_directory_path() / "doa4_toml_vanilla_test_data";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  auto path = dir / "dead_or_alive_4.toml";
  const std::string start =
      "# my settings\n"
      "doa4_skip_intro=true\n"
      "fullscreen = false\n"
      "  doa4_alpha152 = true\n"
      "apu = \"any\"\n"
      "\n";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << start;
  }
  const std::string on_error = doa4::SaveTomlKey(path, "doa4_vanilla_only", "true");
  CHECK(on_error.empty());
  const std::string after_on = ReadAll(path);
  CHECK(after_on == start + "doa4_vanilla_only = true\n");
  const std::string off_error = doa4::SaveTomlKey(path, "doa4_vanilla_only", "false");
  CHECK(off_error.empty());
  const std::string after_off = ReadAll(path);
  CHECK(after_off == start + "doa4_vanilla_only = false\n");
  // The same, line by line (a failure here names the kind of change): the start's lines first, in order, then the one.
  {
    std::vector<std::string> lines;
    for (size_t i = 0; i < after_off.size();) {
      size_t end = after_off.find('\n', i);
      if (end == std::string::npos) end = after_off.size();
      lines.push_back(after_off.substr(i, end - i));
      i = end + 1;
    }
    const std::vector<std::string> want = {"# my settings", "doa4_skip_intro=true", "fullscreen = false",
                                           "  doa4_alpha152 = true", "apu = \"any\"", "", "doa4_vanilla_only = false"};
    CHECK(lines == want);
  }
  auto tmp = path;
  tmp += ".tmp";
  CHECK(!std::filesystem::exists(tmp));
  std::filesystem::remove_all(dir);
}

int main() {
  TestConfigAfterSession();
  TestUpsert();
  TestTomlString();
  try {
    TestSave();
    TestSaveKeepsFileLoadable();
    TestSaveTwoKeys();
    TestGameFilesPathRoundTrip();
    TestGameFilesSurvivesTheSdkWriter();
    TestVanillaKeySave();
  } catch (const std::exception& e) {
    std::printf("FAIL TestSave threw: %s\n", e.what());
    ++g_failures;
  }
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("toml_upsert: all checks passed\n");
  return 0;
}
