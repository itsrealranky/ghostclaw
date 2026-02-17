#include "ghostclaw/providers/synthetic.hpp"

#include "ghostclaw/common/fs.hpp"

#include <sstream>

namespace ghostclaw::providers {

namespace {

constexpr std::size_t kPreviewChars = 320;

} // namespace

SyntheticProvider::SyntheticProvider(std::string name) : name_(std::move(name)) {}

common::Result<std::string> SyntheticProvider::chat(const std::string &message,
                                                    const std::string &model,
                                                    const double temperature) {
  return chat_with_system(std::nullopt, message, model, temperature);
}

common::Result<std::string>
SyntheticProvider::chat_with_system(const std::optional<std::string> &system_prompt,
                                    const std::string &message, const std::string &model,
                                    const double temperature) {
  return common::Result<std::string>::success(
      build_response_text(system_prompt, message, model, temperature));
}

common::Status SyntheticProvider::warmup() { return common::Status::success(); }

std::string SyntheticProvider::name() const { return name_; }

std::string SyntheticProvider::summarize_message(const std::string &message) {
  std::string summary = common::trim(message);
  if (summary.empty()) {
    return "(empty prompt)";
  }
  if (summary.size() <= kPreviewChars) {
    return summary;
  }
  summary.resize(kPreviewChars);
  summary += "...";
  return summary;
}

std::string SyntheticProvider::build_response_text(
    const std::optional<std::string> &system_prompt, const std::string &message,
    const std::string &model, const double temperature) {
  std::ostringstream out;
  out << "Synthetic response";
  if (!common::trim(model).empty()) {
    out << " (" << model << ")";
  }
  out << ": " << summarize_message(message);
  if (system_prompt.has_value() && !common::trim(*system_prompt).empty()) {
    out << "\n\n[synthetic-note] System prompt was provided ("
        << std::to_string(common::trim(*system_prompt).size()) << " chars).";
  }
  out << "\n[synthetic-note] temperature=" << temperature;
  return out.str();
}

} // namespace ghostclaw::providers
