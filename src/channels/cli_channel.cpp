#include "ghostclaw/channels/cli_channel.hpp"

#include <ctime>
#include <iostream>

namespace ghostclaw::channels {

CliChannel::~CliChannel() { stop(); }

std::string_view CliChannel::name() const { return "cli"; }

common::Status CliChannel::start() {
  if (running_) {
    return common::Status::success();
  }

  running_ = true;
  input_thread_ = std::thread([this]() {
    while (running_) {
      std::string line;
      if (!std::getline(std::cin, line)) {
        break;
      }
      if (line.empty()) {
        continue;
      }

      MessageCallback callback_copy;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_copy = callback_;
      }
      if (callback_copy) {
        ChannelMessage msg;
        msg.id = std::to_string(std::time(nullptr));
        msg.sender = "stdin";
        msg.channel = "cli";
        msg.content = line;
        msg.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
        callback_copy(msg);
      }
    }
    running_ = false;
  });

  return common::Status::success();
}

void CliChannel::stop() {
  running_ = false;
  if (input_thread_.joinable()) {
    try {
      input_thread_.detach();
    } catch (const std::system_error &) {
      // Thread may have already finished; safe to ignore.
    }
  }
}

common::Status CliChannel::send(const std::string &recipient, const std::string &message) {
  (void)recipient;
  std::cout << message << "\n";
  return common::Status::success();
}

void CliChannel::on_message(MessageCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  callback_ = std::move(callback);
}

bool CliChannel::health_check() { return running_ || !input_thread_.joinable(); }

bool CliChannel::supports_streaming() const { return true; }

common::Status CliChannel::stream_chunk(const std::string &, std::string_view text) {
  std::cout << text;
  std::cout.flush();
  return common::Status::success();
}

void CliChannel::inject_for_test(const ChannelMessage &message) {
  MessageCallback callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = callback_;
  }
  if (callback_copy) {
    callback_copy(message);
  }
}

} // namespace ghostclaw::channels
