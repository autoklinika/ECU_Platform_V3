#pragma once

#include "ecu/core_v2/runtime/configuration_gate.hpp"
#include "ecu/core_v2/runtime/lifecycle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

using ModuleId = std::uint32_t;
using ModuleCapabilityMask = std::uint64_t;

enum class ModuleKind : std::uint8_t {
  core_service,
  protocol,
  dut_profile,
  bench_service,
  application,
};

struct ModuleDescriptor {
  ModuleId id{0U};
  ModuleKind kind{ModuleKind::application};
  ModuleCapabilityMask capabilities{0U};
};

[[nodiscard]] constexpr bool is_valid_module_descriptor(
    const ModuleDescriptor& descriptor) noexcept {
  return descriptor.id != 0U;
}

class ICoreModule : public ILifecycleComponent {
 public:
  [[nodiscard]] virtual ModuleDescriptor descriptor()
      const noexcept = 0;

 protected:
  ~ICoreModule() = default;
};

enum class ModuleRegistrationStatus : std::uint8_t {
  registered,
  already_registered,
  id_conflict,
  invalid_argument,
  configuration_frozen,
  capacity_exhausted,
};

class ModuleRegistry final {
 public:
  static constexpr std::size_t kMaxModules = 64U;

  [[nodiscard]] ModuleRegistrationStatus register_module(
      ICoreModule& module) noexcept;

  [[nodiscard]] bool freeze_configuration() noexcept;

  [[nodiscard]] ICoreModule* find(ModuleId id) noexcept;
  [[nodiscard]] const ICoreModule* find(
      ModuleId id) const noexcept;

  [[nodiscard]] std::size_t module_count() const noexcept;
  [[nodiscard]] ConfigurationState configuration_state()
      const noexcept;

 private:
  ConfigurationGate configuration_{};
  std::array<ICoreModule*, kMaxModules> modules_{};
};

}  // namespace ecu::core::v2::runtime
