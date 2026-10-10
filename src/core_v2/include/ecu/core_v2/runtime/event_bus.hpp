#pragma once

#include "ecu/core_v2/runtime/configuration_gate.hpp"
#include "ecu/core_v2/runtime/event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

enum class EventSubscriptionStatus : std::uint8_t {
  subscribed,
  already_subscribed,
  invalid_argument,
  configuration_frozen,
  capacity_exhausted,
};

enum class EventPublishStatus : std::uint8_t {
  published,
  invalid_argument,
  configuration_not_frozen,
  busy,
  clock_fault,
  sequence_exhausted,
};

struct EventPublishResult {
  EventPublishStatus status{EventPublishStatus::invalid_argument};
  EventSequence sequence{0U};
  std::size_t deliveries{0U};
};

struct EventExecutionBudgetResult {
  bool valid{false};
  time::MonotonicDuration max_duration{0};
};

class EventBus final {
 public:
  static constexpr std::size_t kMaxSubscriptions = 64U;
  static constexpr EventTypeId kAllEventTypes = 0U;

  explicit EventBus(
      const time::IMonotonicClock& clock) noexcept;

  [[nodiscard]] EventSubscriptionStatus subscribe(
      EventTypeId type,
      IEventSink& sink,
      EventSinkExecutionContract execution) noexcept;

  [[nodiscard]] bool freeze_configuration() noexcept;

  [[nodiscard]] EventPublishResult publish(
      const EventInput& input) noexcept;

  [[nodiscard]] EventSequence last_sequence() const noexcept;
  [[nodiscard]] std::size_t subscription_count() const noexcept;
  [[nodiscard]] ConfigurationState configuration_state()
      const noexcept;

  [[nodiscard]] EventExecutionBudgetResult
  max_publish_duration(EventTypeId type) const noexcept;

 private:
  struct Subscription {
    EventTypeId type{kAllEventTypes};
    IEventSink* sink{nullptr};
    EventSinkExecutionContract execution{};
  };

  [[nodiscard]] bool matching(
      const Subscription& subscription,
      EventTypeId type) const noexcept;
  [[nodiscard]] bool read_healthy_clock(
      time::MonotonicClockReading& reading) noexcept;

  const time::IMonotonicClock& clock_;
  time::MonotonicClockProperties clock_properties_{};
  ConfigurationGate configuration_{};
  std::array<Subscription, kMaxSubscriptions> subscriptions_{};
  EventSequence sequence_{0U};
  time::MonotonicTime last_observed_time_{0};
  bool has_last_observed_time_{false};
  bool publishing_{false};
};

}  // namespace ecu::core::v2::runtime
