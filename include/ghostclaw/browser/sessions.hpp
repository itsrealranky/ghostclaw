#pragma once

#include "ghostclaw/common/result.hpp"

#include <string>
#include <vector>

namespace ghostclaw::browser {

struct SavedTab {
  std::string url;
  std::string title;
};

class SessionPersistence {
public:
  explicit SessionPersistence(std::string session_dir);

  /// Save open tabs to sessions.json.
  [[nodiscard]] common::Status save(const std::vector<SavedTab> &tabs);

  /// Load previously saved tabs from sessions.json.
  [[nodiscard]] common::Result<std::vector<SavedTab>> load();

  /// Remove the sessions.json file.
  [[nodiscard]] common::Status clear();

  /// Patch Chrome's Preferences file to fix "Crashed" exit_type.
  [[nodiscard]] static common::Status
  patch_chrome_prefs(const std::string &user_data_dir);

private:
  [[nodiscard]] std::string session_file_path() const;
  std::string session_dir_;
};

} // namespace ghostclaw::browser
