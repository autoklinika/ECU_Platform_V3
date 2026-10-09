#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::actuation {

enum class SafeStopReason : std::uint8_t {
  explicit_stop,
  interlock_opened,
  command_timeout,
  feedback_timeout,
  cadence_missed,
  clock_fault,
  program_fault,
  transport_fault,
};

enum class ActuatorProgramStatus : std::uint8_t {
  ok,
  fault,
};

struct CyclicCanFrameBatch {
  static constexpr std::size_t kCapacity = 8U;

  std::array<transport::CanFrame, kCapacity> frames{};
  std::uint8_t count{0U};
};

struct CyclicCanActuatorProgramContract {
  std::uint8_t max_active_frames{0U};
  std::uint8_t max_safe_stop_frames{0U};
  time::MonotonicDuration max_active_render_duration{0};
  time::MonotonicDuration max_safe_stop_render_duration{0};
};

class ICyclicCanActuatorProgram {
 public:
  [[nodiscard]] virtual CyclicCanActuatorProgramContract
  execution_contract() const noexcept = 0;

  // The profile owns all DUT-specific wire semantics: CAN identifiers,
  // payload encoding, scaling, rolling counters, checksums/CRC/E2E and any
  // deterministic phase selection. Core supplies only the cycle sequence.
  [[nodiscard]] virtual ActuatorProgramStatus render_active_cycle(
      std::uint32_t cycle_sequence,
      CyclicCanFrameBatch& batch) noexcept = 0;

  // count == 0 is valid when the DUT's safe state is defined as stopping cyclic
  // command transmission. Otherwise the profile emits the bounded neutral/
  // disable frame sequence required by that DUT.
  [[nodiscard]] virtual ActuatorProgramStatus render_safe_stop(
      SafeStopReason reason,
      CyclicCanFrameBatch& batch) noexcept = 0;

 protected:
  ~ICyclicCanActuatorProgram() = default;
};

struct CyclicCanActuatorConfig {
  time::MonotonicDuration period{0};
  time::MonotonicDuration max_lateness{0};
  time::MonotonicDuration command_timeout{0};
  // Zero disables feedback freshness enforcement.
  time::MonotonicDuration feedback_timeout{0};
};

enum class CyclicCanActuatorState : std::uint8_t {
  unconfigured,
  stopped,
  interlocked,
  ready,
  active,
  faulted,
};

enum class CyclicCanActuatorStatus : std::uint8_t {
  ok,
  no_action,
  invalid_argument,
  invalid_state,
  interlocked,
  stale_command,
  command_timeout,
  feedback_timeout,
  cadence_missed,
  clock_fault,
  insufficient_clock_precision,
  program_fault,
  transport_fault,
  safe_stop_failed,
};

struct CyclicCanActuatorCounters {
  std::uint32_t active_cycles_sent{0U};
  std::uint32_t active_frames_sent{0U};
  std::uint32_t command_refreshes{0U};
  std::uint32_t feedback_refreshes{0U};
  std::uint32_t safe_stop_events{0U};
  std::uint32_t safe_stop_frames_sent{0U};
  std::uint32_t interlock_trips{0U};
  std::uint32_t command_timeouts{0U};
  std::uint32_t feedback_timeouts{0U};
  std::uint32_t cadence_misses{0U};
  std::uint32_t clock_faults{0U};
  std::uint32_t program_faults{0U};
  std::uint32_t transport_faults{0U};
  std::uint32_t safe_stop_failures{0U};
};

struct CyclicCanActuatorExecutionBudget {
  bool valid{false};
  time::MonotonicDuration max_service_duration{0};
  time::MonotonicDuration max_activate_duration{0};
  time::MonotonicDuration max_safe_stop_duration{0};
};

class CyclicCanActuatorRuntime final {
 public:
  // bus, clock and program are non-owning and MUST outlive this runtime.
  // All operations are single-executor and serialized by the owner.
  CyclicCanActuatorRuntime(
      transport::CanBusRuntime& bus,
      const time::IMonotonicClock& clock,
      ICyclicCanActuatorProgram& program) noexcept;

  CyclicCanActuatorRuntime(
      const CyclicCanActuatorRuntime&) = delete;
  CyclicCanActuatorRuntime& operator=(
      const CyclicCanActuatorRuntime&) = delete;
  CyclicCanActuatorRuntime(
      CyclicCanActuatorRuntime&&) = delete;
  CyclicCanActuatorRuntime& operator=(
      CyclicCanActuatorRuntime&&) = delete;

  [[nodiscard]] bool configure(
      const CyclicCanActuatorConfig& config) noexcept;

  // start() enters an explicitly interlocked state. Control cannot become
  // active until the owner opens the interlock and supplies a fresh command.
  [[nodiscard]] CyclicCanActuatorStatus start() noexcept;

  [[nodiscard]] CyclicCanActuatorStatus set_interlock(
      bool allow_active_control) noexcept;

  // A command refresh is a high-level command lease/heartbeat. The profile may
  // update its own bounded target state before this call. A late refresh never
  // revives an already-expired active lease.
  [[nodiscard]] CyclicCanActuatorStatus refresh_command() noexcept;

  // Activates cyclic output and transmits the first cycle immediately.
  [[nodiscard]] CyclicCanActuatorStatus activate() noexcept;

  // Called after a profile has accepted valid DUT feedback. A late feedback
  // refresh never revives an already-expired feedback lease.
  [[nodiscard]] CyclicCanActuatorStatus note_valid_feedback() noexcept;

  // service() never sleeps and emits at most one active cycle. The owner must
  // invoke it often enough to meet next_service_deadline(). Missing the
  // configured lateness bound fails closed through the profile's safe-stop.
  [[nodiscard]] CyclicCanActuatorStatus service() noexcept;

  [[nodiscard]] CyclicCanActuatorStatus stop() noexcept;

  // Fault recovery requires the external interlock to be closed. Recovery
  // never resumes control automatically; start(), command refresh, interlock
  // opening and activate() are required again.
  [[nodiscard]] CyclicCanActuatorStatus reset_fault() noexcept;

  [[nodiscard]] CyclicCanActuatorState state() const noexcept;
  [[nodiscard]] CyclicCanActuatorStatus status() const noexcept;
  [[nodiscard]] CyclicCanActuatorCounters counters() const noexcept;
  [[nodiscard]] time::MonotonicTime
  next_service_deadline() const noexcept;
  [[nodiscard]] CyclicCanActuatorExecutionBudget
  execution_budget() const noexcept;

 private:
  [[nodiscard]] bool valid_config(
      const CyclicCanActuatorConfig& config) const noexcept;
  [[nodiscard]] bool valid_program_contract(
      const CyclicCanActuatorProgramContract& contract) const noexcept;
  [[nodiscard]] bool read_healthy_clock(
      time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool precision_sufficient(
      time::MonotonicDuration duration) const noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;
  [[nodiscard]] time::MonotonicTime upper_bound(
      const time::MonotonicClockReading& reading) const noexcept;
  [[nodiscard]] bool set_deadline(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration duration,
      time::MonotonicTime& deadline) const noexcept;
  [[nodiscard]] bool deadline_reached(
      const time::MonotonicClockReading& reading,
      time::MonotonicTime deadline) const noexcept;
  [[nodiscard]] bool add_duration(
      time::MonotonicDuration left,
      time::MonotonicDuration right,
      time::MonotonicDuration& result) const noexcept;
  [[nodiscard]] bool multiply_duration(
      time::MonotonicDuration duration,
      std::uint8_t count,
      time::MonotonicDuration& result) const noexcept;
  [[nodiscard]] CyclicCanActuatorExecutionBudget
  calculate_execution_budget() const noexcept;

  [[nodiscard]] bool validate_batch(
      const CyclicCanFrameBatch& batch,
      std::uint8_t declared_max,
      bool active_batch) const noexcept;
  [[nodiscard]] transport::CanStatus send_batch(
      const CyclicCanFrameBatch& batch,
      bool safe_stop_batch) noexcept;
  [[nodiscard]] CyclicCanActuatorStatus send_active_cycle(
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] CyclicCanActuatorStatus perform_safe_stop(
      SafeStopReason reason,
      CyclicCanActuatorState success_state) noexcept;
  [[nodiscard]] CyclicCanActuatorStatus trip_fault(
      SafeStopReason reason,
      CyclicCanActuatorStatus fault_status) noexcept;
  void clear_leases() noexcept;

  transport::CanBusRuntime& bus_;
  const time::IMonotonicClock& clock_;
  ICyclicCanActuatorProgram& program_;
  time::MonotonicClockProperties clock_properties_{};
  CyclicCanActuatorProgramContract program_contract_{};
  CyclicCanActuatorConfig config_{};
  CyclicCanActuatorExecutionBudget budget_{};
  CyclicCanActuatorCounters counters_{};
  CyclicCanActuatorState state_{
      CyclicCanActuatorState::unconfigured};
  CyclicCanActuatorStatus status_{
      CyclicCanActuatorStatus::invalid_state};
  time::MonotonicTime next_cycle_deadline_{0};
  time::MonotonicTime command_deadline_{0};
  time::MonotonicTime feedback_deadline_{0};
  time::MonotonicTime last_observed_time_{0};
  std::uint32_t cycle_sequence_{0U};
  bool configured_{false};
  bool interlock_allows_control_{false};
  bool command_fresh_{false};
  bool feedback_fresh_{false};
  bool has_last_observed_time_{false};
};

}  // namespace ecu::core::v2::actuation
