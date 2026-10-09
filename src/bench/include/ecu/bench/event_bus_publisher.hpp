#pragma once

#include "ecu/bench/session.hpp"
#include "ecu/core_v2/runtime/event_bus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::bench {

inline constexpr ecu::core::v2::runtime::EventTypeId
    kBenchSessionEventType = 0x42530101U;

inline constexpr std::size_t kBenchSessionEventWireSizeV1 = 48U;

using BenchSessionEventWireBufferV1 =
    std::array<std::byte, kBenchSessionEventWireSizeV1>;

struct DecodedBenchSessionEventV1 {
  BenchSessionEventKind kind{BenchSessionEventKind::operation};
  BenchSessionState previous_state{BenchSessionState::unconfigured};
  BenchSessionState state{BenchSessionState::unconfigured};
  BenchSessionStatus status{BenchSessionStatus::invalid_state};
  BenchSessionReason reason{BenchSessionReason::none};
  BenchFaultSource fault_source{BenchFaultSource::none};
  ecu::core::v2::domain::DutProfileId dut_profile_id{0U};
  ecu::core::v2::runtime::StateRevision lifecycle_revision{0U};
  std::uint64_t operation_generation{0U};
  std::uint64_t last_completed_operation_generation{0U};
  std::uint32_t event_publish_failures{0U};
  std::uint32_t faults{0U};
  std::uint8_t active_resource_count{0U};
  bool configured{false};
  bool cleanup_required{false};
  bool cancellation_requested{false};
  bool observability_degraded{false};
};

[[nodiscard]] bool encode_bench_session_event_v1(
    const BenchSessionEvent& event,
    BenchSessionEventWireBufferV1& destination) noexcept;

[[nodiscard]] bool decode_bench_session_event_v1(
    const std::byte* payload,
    std::size_t payload_size,
    DecodedBenchSessionEventV1& event) noexcept;

class BenchSessionEventBusPublisher final
    : public IBenchSessionEventPublisher {
 public:
  explicit BenchSessionEventBusPublisher(
      ecu::core::v2::runtime::EventBus& events) noexcept;

  [[nodiscard]] bool ready() const noexcept override;

  [[nodiscard]] BenchSessionEventPublisherExecutionContract
  execution_contract() const noexcept override;

  [[nodiscard]] BenchSessionEventPublishStatus publish(
      const BenchSessionEvent& event) noexcept override;

 private:
  [[nodiscard]] ecu::core::v2::runtime::EventSeverity severity(
      const BenchSessionEvent& event) const noexcept;

  ecu::core::v2::runtime::EventBus& events_;
};

}  // namespace ecu::bench
