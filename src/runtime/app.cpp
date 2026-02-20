#include "ghostclaw/runtime/app.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/memory/memory.hpp"
#include "ghostclaw/observability/factory.hpp"
#include "ghostclaw/observability/global.hpp"
#include "ghostclaw/providers/factory.hpp"
#include "ghostclaw/providers/lazy.hpp"
#include "ghostclaw/security/policy.hpp"
#include "ghostclaw/tools/tool_registry.hpp"

#include <mutex>
#include <optional>

namespace ghostclaw::runtime {

namespace {

class LazyMemory final : public memory::IMemory {
public:
  LazyMemory(config::Config config, std::filesystem::path workspace)
      : config_(std::move(config)), workspace_(std::move(workspace)) {}

  [[nodiscard]] std::string_view name() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_ != nullptr) {
      return impl_->name();
    }
    static constexpr std::string_view kLazyName = "lazy-memory";
    return kLazyName;
  }

  [[nodiscard]] common::Status store(const std::string &key, const std::string &content,
                                     memory::MemoryCategory category) override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return common::Status::error(impl.error());
    }
    return impl.value()->store(key, content, category);
  }

  [[nodiscard]] common::Result<std::vector<memory::MemoryEntry>>
  recall(const std::string &query, std::size_t limit) override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return common::Result<std::vector<memory::MemoryEntry>>::failure(impl.error());
    }
    return impl.value()->recall(query, limit);
  }

  [[nodiscard]] common::Result<std::optional<memory::MemoryEntry>>
  get(const std::string &key) override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return common::Result<std::optional<memory::MemoryEntry>>::failure(impl.error());
    }
    return impl.value()->get(key);
  }

  [[nodiscard]] common::Result<std::vector<memory::MemoryEntry>>
  list(std::optional<memory::MemoryCategory> category) override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return common::Result<std::vector<memory::MemoryEntry>>::failure(impl.error());
    }
    return impl.value()->list(category);
  }

  [[nodiscard]] common::Result<bool> forget(const std::string &key) override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return common::Result<bool>::failure(impl.error());
    }
    return impl.value()->forget(key);
  }

  [[nodiscard]] common::Result<std::size_t> count() override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return common::Result<std::size_t>::failure(impl.error());
    }
    return impl.value()->count();
  }

  [[nodiscard]] common::Status reindex() override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return common::Status::error(impl.error());
    }
    return impl.value()->reindex();
  }

  [[nodiscard]] bool health_check() override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return false;
    }
    return impl.value()->health_check();
  }

  [[nodiscard]] memory::MemoryStats stats() override {
    auto impl = ensure_impl();
    if (!impl.ok()) {
      return {};
    }
    return impl.value()->stats();
  }

private:
  [[nodiscard]] common::Result<memory::IMemory *> ensure_impl() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (impl_ != nullptr) {
        return common::Result<memory::IMemory *>::success(impl_.get());
      }
      if (init_error_.has_value()) {
        return common::Result<memory::IMemory *>::failure(*init_error_);
      }
    }

    auto created = memory::create_memory(config_, workspace_);
    if (created == nullptr) {
      std::lock_guard<std::mutex> lock(mutex_);
      init_error_ = "failed to create memory backend";
      return common::Result<memory::IMemory *>::failure(*init_error_);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      impl_ = std::move(created);
      return common::Result<memory::IMemory *>::success(impl_.get());
    }
  }

  config::Config config_;
  std::filesystem::path workspace_;
  mutable std::mutex mutex_;
  std::unique_ptr<memory::IMemory> impl_;
  std::optional<std::string> init_error_;
};

} // namespace

RuntimeContext::RuntimeContext(config::Config config) : config_(std::move(config)) {}

common::Result<RuntimeContext> RuntimeContext::from_disk() {
  auto loaded = config::load_config();
  if (!loaded.ok()) {
    return common::Result<RuntimeContext>::failure(loaded.error());
  }
  return common::Result<RuntimeContext>::success(RuntimeContext(std::move(loaded.value())));
}

const config::Config &RuntimeContext::config() const { return config_; }

config::Config &RuntimeContext::mutable_config() { return config_; }

common::Result<std::shared_ptr<agent::AgentEngine>> RuntimeContext::create_agent_engine() {
  observability::set_global_observer(observability::create_observer(config_));

  auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(workspace.error());
  }

  auto precompiled = providers::precompile_provider_state(config_, workspace.value());
  if (!precompiled.ok()) {
    observability::record_error("runtime", "provider precompile failed: " + precompiled.error());
  }

  auto http_client = std::make_shared<providers::CurlHttpClient>();
  auto provider = std::make_shared<providers::LazyProvider>(
      config_.default_provider,
      [provider_name = config_.default_provider, api_key = config_.api_key,
       reliability = config_.reliability, http_client]() mutable {
        return providers::create_reliable_provider(provider_name, api_key, reliability, http_client);
      });

  auto memory = std::make_unique<LazyMemory>(config_, workspace.value());

  auto policy = security::SecurityPolicy::from_config(config_);
  if (!policy.ok()) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(policy.error());
  }
  auto policy_ptr = std::make_shared<security::SecurityPolicy>(std::move(policy.value()));

  auto registry = tools::ToolRegistry::create_full(policy_ptr, memory.get(), config_);

  auto engine = std::make_shared<agent::AgentEngine>(
      config_, std::move(provider), std::move(memory), std::move(registry), workspace.value());

  return common::Result<std::shared_ptr<agent::AgentEngine>>::success(std::move(engine));
}

} // namespace ghostclaw::runtime
