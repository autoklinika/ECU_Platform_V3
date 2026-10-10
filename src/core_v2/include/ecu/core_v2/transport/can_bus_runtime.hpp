#pragma once

#include "ecu/core_v2/runtime/configuration_gate.hpp"
#include "ecu/core_v2/transport/i_can_driver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::transport {

struct CanFilter {
  std::uint32_t identifier{0U};
  std::uint32_t mask{0U};
  bool match_standard{true};
  bool match_extended{true};

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool matches(const CanFrame& frame) const noexcept;
};

struct CanSinkExecutionContract {
  time::MonotonicDuration max_callback_duration{0};
};

class ICanFrameSink {
 public:
  // Non-owning interface. CanBusRuntime never deletes a sink through this
  // pointer; the concrete sink MUST outlive the runtime/subscription.
  // A protected non-virtual destructor prevents accidental polymorphic delete
  // and avoids a deleting-destructor dependency in deterministic Core.
  // The callback executes on the bus-owning executor. It never blocks, sleeps,
  // allocates dynamically, waits for external events or calls back into this
  // CanBusRuntime except stop(), which is deferred. It completes within the
  // bound declared at subscription time.
  virtual void on_can_frame(const ReceivedCanFrame& frame) noexcept = 0;

 protected:
  ~ICanFrameSink() = default;
};

struct CanSubscriptionHandle {
  std::uint16_t slot{0U};
  std::uint16_t generation{0U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return generation != 0U;
  }
};

enum class CanSubscriptionStatus : std::uint8_t {
  subscribed,
  invalid_argument,
  configuration_frozen,
  capacity_exhausted,
};

struct CanSubscriptionResult {
  CanSubscriptionStatus status{CanSubscriptionStatus::invalid_argument};
  CanSubscriptionHandle handle{};
};

enum class CanBusState : std::uint8_t {
  configuring,
  ready,
  running,
  stopped,
  faulted,
};

struct CanPollResult {
  CanStatus status{CanStatus::ok};
  std::size_t frames_received{0U};
  std::size_t deliveries{0U};
  std::uint32_t rx_dropped{0U};
};

struct CanExecutionBudgetResult {
  CanStatus status{CanStatus::invalid_state};
  time::MonotonicDuration max_duration{0};
};

class CanBusRuntime {
 public:
  static constexpr std::size_t kMaxSubscriptions = 32U;

  // Driver and all registered sinks MUST outlive this runtime object. The
  // runtime is single-executor: all public operations and callbacks are
  // serialized by the host. It is not internally thread-safe.
  explicit CanBusRuntime(ICanDriver& driver) noexcept;
  ~CanBusRuntime() noexcept;

  CanBusRuntime(const CanBusRuntime&) = delete;
  CanBusRuntime& operator=(const CanBusRuntime&) = delete;
  CanBusRuntime(CanBusRuntime&&) = delete;
  CanBusRuntime& operator=(CanBusRuntime&&) = delete;

  [[nodiscard]] CanSubscriptionResult subscribe(
      const CanFilter& filter,
      ICanFrameSink& sink,
      CanSinkExecutionContract execution) noexcept;

  [[nodiscard]] bool freeze_configuration() noexcept;

  [[nodiscard]] CanStatus start(
      const CanChannelConfig& config) noexcept;
  void stop() noexcept;
  [[nodiscard]] CanStatus recover() noexcept;

  [[nodiscard]] CanStatus send(const CanFrame& frame) noexcept;
  [[nodiscard]] CanPollResult poll(std::size_t max_frames) noexcept;

  [[nodiscard]] CanBusState state() const noexcept;
  [[nodiscard]] std::size_t subscription_count() const noexcept;

  // Upper bound for declared external work (driver calls + subscriber
  // callbacks). max_frames bounds work by frame count; each frame may reach all
  // matching subscribers. The host scheduler must add target-specific measured
  // Core WCET/dispatch overhead before treating this as an end-to-end WCET.
  // One possible deferred close() is included in the poll bound.
  [[nodiscard]] time::MonotonicDuration
  max_single_frame_dispatch_duration() const noexcept;
  [[nodiscard]] CanExecutionBudgetResult max_poll_execution_duration(
      std::size_t max_frames) const noexcept;
  [[nodiscard]] CanExecutionBudgetResult
  max_send_execution_duration() const noexcept;

 private:
  struct Subscription {
    bool used{false};
    std::uint16_t generation{0U};
    CanFilter filter{};
    ICanFrameSink* sink{nullptr};
    CanSinkExecutionContract execution{};
  };

  [[nodiscard]] bool operation_busy() const noexcept;
  [[nodiscard]] bool add_callback_budget(
      time::MonotonicDuration duration) noexcept;
  [[nodiscard]] std::size_t dispatch(
      const ReceivedCanFrame& frame) noexcept;
  [[nodiscard]] CanStatus validate_frame(const CanFrame& frame) const noexcept;
  [[nodiscard]] CanStatus validate_received_frame(
      const ReceivedCanFrame& frame) noexcept;

  void latch_fault(CanStatus status) noexcept;
  void latch_external_reentrancy_fault() noexcept;
  void release_lease_only() noexcept;
  void close_session_keep_lease() noexcept;
  void close_and_release() noexcept;
  void transition_to_stopped() noexcept;

  ICanDriver& driver_;
  ICanChannelArbiter* arbiter_{nullptr};
  CanPhysicalChannelId channel_id_{};
  CanChannelConfig active_config_{};
  CanCapabilities active_capabilities_{};
  CanDriverExecutionContract active_execution_{};
  bool owns_driver_lease_{false};
  bool owns_open_session_{false};
  bool dispatching_{false};
  bool external_call_active_{false};
  bool external_reentry_detected_{false};
  bool stop_pending_{false};
  runtime::ConfigurationGate configuration_{};
  std::array<Subscription, kMaxSubscriptions> subscriptions_{};
  CanBusState state_{CanBusState::configuring};
  std::uint16_t next_generation_{1U};
  time::MonotonicDuration callback_budget_{0};
  time::MonotonicTime last_rx_timestamp_{0};
  bool has_last_rx_timestamp_{false};
};

}  // namespace ecu::core::v2::transport
