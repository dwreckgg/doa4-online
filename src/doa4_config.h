// Writes settings into dead_or_alive_4.toml (the config the SDK loads
// at start-up) without touching the rest of the file. rex::cvar::SaveConfig is
// not used: it writes every non-default cvar, including the launcher's
// --game_data_root / --user_data_root.

#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace doa4 {

// `text` with the top-level line for `key` set to `key = value_literal`.
// A top-level line is one before the first [table] header; its key is the
// text before its first '=', trimmed. Other lines are kept byte for byte.
// A missing key is inserted before the first table header, or appended (a
// missing final newline is added first). New lines use "\r\n" if `text`
// contains one, else "\n".
std::string UpsertTomlKey(std::string_view text, std::string_view key,
                          std::string_view value_literal);

// A TOML basic string: "..." with \ and " escaped.
std::string TomlString(std::string_view value);

// One `key = value_literal` line for SaveTomlKeys.
struct TomlKey {
  std::string_view key;
  std::string_view value_literal;
};

// The file at `path` (created if missing) read once, every key upserted with
// UpsertTomlKey in list order into the same text, every key checked against
// the final text, then written once to `path` + ".tmp" and renamed over
// `path`. Returns "" on success, else a description of the failure, with the
// file untouched. An empty list is an error.
std::string SaveTomlKeys(const std::filesystem::path& path,
                         std::initializer_list<TomlKey> keys);

// SaveTomlKeys with the one key.
std::string SaveTomlKey(const std::filesystem::path& path, std::string_view key,
                        std::string_view value_literal);

// The config a netplay session leaves behind: `snapshot` (the file at session
// start, "" if there was none) with each kept (key, value_literal) applied by
// UpsertTomlKey in list order. The list is the player's own controller saves
// made with the Esc menu during the session, in the order they were made, so a
// key the snapshot lacked lands where the save put it.
std::string ConfigAfterSession(std::string_view snapshot,
                               const std::vector<std::pair<std::string, std::string>>& kept);

}  // namespace doa4
