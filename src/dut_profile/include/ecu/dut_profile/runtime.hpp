#pragma once

#include "ecu/dut_profile/profile.hpp"

#include <cstdint>

namespace ecu::dut_profile {

struct DutProfileProgramDescriptor {
  std::uint16_t schema_version{DutProfileDefinition::kSchemaVersion};
  std::uint32_t profile_revision{0U};
  ecu::core::v2::domain::DutProfileId profile_id{0U};
};

[[nodiscard]] constexpr bool is_valid_program_descriptor(
    const DutProfileProgramDescriptor descriptor) noexcept {
  return descriptor.schema_version == DutProfileDefinition::kSchemaVersion &&
         descriptor.profile_revision != 0U &&
         descriptor.profile_id != 0U;
}

class IDutProfileProgram {
 public:
  [[nodiscard]] virtual DutProfileProgramDescriptor descriptor()
      const noexcept = 0;

  [[nodiscard]] virtual ecu::bench::BenchComponentExecutionContract
  execution_contract() const noexcept = 0;

  // The program owns only DUT/profile semantics. Runtime/protocol dependencies
  // are injected by its concrete implementation. Physical drivers must not be
  // exposed through this contract.
  [[nodiscard]] virtual ecu::bench::BenchComponentStatus prepare(
      const ResolvedDutSessionPlan& plan) noexcept = 0;
  [[nodiscard]] virtual ecu::bench::BenchComponentStatus activate(
      const ResolvedDutSessionPlan& plan) noexcept = 0;
  [[nodiscard]] virtual ecu::bench::BenchComponentStatus service(
      const ResolvedDutSessionPlan& plan) noexcept = 0;
  [[nodiscard]] virtual ecu::bench::BenchComponentStatus safe_stop(
      const ResolvedDutSessionPlan& plan) noexcept = 0;
  [[nodiscard]] virtual ecu::bench::BenchComponentStatus stop(
      const ResolvedDutSessionPlan& plan) noexcept = 0;

 protected:
  ~IDutProfileProgram() = default;
};

enum class DutProfileEndpointState : std::uint8_t {
  idle,
  prepared,
  active,
  safe_stopped,
  faulted,
};

struct DutProfileEndpointCounters {
  std::uint32_t prepare_calls{0U};
  std::uint32_t activate_calls{0U};
  std::uint32_t service_calls{0U};
  std::uint32_t safe_stop_calls{0U};
  std::uint32_t stop_calls{0U};
  std::uint32_t faults{0U};
};

struct DutProfileEndpointSnapshot {
  bool valid{false};
  DutProfileEndpointState state{DutProfileEndpointState::idle};
  ecu::bench::BenchComponentStatus last_status{
      ecu::bench::BenchComponentStatus::no_action};
  ecu::core::v2::domain::DutProfileId profile_id{0U};
  std::uint32_t profile_revision{0U};
  DutProfileEndpointCounters counters{};
};

class DutProfileSessionEndpoint final
    : public ecu::bench::IDutSessionEndpoint {
 public:
  DutProfileSessionEndpoint(
      const ResolvedDutSessionPlan& plan,
      IDutProfileProgram& program) noexcept;

  [[nodiscard]] ecu::bench::BenchComponentExecutionContract
  execution_contract() const noexcept override;

  [[nodiscard]] ecu::bench::BenchComponentStatus prepare() noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus activate() noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus service() noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus safe_stop() noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus stop() noexcept override;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] DutProfileEndpointSnapshot snapshot() const noexcept;

 private:
  [[nodiscard]] bool descriptor_matches() const noexcept;
  [[nodiscard]] bool execution_contract_valid() const noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus reject() noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus accept_result(
      ecu::bench::BenchComponentStatus status,
      DutProfileEndpointState success_state) noexcept;
  void note_fault() noexcept;

  ResolvedDutSessionPlan plan_{};
  IDutProfileProgram& program_;
  ecu::bench::BenchComponentExecutionContract contract_{};
  bool valid_{false};
  DutProfileEndpointState state_{DutProfileEndpointState::idle};
  ecu::bench::BenchComponentStatus last_status_{
      ecu::bench::BenchComponentStatus::no_action};
  DutProfileEndpointCounters counters_{};
};

}  // namespace ecu::dut_profile
