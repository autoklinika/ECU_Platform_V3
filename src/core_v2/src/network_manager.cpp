#include "ecu/core_v2/protocol/j1939/network_manager.hpp"

#include <limits>

namespace ecu::core::v2::protocol::j1939 {

namespace {

void saturating_increment(std::uint32_t& value) noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  if (value != maximum) {
    ++value;
  }
}

bool valid_command_source(const std::uint8_t address) noexcept {
  return is_claimable_address(address);
}

}  // namespace

bool decode_commanded_address_message(
    const TpMessage& message,
    CommandedAddressMessage& value) noexcept {
  if (message.pgn != kCommandedAddressPgn ||
      message.size != 9U ||
      !message.broadcast ||
      message.destination_address != kGlobalAddress ||
      !valid_command_source(message.source_address)) {
    return false;
  }

  std::array<std::byte, 8U> name_payload{};
  for (std::size_t i = 0U; i < name_payload.size(); ++i) {
    name_payload[i] = message.data[i];
  }

  const auto raw_name = decode_name_payload(name_payload);
  if (!is_valid_name(decode_name(raw_name))) {
    return false;
  }

  const auto new_address =
      std::to_integer<std::uint8_t>(message.data[8U]);
  if (!is_claimable_address(new_address)) {
    return false;
  }

  value.target_name = raw_name;
  value.new_address = new_address;
  value.source_address = message.source_address;
  return true;
}

bool NetworkManager::configure(
    const AddressClaimConfig& config) noexcept {
  if (!address_claim_.configure(config)) {
    return false;
  }

  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  counters_ = {};
  status_ = NetworkManagerStatus::ok;
  configured_ = true;
  return true;
}

NetworkManagerStatus NetworkManager::start(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_ ||
      status_ == NetworkManagerStatus::protocol_fault ||
      status_ == NetworkManagerStatus::queue_overflow) {
    return NetworkManagerStatus::invalid_state;
  }

  handle_step(address_claim_.begin(now));
  return status_;
}

NetworkManagerStatus NetworkManager::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return NetworkManagerStatus::invalid_state;
  }
  if (status_ == NetworkManagerStatus::protocol_fault ||
      status_ == NetworkManagerStatus::queue_overflow) {
    return status_;
  }

  const auto before = tx_count_;
  const auto step = address_claim_.poll(now);
  handle_step(step);
  if (status_ != NetworkManagerStatus::ok) {
    return status_;
  }

  const bool state_changed =
      step.became_claimed ||
      step.address_changed ||
      step.address_lost;
  return tx_count_ != before || state_changed
             ? NetworkManagerStatus::ok
             : NetworkManagerStatus::no_action;
}

void NetworkManager::on_can_frame(
    const transport::ReceivedCanFrame& frame) noexcept {
  if (!configured_ ||
      status_ == NetworkManagerStatus::protocol_fault ||
      status_ == NetworkManagerStatus::queue_overflow) {
    return;
  }

  saturating_increment(counters_.frames_seen);
  handle_step(address_claim_.on_frame(frame));
}

NetworkManagerStatus NetworkManager::on_transport_message(
    const TpMessage& message,
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return NetworkManagerStatus::invalid_state;
  }
  if (status_ == NetworkManagerStatus::protocol_fault ||
      status_ == NetworkManagerStatus::queue_overflow) {
    return status_;
  }
  if (message.pgn != kCommandedAddressPgn) {
    return NetworkManagerStatus::no_action;
  }

  saturating_increment(counters_.commanded_address_messages);
  CommandedAddressMessage commanded{};
  if (!decode_commanded_address_message(message, commanded)) {
    saturating_increment(counters_.malformed_management_frames);
    return NetworkManagerStatus::no_action;
  }

  const auto step = address_claim_.command_address(
      commanded.target_name,
      commanded.new_address,
      now);
  handle_step(step);
  if (status_ != NetworkManagerStatus::ok) {
    return status_;
  }
  if (step.status == AddressClaimStatus::no_action) {
    return NetworkManagerStatus::no_action;
  }

  saturating_increment(counters_.commanded_address_applied);
  return NetworkManagerStatus::ok;
}

bool NetworkManager::try_take_tx(
    transport::CanFrame& frame) noexcept {
  if (tx_count_ == 0U) {
    return false;
  }

  frame = tx_queue_[tx_head_];
  tx_head_ = (tx_head_ + 1U) % tx_queue_.size();
  --tx_count_;
  return true;
}

AddressClaimState NetworkManager::address_claim_state()
    const noexcept {
  return address_claim_.state();
}

std::uint8_t NetworkManager::current_address() const noexcept {
  return address_claim_.current_address();
}

NetworkManagerStatus NetworkManager::status() const noexcept {
  return status_;
}

NetworkManagerCounters NetworkManager::counters() const noexcept {
  return counters_;
}

std::size_t NetworkManager::pending_tx_count() const noexcept {
  return tx_count_;
}

transport::CanFilter NetworkManager::rx_filter() noexcept {
  return transport::CanFilter{
      0U,
      0U,
      false,
      true};
}

void NetworkManager::handle_step(
    const AddressClaimStep& step) noexcept {
  if (step.conflict_observed) {
    saturating_increment(counters_.address_conflicts);
  }
  if (step.address_lost) {
    saturating_increment(counters_.address_losses);
  }

  switch (step.status) {
    case AddressClaimStatus::ok:
    case AddressClaimStatus::no_action:
      break;
    case AddressClaimStatus::invalid_argument:
      saturating_increment(
          counters_.malformed_management_frames);
      return;
    case AddressClaimStatus::invalid_state:
      status_ = NetworkManagerStatus::invalid_state;
      return;
    case AddressClaimStatus::invalid_time:
    case AddressClaimStatus::duplicate_name:
    case AddressClaimStatus::faulted:
      latch_protocol_fault();
      return;
  }

  if (step.tx_kind == AddressClaimTxKind::none) {
    status_ = NetworkManagerStatus::ok;
    return;
  }

  if (!enqueue(step.tx_frame)) {
    status_ = NetworkManagerStatus::queue_overflow;
    saturating_increment(counters_.tx_queue_overflows);
    return;
  }

  saturating_increment(counters_.tx_enqueued);
  status_ = NetworkManagerStatus::ok;
}

bool NetworkManager::enqueue(
    const transport::CanFrame& frame) noexcept {
  if (tx_count_ >= tx_queue_.size()) {
    return false;
  }

  tx_queue_[tx_tail_] = frame;
  tx_tail_ = (tx_tail_ + 1U) % tx_queue_.size();
  ++tx_count_;
  return true;
}

void NetworkManager::latch_protocol_fault() noexcept {
  status_ = NetworkManagerStatus::protocol_fault;
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
}

}  // namespace ecu::core::v2::protocol::j1939
