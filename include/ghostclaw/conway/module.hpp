#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"

#include <string>

namespace ghostclaw::conway {

enum class SurvivalTier {
  Normal,      // Full capabilities, balance > low_compute_threshold
  LowCompute,  // Reduced capabilities, balance between critical and low_compute
  Critical,    // Minimal operations, balance near zero
  Dead,        // Balance is zero, agent should stop non-essential work
};

struct ConwayStatus {
  bool available = false;
  std::string wallet_address;
  double credits_usd = 0.0;
  double usdc_balance = 0.0;
  SurvivalTier tier = SurvivalTier::Normal;
  std::string error;
};

[[nodiscard]] std::string survival_tier_to_string(SurvivalTier tier);
[[nodiscard]] SurvivalTier compute_survival_tier(const config::ConwayConfig &config,
                                                  double credits_usd);

// Read the Conway wallet address from the wallet file
[[nodiscard]] common::Result<std::string> read_wallet_address(const config::ConwayConfig &config);

// Display a human-readable Conway status block
[[nodiscard]] std::string format_status(const ConwayStatus &status);

} // namespace ghostclaw::conway
