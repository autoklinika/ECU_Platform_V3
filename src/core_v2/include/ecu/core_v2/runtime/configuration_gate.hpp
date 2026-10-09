#pragma once

#include <cstdint>

namespace ecu::core::v2::runtime {

enum class ConfigurationState : std::uint8_t {
  configuring,
  frozen,
};

class ConfigurationGate {
 public:
  [[nodiscard]] ConfigurationState state() const noexcept {
    return state_;
  }

  [[nodiscard]] bool accepts_registration() const noexcept {
    return state_ == ConfigurationState::configuring;
  }

  [[nodiscard]] bool freeze() noexcept {
    if (state_ != ConfigurationState::configuring) {
      return false;
    }
    state_ = ConfigurationState::frozen;
    return true;
  }

 private:
  ConfigurationState state_{ConfigurationState::configuring};
};

}  // namespace ecu::core::v2::runtime
