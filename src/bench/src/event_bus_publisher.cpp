#include "ecu/bench/event_bus_publisher.hpp"

namespace ecu::bench {

namespace {

void put_u16(
    BenchSessionEventWireBufferV1& buffer,
    const std::size_t offset,
    const std::uint16_t value) noexcept {
  buffer[offset] = static_cast<std::byte>(value & 0xFFU);
  buffer[offset + 1U] =
      static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void put_u32(
    BenchSessionEventWireBufferV1& buffer,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
  for (std::size_t index = 0U; index < 4U; ++index) {
    buffer[offset + index] =
        static_cast<std::byte>(
            (value >> (8U * index)) & 0xFFU);
  }
}

void put_u64(
    BenchSessionEventWireBufferV1& buffer,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
  for (std::size_t index = 0U; index < 8U; ++index) {
    buffer[offset + index] =
        static_cast<std::byte>(
            (value >> (8U * index)) & 0xFFU);
  }
}

[[nodiscard]] std::uint8_t get_u8(
    const std::byte* payload,
    const std::size_t offset) noexcept {
  return std::to_integer<std::uint8_t>(payload[offset]);
}

[[nodiscard]] std::uint16_t get_u16(
    const std::byte* payload,
    const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(get_u8(payload, offset)) |
      static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(
              get_u8(payload, offset + 1U))
          << 8U));
}

[[nodiscard]] std::uint32_t get_u32(
    const std::byte* payload,
    const std::size_t offset) noexcept {
  std::uint32_t value = 0U;
  for (std::size_t index = 0U; index < 4U; ++index) {
    value |=
        static_cast<std::uint32_t>(
            get_u8(payload, offset + index))
        << (8U * index);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(
    const std::byte* payload,
    const std::size_t offset) noexcept {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value |=
        static_cast<std::uint64_t>(
            get_u8(payload, offset + index))
        << (8U * index);
  }
  return value;
}

[[nodiscard]] bool valid_event_kind(
    const std::uint8_t value) noexcept {
  return value <=
      static_cast<std::uint8_t>(BenchSessionEventKind::fault);
}

[[nodiscard]] bool valid_session_state(
    const std::uint8_t value) noexcept {
  return value <=
      static_cast<std::uint8_t>(BenchSessionState::faulted);
}

[[nodiscard]] bool valid_session_status(
    const std::uint8_t value) noexcept {
  return value <=
      static_cast<std::uint8_t>(
          BenchSessionStatus::cancellation_unavailable);
}

[[nodiscard]] bool valid_session_reason(
    const std::uint8_t value) noexcept {
  return value <=
      static_cast<std::uint8_t>(
          BenchSessionReason::cancellation_unavailable);
}

[[nodiscard]] bool valid_fault_source(
    const std::uint8_t value) noexcept {
  return value <=
      static_cast<std::uint8_t>(BenchFaultSource::safe_shutdown);
}

}  // namespace

bool encode_bench_session_event_v1(
    const BenchSessionEvent& event,
    BenchSessionEventWireBufferV1& destination) noexcept {
  if (event.schema_version != BenchSessionEvent::kSchemaVersion ||
      event.snapshot.schema_version !=
          BenchSessionSnapshot::kSchemaVersion) {
    return false;
  }

  destination = {};
  put_u16(destination, 0U, event.schema_version);
  destination[2U] = static_cast<std::byte>(event.kind);
  destination[3U] =
      static_cast<std::byte>(event.previous_state);
  destination[4U] =
      static_cast<std::byte>(event.snapshot.state);
  destination[5U] =
      static_cast<std::byte>(event.snapshot.status);
  destination[6U] =
      static_cast<std::byte>(event.snapshot.reason);
  destination[7U] =
      static_cast<std::byte>(event.snapshot.fault_source);

  std::uint8_t flags = 0U;
  if (event.snapshot.configured) {
    flags |= 0x01U;
  }
  if (event.snapshot.cleanup_required) {
    flags |= 0x02U;
  }
  if (event.snapshot.cancellation_requested) {
    flags |= 0x04U;
  }
  if (event.snapshot.observability_degraded) {
    flags |= 0x08U;
  }
  destination[8U] = static_cast<std::byte>(flags);
  destination[9U] =
      static_cast<std::byte>(
          event.snapshot.active_resource_count);

  put_u32(
      destination,
      12U,
      event.snapshot.dut_profile_id);
  put_u64(
      destination,
      16U,
      event.snapshot.lifecycle_revision);
  put_u64(
      destination,
      24U,
      event.snapshot.operation_generation);
  put_u64(
      destination,
      32U,
      event.snapshot.last_completed_operation_generation);
  put_u32(
      destination,
      40U,
      event.snapshot.counters.event_publish_failures);
  put_u32(
      destination,
      44U,
      event.snapshot.counters.faults);

  return true;
}

bool decode_bench_session_event_v1(
    const std::byte* payload,
    const std::size_t payload_size,
    DecodedBenchSessionEventV1& event) noexcept {
  event = {};

  if (payload == nullptr ||
      payload_size != kBenchSessionEventWireSizeV1 ||
      get_u16(payload, 0U) != BenchSessionEvent::kSchemaVersion) {
    return false;
  }

  const auto kind = get_u8(payload, 2U);
  const auto previous_state = get_u8(payload, 3U);
  const auto state = get_u8(payload, 4U);
  const auto status = get_u8(payload, 5U);
  const auto reason = get_u8(payload, 6U);
  const auto fault_source = get_u8(payload, 7U);
  const auto flags = get_u8(payload, 8U);

  if (!valid_event_kind(kind) ||
      !valid_session_state(previous_state) ||
      !valid_session_state(state) ||
      !valid_session_status(status) ||
      !valid_session_reason(reason) ||
      !valid_fault_source(fault_source) ||
      get_u8(payload, 9U) > BenchSession::kMaxLeases ||
      (flags & 0xF0U) != 0U ||
      get_u8(payload, 10U) != 0U ||
      get_u8(payload, 11U) != 0U) {
    return false;
  }

  event.kind = static_cast<BenchSessionEventKind>(kind);
  event.previous_state =
      static_cast<BenchSessionState>(previous_state);
  event.state = static_cast<BenchSessionState>(state);
  event.status = static_cast<BenchSessionStatus>(status);
  event.reason = static_cast<BenchSessionReason>(reason);
  event.fault_source =
      static_cast<BenchFaultSource>(fault_source);
  event.configured = (flags & 0x01U) != 0U;
  event.cleanup_required = (flags & 0x02U) != 0U;
  event.cancellation_requested = (flags & 0x04U) != 0U;
  event.observability_degraded = (flags & 0x08U) != 0U;
  event.active_resource_count = get_u8(payload, 9U);
  event.dut_profile_id = get_u32(payload, 12U);
  event.lifecycle_revision = get_u64(payload, 16U);
  event.operation_generation = get_u64(payload, 24U);
  event.last_completed_operation_generation =
      get_u64(payload, 32U);
  event.event_publish_failures = get_u32(payload, 40U);
  event.faults = get_u32(payload, 44U);
  return true;
}

BenchSessionEventBusPublisher::BenchSessionEventBusPublisher(
    ecu::core::v2::runtime::EventBus& events) noexcept
    : events_(events) {}

bool BenchSessionEventBusPublisher::ready() const noexcept {
  return events_.configuration_state() ==
      ecu::core::v2::runtime::ConfigurationState::frozen;
}

BenchSessionEventPublisherExecutionContract
BenchSessionEventBusPublisher::execution_contract() const noexcept {
  const auto budget =
      events_.max_publish_duration(kBenchSessionEventType);
  return {
      budget.valid
          ? budget.max_duration
          : ecu::core::v2::time::MonotonicDuration{0}};
}

BenchSessionEventPublishStatus
BenchSessionEventBusPublisher::publish(
    const BenchSessionEvent& event) noexcept {
  BenchSessionEventWireBufferV1 payload{};
  if (!ready() ||
      !encode_bench_session_event_v1(event, payload)) {
    return BenchSessionEventPublishStatus::unavailable;
  }

  const auto correlation =
      event.snapshot.operation_generation != 0U
          ? event.snapshot.operation_generation
          : event.snapshot.last_completed_operation_generation;

  const auto result = events_.publish({
      kBenchSessionEventType,
      correlation,
      severity(event),
      payload.data(),
      payload.size()});

  switch (result.status) {
    case ecu::core::v2::runtime::EventPublishStatus::published:
      return result.deliveries == 0U
                 ? BenchSessionEventPublishStatus::no_subscribers
                 : BenchSessionEventPublishStatus::published;
    case ecu::core::v2::runtime::EventPublishStatus::busy:
      return BenchSessionEventPublishStatus::busy;
    case ecu::core::v2::runtime::EventPublishStatus::invalid_argument:
    case ecu::core::v2::runtime::EventPublishStatus::configuration_not_frozen:
    case ecu::core::v2::runtime::EventPublishStatus::clock_fault:
    case ecu::core::v2::runtime::EventPublishStatus::sequence_exhausted:
      return BenchSessionEventPublishStatus::unavailable;
  }

  return BenchSessionEventPublishStatus::unavailable;
}

ecu::core::v2::runtime::EventSeverity
BenchSessionEventBusPublisher::severity(
    const BenchSessionEvent& event) const noexcept {
  if (event.snapshot.status ==
      BenchSessionStatus::safe_shutdown_failed) {
    return ecu::core::v2::runtime::EventSeverity::critical;
  }
  if (event.kind == BenchSessionEventKind::fault) {
    return ecu::core::v2::runtime::EventSeverity::error;
  }
  if (event.snapshot.status ==
      BenchSessionStatus::resource_unavailable) {
    return ecu::core::v2::runtime::EventSeverity::warning;
  }
  return ecu::core::v2::runtime::EventSeverity::info;
}

}  // namespace ecu::bench
