#include "ghostclaw/voice/wake.hpp"

#include "ghostclaw/common/fs.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ghostclaw::voice {

namespace {

bool is_token_boundary(char ch) {
  return !std::isalnum(static_cast<unsigned char>(ch));
}

std::string trim_leading_separators(const std::string &value) {
  std::size_t pos = 0;
  while (pos < value.size()) {
    const char ch = value[pos];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == ':' || ch == ',' ||
        ch == '-' || ch == ';' || ch == '.') {
      ++pos;
      continue;
    }
    break;
  }
  return common::trim(value.substr(pos));
}

} // namespace

WakeWordDetector::WakeWordDetector(WakeWordConfig config) : config_(std::move(config)) {}

void WakeWordDetector::set_config(WakeWordConfig config) { config_ = std::move(config); }

const WakeWordConfig &WakeWordDetector::config() const { return config_; }

std::string WakeWordDetector::normalize_for_match(std::string value,
                                                  const bool case_sensitive) {
  value = common::trim(std::move(value));
  if (!case_sensitive) {
    value = common::to_lower(std::move(value));
  }
  return value;
}

WakeMatch WakeWordDetector::detect(const std::string_view text) const {
  WakeMatch match;
  match.original_text = std::string(text);

  const std::string normalized_text =
      normalize_for_match(std::string(text), config_.case_sensitive);
  if (normalized_text.empty()) {
    return match;
  }

  std::size_t best_pos = std::string::npos;
  std::size_t best_len = 0;
  std::string best_wake_word;

  for (const std::string &raw_wake_word : config_.wake_words) {
    const std::string wake_word = normalize_for_match(raw_wake_word, config_.case_sensitive);
    if (wake_word.empty()) {
      continue;
    }

    std::size_t search_pos = 0;
    while (search_pos < normalized_text.size()) {
      const std::size_t found = normalized_text.find(wake_word, search_pos);
      if (found == std::string::npos) {
        break;
      }

      const bool valid_start =
          (found == 0) || is_token_boundary(normalized_text[found - 1]);
      const std::size_t end = found + wake_word.size();
      const bool valid_end =
          (end >= normalized_text.size()) || is_token_boundary(normalized_text[end]);
      if (valid_start && valid_end && (best_pos == std::string::npos || found < best_pos)) {
        best_pos = found;
        best_len = wake_word.size();
        best_wake_word = raw_wake_word;
      }
      search_pos = found + 1;
    }
  }

  if (best_pos == std::string::npos) {
    return match;
  }

  match.detected = true;
  match.wake_word = best_wake_word;
  match.position = best_pos;

  const std::size_t original_pos = best_pos;
  const std::size_t after_wake = original_pos + best_len;
  if (after_wake <= match.original_text.size()) {
    match.command_text = trim_leading_separators(match.original_text.substr(after_wake));
  }
  return match;
}

common::Status PushToTalkBuffer::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = true;
  chunks_.clear();
  return common::Status::success();
}

void PushToTalkBuffer::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = false;
}

bool PushToTalkBuffer::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

common::Status PushToTalkBuffer::feed(const std::string_view chunk) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return common::Status::error("push-to-talk is not active");
  }
  const std::string trimmed = common::trim(std::string(chunk));
  if (!trimmed.empty()) {
    chunks_.push_back(trimmed);
  }
  return common::Status::success();
}

std::string PushToTalkBuffer::consume() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream out;
  for (std::size_t i = 0; i < chunks_.size(); ++i) {
    if (i > 0) {
      out << ' ';
    }
    out << chunks_[i];
  }
  chunks_.clear();
  return common::trim(out.str());
}

void PushToTalkBuffer::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  chunks_.clear();
}

VoiceWakeController::VoiceWakeController(WakeWordConfig config) : detector_(std::move(config)) {}

WakeWordDetector &VoiceWakeController::detector() { return detector_; }

const WakeWordDetector &VoiceWakeController::detector() const { return detector_; }

PushToTalkBuffer &VoiceWakeController::push_to_talk() { return push_to_talk_; }

const PushToTalkBuffer &VoiceWakeController::push_to_talk() const { return push_to_talk_; }

VoiceInputEvent VoiceWakeController::process_transcript(const std::string_view text,
                                                        const bool final_chunk,
                                                        const bool use_push_to_talk) {
  VoiceInputEvent event;

  if (use_push_to_talk) {
    const auto status = push_to_talk_.feed(text);
    if (!status.ok()) {
      return event;
    }
    if (!final_chunk) {
      return event;
    }

    event.type = VoiceInputEventType::PushToTalk;
    event.text = push_to_talk_.consume();
    return event;
  }

  if (!final_chunk) {
    return event;
  }

  const WakeMatch wake = detector_.detect(text);
  if (!wake.detected) {
    return event;
  }

  event.type = VoiceInputEventType::WakeWord;
  event.text = wake.command_text;
  event.wake_word = wake.wake_word;
  return event;
}

} // namespace ghostclaw::voice
