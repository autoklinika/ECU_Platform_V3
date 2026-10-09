#pragma once

#include "ecu/core_v2/runtime/command.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

using EventSequence = std::uint64_t;
using EventTypeId = std::uint32_t;

enum class EventSeverity : std::uint8_t {
  debug,
  info,
  warning,
  error,
  critical,
};

struct EventInput {
  EventTypeId type{0U};
  CorrelationId correlation_id{0U};
  EventSeverity severity{EventSeverity::info};
  const std::byte* payload{nullptr};
  std::size_t payload_size{0U};
};

struct EventHeader {
  EventSequence sequence{0U};
  EventTypeId type{0U};
  CorrelationId correlation_id{0U};
  EventSeverity severity{EventSeverity::info};
  time::MonotonicTime timestamp{0};
};

struct EventView {
  EventHeader header{};
  const std::byte* payload{nullptr};
  std::size_t payload_size{0U};
};

[[nodiscard]] constexpr bool is_valid_event_input(
    const EventInput& event) noexcept {
  return event.type != 0U &&
         (event.payload_size == 0U || event.payload != nullptr);
}

class IEventSink {
 public:
  virtual void on_event(const EventView& event) noexcept = 0;

 protected:
  ~IEventSink() = default;
};

struct EventSinkExecutionContract {
  time::MonotonicDuration max_callback_duration{0};
};

}  // namespace ecu::core::v2::runtime
