#include "ghostclaw/canvas/host.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

namespace ghostclaw::canvas {

namespace {

constexpr std::size_t kMaxHtmlBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxJavaScriptBytes = 256U * 1024U;
constexpr std::size_t kMaxRecentScripts = 32U;

std::uint64_t unix_time_ms_now() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  if (ms <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(ms);
}

void trim_view_prefix(std::string_view &view) {
  while (!view.empty() &&
         (view.front() == ' ' || view.front() == '\t' || view.front() == '\n' ||
          view.front() == '\r')) {
    view.remove_prefix(1U);
  }
}

void trim_view(std::string_view &view) {
  trim_view_prefix(view);
  while (!view.empty() &&
         (view.back() == ' ' || view.back() == '\t' || view.back() == '\n' ||
          view.back() == '\r')) {
    view.remove_suffix(1U);
  }
}

common::Result<std::pair<std::string, std::size_t>>
parse_js_string_literal(std::string_view input, std::size_t start_index) {
  if (start_index >= input.size()) {
    return common::Result<std::pair<std::string, std::size_t>>::failure(
        "expected JavaScript string literal");
  }

  const char quote = input[start_index];
  if (quote != '"' && quote != '\'') {
    return common::Result<std::pair<std::string, std::size_t>>::failure(
        "expected JavaScript string literal");
  }

  std::string out;
  out.reserve(input.size() - start_index);

  bool escaping = false;
  for (std::size_t i = start_index + 1U; i < input.size(); ++i) {
    const char ch = input[i];
    if (escaping) {
      switch (ch) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '\\':
      case '\'':
      case '"':
        out.push_back(ch);
        break;
      default:
        out.push_back(ch);
        break;
      }
      escaping = false;
      continue;
    }

    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == quote) {
      return common::Result<std::pair<std::string, std::size_t>>::success(
          std::make_pair(std::move(out), i + 1U));
    }
    out.push_back(ch);
  }

  return common::Result<std::pair<std::string, std::size_t>>::failure(
      "unterminated JavaScript string literal");
}

common::Result<std::vector<std::string>>
parse_js_call_string_args(const std::string &source, const std::string_view function_name,
                          const std::size_t expected_args) {
  std::string working = common::trim(source);
  if (!working.empty() && working.back() == ';') {
    working.pop_back();
    working = common::trim(working);
  }

  const std::string prefix = std::string(function_name) + "(";
  if (!common::starts_with(working, prefix) || working.empty() || working.back() != ')') {
    return common::Result<std::vector<std::string>>::failure("invalid JavaScript call syntax");
  }

  std::string_view args =
      std::string_view(working).substr(prefix.size(), working.size() - prefix.size() - 1U);
  std::vector<std::string> out;
  out.reserve(expected_args);

  std::size_t cursor = 0;
  while (cursor < args.size()) {
    while (cursor < args.size() &&
           (args[cursor] == ' ' || args[cursor] == '\t' || args[cursor] == '\n' ||
            args[cursor] == '\r')) {
      ++cursor;
    }

    if (cursor >= args.size()) {
      break;
    }

    const auto parsed = parse_js_string_literal(args, cursor);
    if (!parsed.ok()) {
      return common::Result<std::vector<std::string>>::failure(parsed.error());
    }

    out.push_back(parsed.value().first);
    cursor = parsed.value().second;

    while (cursor < args.size() &&
           (args[cursor] == ' ' || args[cursor] == '\t' || args[cursor] == '\n' ||
            args[cursor] == '\r')) {
      ++cursor;
    }

    if (cursor >= args.size()) {
      break;
    }

    if (args[cursor] != ',') {
      return common::Result<std::vector<std::string>>::failure("expected ',' between arguments");
    }
    ++cursor;
  }

  if (out.size() != expected_args) {
    return common::Result<std::vector<std::string>>::failure(
        "unexpected JavaScript argument count");
  }
  return common::Result<std::vector<std::string>>::success(std::move(out));
}

common::Result<std::string> parse_assignment_string(const std::string &source,
                                                    const std::string_view prefix) {
  if (!common::starts_with(source, std::string(prefix))) {
    return common::Result<std::string>::failure("assignment prefix mismatch");
  }

  std::string_view rhs = std::string_view(source).substr(prefix.size());
  trim_view(rhs);
  if (!rhs.empty() && rhs.back() == ';') {
    rhs.remove_suffix(1U);
    trim_view(rhs);
  }

  const auto parsed = parse_js_string_literal(rhs, 0);
  if (!parsed.ok()) {
    return common::Result<std::string>::failure(parsed.error());
  }

  if (parsed.value().second != rhs.size()) {
    return common::Result<std::string>::failure("unexpected trailing characters in assignment");
  }

  return common::Result<std::string>::success(parsed.value().first);
}

} // namespace

common::Result<void> CanvasHost::push(const std::string &html) {
  if (html.empty()) {
    return common::Result<void>::failure("html is required");
  }
  if (html.size() > kMaxHtmlBytes) {
    return common::Result<void>::failure("html payload is too large");
  }

  std::unique_lock lock(mutex_);
  html_ = html;
  touch_locked();
  return common::Result<void>::success();
}

common::Result<void> CanvasHost::eval(const std::string &js) {
  std::unique_lock lock(mutex_);
  return apply_eval_locked(js);
}

common::Result<std::string> CanvasHost::snapshot() {
  std::shared_lock lock(mutex_);

  std::ostringstream out;
  out << "{";
  out << "\"version\":" << version_;
  out << ",\"updated_at_ms\":" << updated_at_ms_;
  out << ",\"script_count\":" << script_count_;
  out << ",\"html\":\"" << common::json_escape(html_) << "\"";
  out << ",\"recent_scripts\":[";
  for (std::size_t i = 0; i < recent_scripts_.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << "\"" << common::json_escape(recent_scripts_[i]) << "\"";
  }
  out << "]";
  out << "}";
  return common::Result<std::string>::success(out.str());
}

common::Result<void> CanvasHost::reset() {
  std::unique_lock lock(mutex_);
  html_.clear();
  recent_scripts_.clear();
  script_count_ = 0;
  touch_locked();
  return common::Result<void>::success();
}

common::Result<void> CanvasHost::apply_eval_locked(const std::string &js) {
  const std::string trimmed = common::trim(js);
  if (trimmed.empty()) {
    return common::Result<void>::failure("javascript is required");
  }
  if (trimmed.size() > kMaxJavaScriptBytes) {
    return common::Result<void>::failure("javascript payload is too large");
  }

  if (trimmed == "clear()" || trimmed == "clear();" || trimmed == "reset()" ||
      trimmed == "reset();") {
    html_.clear();
  } else if (common::starts_with(trimmed, "setHtml(")) {
    const auto args = parse_js_call_string_args(trimmed, "setHtml", 1U);
    if (!args.ok()) {
      return common::Result<void>::failure(args.error());
    }
    html_ = args.value()[0];
  } else if (common::starts_with(trimmed, "appendHtml(")) {
    const auto args = parse_js_call_string_args(trimmed, "appendHtml", 1U);
    if (!args.ok()) {
      return common::Result<void>::failure(args.error());
    }
    if (html_.size() + args.value()[0].size() > kMaxHtmlBytes) {
      return common::Result<void>::failure("resulting html payload is too large");
    }
    html_.append(args.value()[0]);
  } else if (common::starts_with(trimmed, "prependHtml(")) {
    const auto args = parse_js_call_string_args(trimmed, "prependHtml", 1U);
    if (!args.ok()) {
      return common::Result<void>::failure(args.error());
    }
    if (html_.size() + args.value()[0].size() > kMaxHtmlBytes) {
      return common::Result<void>::failure("resulting html payload is too large");
    }
    html_.insert(0, args.value()[0]);
  } else if (common::starts_with(trimmed, "replaceHtml(")) {
    const auto args = parse_js_call_string_args(trimmed, "replaceHtml", 2U);
    if (!args.ok()) {
      return common::Result<void>::failure(args.error());
    }
    const auto pos = html_.find(args.value()[0]);
    if (pos != std::string::npos) {
      html_.replace(pos, args.value()[0].size(), args.value()[1]);
    }
  } else if (common::starts_with(trimmed, "document.body.innerHTML =")) {
    const auto value = parse_assignment_string(trimmed, "document.body.innerHTML =");
    if (!value.ok()) {
      return common::Result<void>::failure(value.error());
    }
    if (value.value().size() > kMaxHtmlBytes) {
      return common::Result<void>::failure("resulting html payload is too large");
    }
    html_ = value.value();
  } else if (common::starts_with(trimmed, "document.body.innerHTML +=")) {
    const auto value = parse_assignment_string(trimmed, "document.body.innerHTML +=");
    if (!value.ok()) {
      return common::Result<void>::failure(value.error());
    }
    if (html_.size() + value.value().size() > kMaxHtmlBytes) {
      return common::Result<void>::failure("resulting html payload is too large");
    }
    html_.append(value.value());
  } else {
    // Keep unsupported scripts as successful no-ops so agents can still call eval
    // without being coupled to one execution backend.
  }

  append_script_locked(trimmed);
  touch_locked();
  return common::Result<void>::success();
}

void CanvasHost::append_script_locked(const std::string &js) {
  ++script_count_;
  if (recent_scripts_.size() >= kMaxRecentScripts) {
    recent_scripts_.erase(recent_scripts_.begin());
  }
  recent_scripts_.push_back(js);
}

void CanvasHost::touch_locked() {
  ++version_;
  updated_at_ms_ = unix_time_ms_now();
}

} // namespace ghostclaw::canvas
