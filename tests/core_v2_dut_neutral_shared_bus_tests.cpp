
#include "ecu/core_v2/actuation/cyclic_can_actuator_runtime.hpp"
#include "ecu/core_v2/protocol/isotp/isotp_endpoint.hpp"
#include "ecu/core_v2/protocol/j1939/network_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;

constexpr time::MonotonicClockDomainId kDomain{0xD071U};

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

class TestClock final : public time::IMonotonicClock {
 public:
  [[nodiscard]] time::MonotonicClockProperties properties()
      const noexcept override {
    return {
        kDomain,
        time::MonotonicDuration{100},
        time::MonotonicDuration{1000},
        time::MonotonicDuration{100},
        true};
  }

  [[nodiscard]] time::MonotonicClockReading read()
      const noexcept override {
    return {
        time::MonotonicClockStatus::ok,
        kDomain,
        now,
        time::MonotonicDuration{0}};
  }

  time::MonotonicTime now{0};
};

class TestDriver final : public transport::ICanDriver,
                         public transport::ICanChannelArbiter {
 public:
  [[nodiscard]] transport::CanPhysicalChannelId
  physical_channel_id() const noexcept override {
    return {1U};
  }

  [[nodiscard]] transport::ICanChannelArbiter&
  channel_arbiter() noexcept override {
    return *this;
  }

  [[nodiscard]] transport::CanDriverExecutionContract
  execution_contract() const noexcept override {
    const time::MonotonicDuration bound{1000};
    return {bound, bound, bound, bound, bound, bound, bound};
  }

  [[nodiscard]] transport::CanCapabilities capabilities()
      const noexcept override {
    return {true, true, true, true, 64U};
  }

  [[nodiscard]] bool try_acquire(
      const transport::CanPhysicalChannelId channel,
      const void* owner) noexcept override {
    if (channel.value != 1U || owner == nullptr ||
        (owner_ != nullptr && owner_ != owner)) {
      return false;
    }
    owner_ = owner;
    return true;
  }

  void release(
      const transport::CanPhysicalChannelId channel,
      const void* owner) noexcept override {
    if (channel.value == 1U && owner_ == owner) {
      owner_ = nullptr;
    }
  }

  [[nodiscard]] transport::CanStatus open(
      const transport::CanChannelConfig& config) noexcept override {
    if (!transport::capabilities_support(capabilities(), config)) {
      return transport::CanStatus::unsupported;
    }
    open_ = true;
    return transport::CanStatus::ok;
  }

  void close() noexcept override {
    open_ = false;
    rx_read_ = 0U;
    rx_count_ = 0U;
  }

  [[nodiscard]] bool is_open() const noexcept override {
    return open_;
  }

  [[nodiscard]] transport::CanStatus try_send(
      const transport::CanFrame& frame) noexcept override {
    if (!open_) {
      return transport::CanStatus::not_open;
    }
    if (tx_count >= tx.size()) {
      return transport::CanStatus::would_block;
    }
    tx[tx_count++] = frame;
    return transport::CanStatus::ok;
  }

  [[nodiscard]] transport::CanReceiveResult
  try_receive() noexcept override {
    if (!open_) {
      return {transport::CanStatus::not_open, {}, 0U};
    }
    if (rx_count_ == 0U) {
      return {transport::CanStatus::would_block, {}, 0U};
    }

    const auto value = rx[rx_read_];
    rx_read_ = (rx_read_ + 1U) % rx.size();
    --rx_count_;
    return {transport::CanStatus::ok, value, 0U};
  }

  bool inject(
      const transport::CanFrame& frame,
      const time::MonotonicClockReading& timestamp) noexcept {
    if (rx_count_ >= rx.size()) {
      return false;
    }
    const auto index = (rx_read_ + rx_count_) % rx.size();
    rx[index] = {frame, timestamp};
    ++rx_count_;
    return true;
  }

  std::array<transport::CanFrame, 32U> tx{};
  std::size_t tx_count{0U};

 private:
  const void* owner_{nullptr};
  bool open_{false};
  std::array<transport::ReceivedCanFrame, 16U> rx{};
  std::size_t rx_read_{0U};
  std::size_t rx_count_{0U};
};

class RawActuatorProgram final
    : public actuation::ICyclicCanActuatorProgram {
 public:
  [[nodiscard]] actuation::CyclicCanActuatorProgramContract
  execution_contract() const noexcept override {
    return {
        1U,
        1U,
        time::MonotonicDuration{1000},
        time::MonotonicDuration{1000}};
  }

  [[nodiscard]] actuation::ActuatorProgramStatus
  render_active_cycle(
      const std::uint32_t sequence,
      actuation::CyclicCanFrameBatch& batch) noexcept override {
    batch = {};
    batch.count = 1U;
    auto& frame = batch.frames[0U];
    frame.identifier = 0x180U;
    frame.identifier_format =
        transport::CanIdentifierFormat::standard_11_bit;
    frame.format = transport::CanFrameFormat::classic;
    frame.type = transport::CanFrameType::data;
    frame.length = 2U;
    frame.payload[0U] = static_cast<std::byte>(
        static_cast<std::uint8_t>(sequence & 0xFFU));
    frame.payload[1U] = std::byte{0x40U};
    return actuation::ActuatorProgramStatus::ok;
  }

  [[nodiscard]] actuation::ActuatorProgramStatus
  render_safe_stop(
      const actuation::SafeStopReason,
      actuation::CyclicCanFrameBatch& batch) noexcept override {
    batch = {};
    batch.count = 1U;
    auto& frame = batch.frames[0U];
    frame.identifier = 0x180U;
    frame.identifier_format =
        transport::CanIdentifierFormat::standard_11_bit;
    frame.format = transport::CanFrameFormat::classic;
    frame.type = transport::CanFrameType::data;
    frame.length = 1U;
    frame.payload[0U] = std::byte{0x00U};
    return actuation::ActuatorProgramStatus::ok;
  }
};

protocol::j1939::NameFields local_name() {
  protocol::j1939::NameFields fields{};
  fields.identity_number = 100U;
  fields.manufacturer_code = 0x321U;
  fields.function = 130U;
  fields.vehicle_system = 7U;
  fields.industry_group = 2U;
  fields.arbitrary_address_capable = true;
  return fields;
}

protocol::j1939::AddressClaimConfig j1939_config() {
  protocol::j1939::AddressClaimConfig config{};
  config.name = local_name();
  config.preferred_address = 0x20U;
  config.timestamp_domain = kDomain;
  config.max_timestamp_uncertainty =
      time::MonotonicDuration{100};
  return config;
}

protocol::isotp::IsoTpConfig isotp_config() {
  protocol::isotp::IsoTpConfig config{};
  config.frame_format = transport::CanFrameFormat::classic;
  config.tx_data_length = 8U;
  config.flow_control_timeout =
      time::MonotonicDuration{100000000};
  config.consecutive_frame_timeout =
      time::MonotonicDuration{100000000};
  config.timestamp_domain = kDomain;
  config.max_timestamp_uncertainty =
      time::MonotonicDuration{100};
  return config;
}

transport::CanFilter isotp_filter() {
  transport::CanFilter filter{};
  filter.identifier = 0x708U;
  filter.mask = 0x7FFU;
  filter.match_standard = true;
  filter.match_extended = false;
  return filter;
}

transport::CanFrame isotp_single_response() {
  transport::CanFrame frame{};
  frame.identifier = 0x708U;
  frame.identifier_format =
      transport::CanIdentifierFormat::standard_11_bit;
  frame.format = transport::CanFrameFormat::classic;
  frame.type = transport::CanFrameType::data;
  frame.length = 4U;
  frame.payload[0U] = std::byte{0x03U};
  frame.payload[1U] = std::byte{0x62U};
  frame.payload[2U] = std::byte{0xF1U};
  frame.payload[3U] = std::byte{0x90U};
  return frame;
}

transport::CanFrame remote_address_claim() {
  std::array<std::byte, 8U> name_payload{};
  auto fields = local_name();
  fields.identity_number = 200U;
  (void)protocol::j1939::encode_name_payload(fields, name_payload);

  std::array<std::uint8_t, 8U> bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] =
        std::to_integer<std::uint8_t>(name_payload[index]);
  }

  transport::CanFrame frame{};
  (void)protocol::j1939::build_classic_data_frame(
      protocol::j1939::MessageAddress{
          6U,
          protocol::j1939::kAddressClaimedPgn,
          0x80U,
          protocol::j1939::kGlobalAddress},
      bytes.data(),
      static_cast<std::uint8_t>(bytes.size()),
      frame);
  return frame;
}

}  // namespace

int main() {
  int failures = 0;

  TestClock clock;
  TestDriver driver;
  transport::CanBusRuntime bus{driver};

  protocol::j1939::NetworkManager j1939;
  failures += require(
      j1939.configure(j1939_config()),
      "J1939 network manager configures");

  protocol::isotp::IsoTpEndpoint isotp{
      {0x700U,
       0x708U,
       transport::CanIdentifierFormat::standard_11_bit},
      isotp_config()};
  protocol::isotp::IsoTpCanFrameSinkAdapter isotp_sink{isotp};

  failures += require(
      bus.subscribe(
          protocol::j1939::NetworkManager::rx_filter(),
          j1939,
          {time::MonotonicDuration{100000}})
              .status ==
          transport::CanSubscriptionStatus::subscribed &&
      bus.subscribe(
          isotp_filter(),
          isotp_sink,
          {time::MonotonicDuration{100000}})
              .status ==
          transport::CanSubscriptionStatus::subscribed &&
      bus.freeze_configuration() &&
      bus.start({
          500000U,
          false,
          0U,
          transport::CanMode::normal,
          kDomain}) == transport::CanStatus::ok,
      "one shared bus hosts protocol consumers");

  failures += require(
      j1939.start(clock.read()) ==
          protocol::j1939::NetworkManagerStatus::ok,
      "J1939 starts on shared runtime");

  transport::CanFrame j1939_tx{};
  failures += require(
      j1939.try_take_tx(j1939_tx) &&
      bus.send(j1939_tx) == transport::CanStatus::ok,
      "J1939 TX uses authoritative bus runtime");

  const std::array<std::byte, 3U> request{
      std::byte{0x22U},
      std::byte{0xF1U},
      std::byte{0x90U}};
  failures += require(
      isotp.start_send(request.data(), request.size()) ==
          protocol::isotp::IsoTpStatus::in_progress &&
      isotp.service(bus, clock.read()) ==
          protocol::isotp::IsoTpStatus::ok,
      "ISO-TP TX uses same authoritative bus runtime");

  RawActuatorProgram program;
  actuation::CyclicCanActuatorRuntime actuator{
      bus,
      clock,
      program};
  failures += require(
      actuator.configure({
          time::MonotonicDuration{10000000},
          time::MonotonicDuration{2000000},
          time::MonotonicDuration{100000000},
          time::MonotonicDuration{0}}) &&
      actuator.start() ==
          actuation::CyclicCanActuatorStatus::ok &&
      actuator.refresh_command() ==
          actuation::CyclicCanActuatorStatus::ok &&
      actuator.set_interlock(true) ==
          actuation::CyclicCanActuatorStatus::ok &&
      actuator.activate() ==
          actuation::CyclicCanActuatorStatus::ok,
      "raw cyclic actuator shares bus without protocol dependency");

  failures += require(
      driver.tx_count == 3U &&
      driver.tx[0U].identifier_format ==
          transport::CanIdentifierFormat::extended_29_bit &&
      driver.tx[1U].identifier == 0x700U &&
      driver.tx[2U].identifier == 0x180U,
      "J1939 ISO-TP and raw actuator TX remain distinct");

  failures += require(
      driver.inject(isotp_single_response(), clock.read()) &&
      driver.inject(remote_address_claim(), clock.read()),
      "mixed RX traffic injected");

  const auto poll = bus.poll(4U);
  failures += require(
      poll.status == transport::CanStatus::ok &&
      poll.frames_received == 2U &&
      poll.deliveries == 2U &&
      isotp.has_received() &&
      j1939.counters().frames_seen == 1U,
      "mixed RX is routed only to matching shared subscriptions");

  const auto response = isotp.take_received();
  failures += require(
      response.status == protocol::isotp::IsoTpStatus::ok &&
      response.length == 3U &&
      response.payload[0U] == std::byte{0x62U} &&
      j1939.status() ==
          protocol::j1939::NetworkManagerStatus::ok &&
      actuator.state() ==
          actuation::CyclicCanActuatorState::active,
      "protocol RX and actuator execution coexist independently");

  failures += require(
      actuator.stop() ==
          actuation::CyclicCanActuatorStatus::ok &&
      driver.tx_count == 4U &&
      driver.tx[3U].identifier == 0x180U &&
      driver.tx[3U].payload[0U] == std::byte{0x00U},
      "actuator safe-stop does not disturb protocol state");

  if (failures == 0) {
    std::cout << "CORE_V2_DUT_NEUTRAL_SHARED_BUS_PROOF=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
