#include "ecu/core_v2/protocol/j1939/address_claim.hpp"

#include "ecu/core_v2/protocol/j1939/request.hpp"

#include <limits>

namespace ecu::core::v2::protocol::j1939 {

bool decode_address_claimed(
    const transport::CanFrame& frame,
    AddressClaimedMessage& message) noexcept {
  if (frame.length != 8U) {
    return false;
  }

  IdentifierFields fields{};
  if (!decode_classic_frame_identifier(frame, fields) ||
      parameter_group_number(fields) != kAddressClaimedPgn) {
    return false;
  }

  std::uint8_t destination = 0U;
  if (!destination_address(fields, destination) ||
      destination != kGlobalAddress) {
    return false;
  }

  std::array<std::byte, 8U> payload{};
  for (std::size_t index = 0U;
       index < payload.size();
       ++index) {
    payload[index] = frame.payload[index];
  }

  const auto raw_name = decode_name_payload(payload);
  if (!is_valid_name(decode_name(raw_name))) {
    return false;
  }

  message.source_address = fields.source_address;
  message.raw_name = raw_name;
  return true;
}

bool AddressClaimEngine::configure(
    const AddressClaimConfig& config) noexcept {
  if (configured_ && state_ != AddressClaimState::idle) {
    return false;
  }

  std::uint64_t raw_name = 0U;
  if (!valid_config(config, raw_name)) {
    return false;
  }

  config_ = config;
  raw_name_ = raw_name;
  current_address_ = kNullAddress;
  next_alternative_ = 0U;
  state_ = AddressClaimState::idle;
  claim_deadline_ = time::MonotonicTime{0};
  cannot_claim_response_deadline_ = time::MonotonicTime{0};
  last_observed_time_ = time::MonotonicTime{0};
  configured_ = true;
  has_last_observed_time_ = false;
  pending_cannot_claim_response_ = false;
  return true;
}

AddressClaimStep AddressClaimEngine::begin(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return {AddressClaimStatus::invalid_state};
  }
  if (state_ != AddressClaimState::idle) {
    return {AddressClaimStatus::invalid_state};
  }
  if (!observe_time(now)) {
    return {AddressClaimStatus::invalid_time};
  }

  next_alternative_ = 0U;
  return start_claim(config_.preferred_address, now, false);
}

AddressClaimStep AddressClaimEngine::on_frame(
    const transport::ReceivedCanFrame& frame) noexcept {
  if (!configured_) {
    return {AddressClaimStatus::invalid_state};
  }
  if (state_ == AddressClaimState::faulted) {
    return {AddressClaimStatus::faulted};
  }

  IdentifierFields fields{};
  if (!decode_classic_frame_identifier(frame.frame, fields)) {
    return {AddressClaimStatus::no_action};
  }

  const auto pgn = parameter_group_number(fields);
  if (pgn == kAddressClaimedPgn) {
    AddressClaimedMessage claimed{};
    if (!decode_address_claimed(frame.frame, claimed)) {
      return {AddressClaimStatus::invalid_argument};
    }

    if (!observe_time(frame.timestamp)) {
      return {AddressClaimStatus::invalid_time};
    }

    if (claimed.source_address == kNullAddress ||
        state_ == AddressClaimState::idle ||
        state_ == AddressClaimState::cannot_claim ||
        claimed.source_address != current_address_) {
      return {AddressClaimStatus::no_action};
    }

    if (claimed.raw_name == raw_name_) {
      state_ = AddressClaimState::faulted;
      current_address_ = kNullAddress;
      pending_cannot_claim_response_ = false;
      AddressClaimStep result{};
      result.status = AddressClaimStatus::duplicate_name;
      result.address_lost = true;
      return result;
    }

    if (name_has_higher_priority(claimed.raw_name, raw_name_)) {
      auto result = lose_address(frame.timestamp);
      result.conflict_observed = true;
      return result;
    }

    const bool was_claimed =
        state_ == AddressClaimState::claimed;
    auto result =
        start_claim(current_address_, frame.timestamp, false);
    result.conflict_observed = true;
    if (was_claimed) {
      result.became_claimed = false;
    }
    return result;
  }

  if (pgn == kRequestPgn &&
      addressed_to_us_or_global(fields)) {
    RequestMessage request{};
    if (!decode_request(frame.frame, request)) {
      return {AddressClaimStatus::invalid_argument};
    }
    if (request.requested_pgn != kAddressClaimedPgn) {
      return {AddressClaimStatus::no_action};
    }
    if (!observe_time(frame.timestamp)) {
      return {AddressClaimStatus::invalid_time};
    }

    if (state_ == AddressClaimState::idle) {
      next_alternative_ = 0U;
      return start_claim(
          config_.preferred_address,
          frame.timestamp,
          false);
    }

    if (state_ == AddressClaimState::claiming ||
        state_ == AddressClaimState::claimed) {
      AddressClaimStep result{};
      result.status = AddressClaimStatus::ok;
      result.tx_kind = AddressClaimTxKind::address_claim;
      if (!build_claim_frame(
              current_address_, result.tx_frame)) {
        state_ = AddressClaimState::faulted;
        result.status = AddressClaimStatus::faulted;
        result.tx_kind = AddressClaimTxKind::none;
      }
      return result;
    }

    if (state_ == AddressClaimState::cannot_claim) {
      return schedule_cannot_claim_response(
          frame.timestamp);
    }
  }

  return {AddressClaimStatus::no_action};
}

AddressClaimStep AddressClaimEngine::poll(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return {AddressClaimStatus::invalid_state};
  }
  if (state_ == AddressClaimState::faulted) {
    return {AddressClaimStatus::faulted};
  }
  if (!observe_time(now)) {
    return {AddressClaimStatus::invalid_time};
  }

  const auto lower = lower_bound(now);

  if (state_ == AddressClaimState::claiming &&
      lower >= claim_deadline_) {
    state_ = AddressClaimState::claimed;
    AddressClaimStep result{};
    result.status = AddressClaimStatus::ok;
    result.became_claimed = true;
    return result;
  }

  if (pending_cannot_claim_response_ &&
      lower >= cannot_claim_response_deadline_) {
    pending_cannot_claim_response_ = false;
    AddressClaimStep result{};
    result.status = AddressClaimStatus::ok;
    result.tx_kind = AddressClaimTxKind::cannot_claim;
    if (!build_claim_frame(kNullAddress, result.tx_frame)) {
      state_ = AddressClaimState::faulted;
      result.status = AddressClaimStatus::faulted;
      result.tx_kind = AddressClaimTxKind::none;
    }
    return result;
  }

  return {AddressClaimStatus::no_action};
}

AddressClaimStep AddressClaimEngine::command_address(
    const std::uint64_t target_name,
    const std::uint8_t commanded_address,
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return {AddressClaimStatus::invalid_state};
  }
  if (state_ == AddressClaimState::faulted) {
    return {AddressClaimStatus::faulted};
  }
  if (target_name != raw_name_ ||
      !config_.accept_commanded_address) {
    return {AddressClaimStatus::no_action};
  }
  if (state_ == AddressClaimState::idle) {
    return {AddressClaimStatus::invalid_state};
  }
  if (!is_claimable_address(commanded_address)) {
    return {AddressClaimStatus::invalid_argument};
  }
  if (!observe_time(now)) {
    return {AddressClaimStatus::invalid_time};
  }

  const bool changed = current_address_ != commanded_address;
  next_alternative_ = 0U;
  return start_claim(commanded_address, now, changed);
}

AddressClaimState AddressClaimEngine::state() const noexcept {
  return state_;
}

std::uint8_t AddressClaimEngine::current_address() const noexcept {
  return current_address_;
}

std::uint64_t AddressClaimEngine::raw_name() const noexcept {
  return raw_name_;
}

bool AddressClaimEngine::configured() const noexcept {
  return configured_;
}

bool AddressClaimEngine::valid_config(
    const AddressClaimConfig& config,
    std::uint64_t& raw_name) const noexcept {
  if (!encode_name(config.name, raw_name) ||
      !is_claimable_address(config.preferred_address) ||
      !config.timestamp_domain.valid() ||
      config.max_timestamp_uncertainty.count() < 0 ||
      config.cannot_claim_response_delay.count() < 0 ||
      config.cannot_claim_response_delay >
          kMaxCannotClaimResponseDelay ||
      config.alternative_count >
          config.alternative_addresses.size()) {
    return false;
  }

  if (!config.name.arbitrary_address_capable &&
      config.alternative_count != 0U) {
    return false;
  }

  for (std::size_t i = 0U;
       i < config.alternative_count;
       ++i) {
    const auto address = config.alternative_addresses[i];
    if (!is_claimable_address(address) ||
        address == config.preferred_address) {
      return false;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (config.alternative_addresses[j] == address) {
        return false;
      }
    }
  }

  return true;
}

bool AddressClaimEngine::observe_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading, config_.timestamp_domain) ||
      reading.uncertainty >
          config_.max_timestamp_uncertainty ||
      (has_last_observed_time_ &&
       reading.value < last_observed_time_)) {
    state_ = AddressClaimState::faulted;
    current_address_ = kNullAddress;
    pending_cannot_claim_response_ = false;
    return false;
  }

  last_observed_time_ = reading.value;
  has_last_observed_time_ = true;
  return true;
}

time::MonotonicTime AddressClaimEngine::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

bool AddressClaimEngine::upper_bound(
    const time::MonotonicClockReading& reading,
    time::MonotonicTime& value) const noexcept {
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (reading.value.count() >
      maximum - reading.uncertainty.count()) {
    return false;
  }
  value = time::MonotonicTime{
      reading.value.count() + reading.uncertainty.count()};
  return true;
}

bool AddressClaimEngine::set_deadline_after(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration delay,
    time::MonotonicTime& deadline) noexcept {
  time::MonotonicTime start{0};
  if (!upper_bound(reading, start)) {
    return false;
  }

  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (delay.count() < 0 ||
      start.count() > maximum - delay.count()) {
    return false;
  }

  deadline =
      time::MonotonicTime{start.count() + delay.count()};
  return true;
}

bool AddressClaimEngine::requires_stabilization_delay(
    const std::uint8_t address) const noexcept {
  return address >= 128U && address <= 247U;
}

bool AddressClaimEngine::next_address(
    std::uint8_t& address) noexcept {
  if (!config_.name.arbitrary_address_capable) {
    return false;
  }

  while (next_alternative_ < config_.alternative_count) {
    const auto candidate =
        config_.alternative_addresses[next_alternative_++];
    if (candidate != current_address_) {
      address = candidate;
      return true;
    }
  }
  return false;
}

AddressClaimStep AddressClaimEngine::start_claim(
    const std::uint8_t address,
    const time::MonotonicClockReading& now,
    const bool address_changed) noexcept {
  AddressClaimStep result{};
  result.status = AddressClaimStatus::ok;
  result.tx_kind = AddressClaimTxKind::address_claim;
  result.address_changed = address_changed;

  current_address_ = address;
  pending_cannot_claim_response_ = false;

  if (!build_claim_frame(address, result.tx_frame)) {
    state_ = AddressClaimState::faulted;
    current_address_ = kNullAddress;
    result.status = AddressClaimStatus::faulted;
    result.tx_kind = AddressClaimTxKind::none;
    return result;
  }

  if (requires_stabilization_delay(address)) {
    if (!set_deadline_after(
            now,
            kClaimStabilizationDelay,
            claim_deadline_)) {
      state_ = AddressClaimState::faulted;
      current_address_ = kNullAddress;
      result.status = AddressClaimStatus::invalid_time;
      result.tx_kind = AddressClaimTxKind::none;
      return result;
    }
    state_ = AddressClaimState::claiming;
  } else {
    state_ = AddressClaimState::claimed;
    result.became_claimed = true;
  }

  return result;
}

AddressClaimStep AddressClaimEngine::lose_address(
    const time::MonotonicClockReading& now) noexcept {
  std::uint8_t replacement = 0U;
  if (next_address(replacement)) {
    auto result = start_claim(replacement, now, true);
    result.address_lost = true;
    return result;
  }

  current_address_ = kNullAddress;
  state_ = AddressClaimState::cannot_claim;
  pending_cannot_claim_response_ = false;

  AddressClaimStep result{};
  result.status = AddressClaimStatus::ok;
  result.tx_kind = AddressClaimTxKind::cannot_claim;
  result.address_changed = true;
  result.address_lost = true;
  if (!build_claim_frame(kNullAddress, result.tx_frame)) {
    state_ = AddressClaimState::faulted;
    result.status = AddressClaimStatus::faulted;
    result.tx_kind = AddressClaimTxKind::none;
  }
  return result;
}

bool AddressClaimEngine::build_claim_frame(
    const std::uint8_t source_address,
    transport::CanFrame& frame) const noexcept {
  std::array<std::byte, 8U> payload{};
  if (!encode_name_payload(config_.name, payload) ||
      !build_classic_data_frame(
          MessageAddress{
              6U,
              kAddressClaimedPgn,
              source_address,
              kGlobalAddress},
          nullptr,
          0U,
          frame)) {
    return false;
  }

  frame.length = 8U;
  for (std::size_t i = 0U; i < payload.size(); ++i) {
    frame.payload[i] = payload[i];
  }
  return true;
}

bool AddressClaimEngine::addressed_to_us_or_global(
    const IdentifierFields& fields) const noexcept {
  std::uint8_t destination = 0U;
  if (!destination_address(fields, destination)) {
    return false;
  }

  return destination == kGlobalAddress ||
         destination == current_address_;
}

AddressClaimStep
AddressClaimEngine::schedule_cannot_claim_response(
    const time::MonotonicClockReading& now) noexcept {
  if (config_.cannot_claim_response_delay.count() == 0) {
    AddressClaimStep result{};
    result.status = AddressClaimStatus::ok;
    result.tx_kind = AddressClaimTxKind::cannot_claim;
    if (!build_claim_frame(kNullAddress, result.tx_frame)) {
      state_ = AddressClaimState::faulted;
      result.status = AddressClaimStatus::faulted;
      result.tx_kind = AddressClaimTxKind::none;
    }
    return result;
  }

  if (!set_deadline_after(
          now,
          config_.cannot_claim_response_delay,
          cannot_claim_response_deadline_)) {
    state_ = AddressClaimState::faulted;
    current_address_ = kNullAddress;
    return {AddressClaimStatus::invalid_time};
  }

  pending_cannot_claim_response_ = true;
  return {AddressClaimStatus::ok};
}

}  // namespace ecu::core::v2::protocol::j1939
