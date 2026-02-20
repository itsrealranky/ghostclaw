#include "ghostclaw/conway/module.hpp"

#include "ghostclaw/common/fs.hpp"

#include <fstream>
#include <regex>
#include <sstream>

namespace ghostclaw::conway {

std::string survival_tier_to_string(SurvivalTier tier) {
  switch (tier) {
  case SurvivalTier::Normal:
    return "normal";
  case SurvivalTier::LowCompute:
    return "low_compute";
  case SurvivalTier::Critical:
    return "critical";
  case SurvivalTier::Dead:
    return "dead";
  }
  return "unknown";
}

SurvivalTier compute_survival_tier(const config::ConwayConfig &config, double credits_usd) {
  if (credits_usd <= 0.0) {
    return SurvivalTier::Dead;
  }
  if (credits_usd < config.critical_threshold_usd) {
    return SurvivalTier::Critical;
  }
  if (credits_usd < config.low_compute_threshold_usd) {
    return SurvivalTier::LowCompute;
  }
  return SurvivalTier::Normal;
}

common::Result<std::string> read_wallet_address(const config::ConwayConfig &config) {
  const std::string wallet_path = common::expand_path(config.wallet_path);
  if (!std::filesystem::exists(wallet_path)) {
    return common::Result<std::string>::failure(
        "Conway wallet not found at " + wallet_path +
        ". Run 'ghostclaw conway setup' to initialize.");
  }

  std::ifstream in(wallet_path);
  if (!in) {
    return common::Result<std::string>::failure("Failed to read Conway wallet file");
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string content = buffer.str();

  // Extract "address" field from JSON: { "address": "0x..." }
  std::regex addr_re(R"re("address"\s*:\s*"(0x[0-9a-fA-F]{40})")re");
  std::smatch match;
  if (std::regex_search(content, match, addr_re)) {
    return common::Result<std::string>::success(match[1].str());
  }

  return common::Result<std::string>::failure("Could not parse wallet address from wallet file");
}

std::string format_status(const ConwayStatus &status) {
  std::ostringstream out;
  if (!status.available) {
    out << "Conway: unavailable";
    if (!status.error.empty()) {
      out << " (" << status.error << ")";
    }
    out << "\n";
    return out.str();
  }

  out << "Conway: enabled\n";
  if (!status.wallet_address.empty()) {
    out << "  Wallet: " << status.wallet_address << "\n";
  }
  out << "  Credits: $" << status.credits_usd << " USD\n";
  out << "  USDC: " << status.usdc_balance << " USDC\n";
  out << "  Survival tier: " << survival_tier_to_string(status.tier) << "\n";
  return out.str();
}

} // namespace ghostclaw::conway
