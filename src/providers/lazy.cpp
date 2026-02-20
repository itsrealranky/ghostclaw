#include "ghostclaw/providers/lazy.hpp"

namespace ghostclaw::providers {

LazyProvider::LazyProvider(std::string pending_name, ProviderFactory factory)
    : pending_name_(std::move(pending_name)), factory_(std::move(factory)) {}

common::Result<std::shared_ptr<Provider>> LazyProvider::ensure_provider() const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (provider_ != nullptr) {
      return common::Result<std::shared_ptr<Provider>>::success(provider_);
    }
    if (init_error_.has_value()) {
      return common::Result<std::shared_ptr<Provider>>::failure(*init_error_);
    }
  }

  auto created = factory_();
  if (!created.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    init_error_ = created.error();
    return common::Result<std::shared_ptr<Provider>>::failure(created.error());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    provider_ = created.value();
  }
  return common::Result<std::shared_ptr<Provider>>::success(created.value());
}

common::Result<std::string> LazyProvider::chat(const std::string &message, const std::string &model,
                                                const double temperature) {
  auto provider = ensure_provider();
  if (!provider.ok()) {
    return common::Result<std::string>::failure(provider.error());
  }
  return provider.value()->chat(message, model, temperature);
}

common::Result<std::string>
LazyProvider::chat_with_system(const std::optional<std::string> &system_prompt,
                               const std::string &message, const std::string &model,
                               const double temperature) {
  auto provider = ensure_provider();
  if (!provider.ok()) {
    return common::Result<std::string>::failure(provider.error());
  }
  return provider.value()->chat_with_system(system_prompt, message, model, temperature);
}

common::Result<std::string> LazyProvider::chat_with_system_tools(
    const std::optional<std::string> &system_prompt, const std::string &message,
    const std::string &model, const double temperature,
    const std::vector<tools::ToolSpec> &tools) {
  auto provider = ensure_provider();
  if (!provider.ok()) {
    return common::Result<std::string>::failure(provider.error());
  }
  return provider.value()->chat_with_system_tools(system_prompt, message, model, temperature,
                                                  tools);
}

common::Result<std::string>
LazyProvider::chat_with_system_stream(const std::optional<std::string> &system_prompt,
                                      const std::string &message, const std::string &model,
                                      const double temperature,
                                      const StreamChunkCallback &on_chunk) {
  auto provider = ensure_provider();
  if (!provider.ok()) {
    return common::Result<std::string>::failure(provider.error());
  }
  return provider.value()->chat_with_system_stream(system_prompt, message, model, temperature,
                                                   on_chunk);
}

common::Status LazyProvider::warmup() {
  auto provider = ensure_provider();
  if (!provider.ok()) {
    return common::Status::error(provider.error());
  }
  return provider.value()->warmup();
}

std::string LazyProvider::name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (provider_ != nullptr) {
    return provider_->name();
  }
  return pending_name_;
}

} // namespace ghostclaw::providers
