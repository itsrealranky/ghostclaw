#include "ghostclaw/browser/sessions.hpp"

#include "ghostclaw/common/json_util.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace ghostclaw::browser {

SessionPersistence::SessionPersistence(std::string session_dir)
    : session_dir_(std::move(session_dir)) {}

std::string SessionPersistence::session_file_path() const {
  return session_dir_ + "/sessions.json";
}

common::Status SessionPersistence::save(const std::vector<SavedTab> &tabs) {
  try {
    std::filesystem::create_directories(session_dir_);
  } catch (const std::exception &e) {
    return common::Status::error(std::string("failed to create session dir: ") + e.what());
  }

  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << "{\"url\":\"" << common::json_escape(tabs[i].url) << "\","
        << "\"title\":\"" << common::json_escape(tabs[i].title) << "\"}";
  }
  out << "]";

  std::ofstream file(session_file_path());
  if (!file.is_open()) {
    return common::Status::error("failed to open session file for writing");
  }
  file << out.str();
  file.close();
  return common::Status::success();
}

common::Result<std::vector<SavedTab>> SessionPersistence::load() {
  const std::string path = session_file_path();
  if (!std::filesystem::exists(path)) {
    return common::Result<std::vector<SavedTab>>::success({});
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    return common::Result<std::vector<SavedTab>>::failure(
        "failed to open session file for reading");
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  file.close();

  if (content.empty() || content == "[]") {
    return common::Result<std::vector<SavedTab>>::success({});
  }

  auto objects = common::json_split_top_level_objects(content);
  std::vector<SavedTab> tabs;
  tabs.reserve(objects.size());
  for (const auto &obj : objects) {
    SavedTab tab;
    tab.url = common::json_get_string(obj, "url");
    tab.title = common::json_get_string(obj, "title");
    tabs.push_back(std::move(tab));
  }
  return common::Result<std::vector<SavedTab>>::success(std::move(tabs));
}

common::Status SessionPersistence::clear() {
  const std::string path = session_file_path();
  if (std::filesystem::exists(path)) {
    try {
      std::filesystem::remove(path);
    } catch (const std::exception &e) {
      return common::Status::error(std::string("failed to remove session file: ") + e.what());
    }
  }
  return common::Status::success();
}

common::Status
SessionPersistence::patch_chrome_prefs(const std::string &user_data_dir) {
  const std::string prefs_path = user_data_dir + "/Default/Preferences";
  if (!std::filesystem::exists(prefs_path)) {
    return common::Status::success(); // nothing to patch
  }

  std::ifstream in(prefs_path);
  if (!in.is_open()) {
    return common::Status::error("failed to open Chrome Preferences for patching");
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  in.close();

  const std::string crashed = "\"exit_type\":\"Crashed\"";
  const std::string normal = "\"exit_type\":\"Normal\"";
  auto pos = content.find(crashed);
  if (pos == std::string::npos) {
    return common::Status::success(); // already normal or not present
  }
  content.replace(pos, crashed.size(), normal);

  std::ofstream out(prefs_path);
  if (!out.is_open()) {
    return common::Status::error("failed to write patched Chrome Preferences");
  }
  out << content;
  out.close();
  return common::Status::success();
}

} // namespace ghostclaw::browser
