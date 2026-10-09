#include "ecu/core_v2/runtime/event_bus.hpp"

#include <limits>

namespace ecu::core::v2::runtime {

EventBus::EventBus(
    const time::IMonotonicClock& clock) noexcept
    : clock_(clock),
      clock_properties_(clock.properties()) {}

EventSubscriptionStatus EventBus::subscribe(
    const EventTypeId type,
    IEventSink& sink,
    const EventSinkExecutionContract execution) noexcept {
  if (!configuration_.accepts_registration()) {
    return EventSubscriptionStatus::configuration_frozen;
  }
  if (execution.max_callback_duration.count() <= 0) {
    return EventSubscriptionStatus::invalid_argument;
  }

  for (const auto& subscription : subscriptions_) {
    if (subscription.sink == &sink &&
        subscription.type == type) {
      return EventSubscriptionStatus::already_subscribed;
    }
  }

  for (auto& subscription : subscriptions_) {
    if (subscription.sink == nullptr) {
      subscription.type = type;
      subscription.sink = &sink;
      subscription.execution = execution;
      return EventSubscriptionStatus::subscribed;
    }
  }

  return EventSubscriptionStatus::capacity_exhausted;
}

bool EventBus::freeze_configuration() noexcept {
  if (subscription_count() == 0U ||
      !time::is_valid_clock_properties(clock_properties_)) {
    return false;
  }
  return configuration_.freeze();
}

EventPublishResult EventBus::publish(
    const EventInput& input) noexcept {
  EventPublishResult result{};
  if (!is_valid_event_input(input)) {
    return result;
  }
  if (configuration_.state() != ConfigurationState::frozen) {
    result.status =
        EventPublishStatus::configuration_not_frozen;
    return result;
  }
  if (publishing_) {
    result.status = EventPublishStatus::busy;
    return result;
  }
  if (sequence_ ==
      (std::numeric_limits<EventSequence>::max)()) {
    result.status = EventPublishStatus::sequence_exhausted;
    return result;
  }

  time::MonotonicClockReading reading{};
  if (!read_healthy_clock(reading)) {
    result.status = EventPublishStatus::clock_fault;
    return result;
  }

  ++sequence_;
  EventView event{};
  event.header.sequence = sequence_;
  event.header.type = input.type;
  event.header.correlation_id = input.correlation_id;
  event.header.severity = input.severity;
  event.header.timestamp = reading.value;
  event.payload = input.payload;
  event.payload_size = input.payload_size;

  publishing_ = true;
  for (const auto& subscription : subscriptions_) {
    if (subscription.sink != nullptr &&
        matching(subscription, input.type)) {
      subscription.sink->on_event(event);
      ++result.deliveries;
    }
  }
  publishing_ = false;

  result.status = EventPublishStatus::published;
  result.sequence = sequence_;
  return result;
}

EventSequence EventBus::last_sequence() const noexcept {
  return sequence_;
}

std::size_t EventBus::subscription_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& subscription : subscriptions_) {
    if (subscription.sink != nullptr) {
      ++count;
    }
  }
  return count;
}

ConfigurationState EventBus::configuration_state()
    const noexcept {
  return configuration_.state();
}

EventExecutionBudgetResult EventBus::max_publish_duration(
    const EventTypeId type) const noexcept {
  EventExecutionBudgetResult result{};
  if (type == 0U ||
      !time::is_valid_clock_properties(clock_properties_)) {
    return result;
  }

  auto total = clock_properties_.max_read_latency.count();
  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();
  if (total <= 0) {
    return result;
  }

  for (const auto& subscription : subscriptions_) {
    if (subscription.sink == nullptr ||
        !matching(subscription, type)) {
      continue;
    }

    const auto duration =
        subscription.execution.max_callback_duration.count();
    if (duration <= 0 || total > maximum - duration) {
      return result;
    }
    total += duration;
  }

  result.valid = true;
  result.max_duration = time::MonotonicDuration{total};
  return result;
}

bool EventBus::matching(
    const Subscription& subscription,
    const EventTypeId type) const noexcept {
  return subscription.type == kAllEventTypes ||
         subscription.type == type;
}

bool EventBus::read_healthy_clock(
    time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_properties(clock_properties_)) {
    return false;
  }

  reading = clock_.read();
  if (!time::is_valid_clock_reading(
          reading,
          clock_properties_.domain) ||
      reading.uncertainty >
          clock_properties_.max_uncertainty) {
    return false;
  }

  if (has_last_observed_time_ &&
      reading.value < last_observed_time_) {
    return false;
  }

  last_observed_time_ = reading.value;
  has_last_observed_time_ = true;
  return true;
}

}  // namespace ecu::core::v2::runtime
