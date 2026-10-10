#include "ecu/core_v2/protocol/isobus/network_management.hpp"

#include <limits>

namespace ecu::core::v2::protocol::isobus {

namespace {

constexpr std::size_t kInvalidSlot =
    ControlFunctionRegistry::kCapacity;

void saturating_increment(std::uint32_t& value) noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  if (value != maximum) {
    ++value;
  }
}

}  // namespace

void ControlFunctionRegistry::reset() noexcept {
  records_ = {};
  size_ = 0U;
  counters_ = {};
}

RegistryStatus ControlFunctionRegistry::observe(
    const transport::CanFrame& frame) noexcept {
  saturating_increment(counters_.frames_seen);

  j1939::IdentifierFields fields{};
  if (!j1939::decode_classic_frame_identifier(frame, fields)) {
    return RegistryStatus::no_action;
  }
  if (j1939::parameter_group_number(fields) !=
      j1939::kAddressClaimedPgn) {
    return RegistryStatus::no_action;
  }

  saturating_increment(counters_.claims_seen);

  j1939::AddressClaimedMessage claimed{};
  if (!j1939::decode_address_claimed(frame, claimed)) {
    saturating_increment(counters_.malformed_claims);
    return RegistryStatus::malformed_frame;
  }

  auto slot = find_name_slot(claimed.raw_name);
  bool discovered = false;
  bool changed = false;

  if (slot == kInvalidSlot) {
    slot = find_free_slot();
    if (slot == kInvalidSlot) {
      saturating_increment(counters_.capacity_exhaustions);
      return RegistryStatus::capacity_exhausted;
    }

    records_[slot].in_use = true;
    records_[slot].name = claimed.raw_name;
    ++size_;
    discovered = true;
    changed = true;
    saturating_increment(counters_.discovered);
  }

  auto& record = records_[slot];
  if (claimed.source_address == j1939::kNullAddress) {
    if (!discovered &&
        (record.state != ControlFunctionState::cannot_claim ||
         record.source_address != j1939::kNullAddress)) {
      changed = true;
      saturating_increment(counters_.address_changes);
    }

    record.source_address = j1939::kNullAddress;
    record.state = ControlFunctionState::cannot_claim;
    saturating_increment(counters_.cannot_claim_reports);
    return changed ? RegistryStatus::ok
                   : RegistryStatus::no_action;
  }

  if (!j1939::is_claimable_address(claimed.source_address)) {
    saturating_increment(counters_.malformed_claims);
    return RegistryStatus::malformed_frame;
  }

  if (!discovered &&
      (record.state != ControlFunctionState::claimed ||
       record.source_address != claimed.source_address)) {
    changed = true;
    saturating_increment(counters_.address_changes);
  }

  record.source_address = claimed.source_address;
  record.state = ControlFunctionState::claimed;

  if (address_is_ambiguous(
          claimed.source_address,
          claimed.raw_name)) {
    saturating_increment(counters_.address_conflicts);
    return RegistryStatus::address_conflict;
  }

  return changed ? RegistryStatus::ok
                 : RegistryStatus::no_action;
}

LookupStatus ControlFunctionRegistry::find_by_name(
    const std::uint64_t name,
    ControlFunctionRecord& record) const noexcept {
  const auto slot = find_name_slot(name);
  if (slot == kInvalidSlot) {
    return LookupStatus::not_found;
  }

  record = records_[slot];
  return LookupStatus::found;
}

LookupStatus ControlFunctionRegistry::find_by_address(
    const std::uint8_t source_address,
    ControlFunctionRecord& record) const noexcept {
  if (!j1939::is_claimable_address(source_address)) {
    return LookupStatus::not_found;
  }

  std::size_t matches = 0U;
  for (const auto& candidate : records_) {
    if (!candidate.in_use ||
        candidate.state != ControlFunctionState::claimed ||
        candidate.source_address != source_address) {
      continue;
    }

    if (matches == 0U) {
      record = candidate;
    }
    ++matches;
    if (matches > 1U) {
      return LookupStatus::ambiguous;
    }
  }

  return matches == 1U ? LookupStatus::found
                       : LookupStatus::not_found;
}

std::size_t ControlFunctionRegistry::size() const noexcept {
  return size_;
}

ControlFunctionRegistryCounters
ControlFunctionRegistry::counters() const noexcept {
  return counters_;
}

std::size_t ControlFunctionRegistry::find_name_slot(
    const std::uint64_t name) const noexcept {
  for (std::size_t index = 0U;
       index < records_.size();
       ++index) {
    if (records_[index].in_use &&
        records_[index].name == name) {
      return index;
    }
  }
  return kInvalidSlot;
}

std::size_t ControlFunctionRegistry::find_free_slot()
    const noexcept {
  for (std::size_t index = 0U;
       index < records_.size();
       ++index) {
    if (!records_[index].in_use) {
      return index;
    }
  }
  return kInvalidSlot;
}

bool ControlFunctionRegistry::address_is_ambiguous(
    const std::uint8_t source_address,
    const std::uint64_t except_name) const noexcept {
  for (const auto& record : records_) {
    if (record.in_use &&
        record.state == ControlFunctionState::claimed &&
        record.source_address == source_address &&
        record.name != except_name) {
      return true;
    }
  }
  return false;
}

bool NetworkManagement::configure(
    const j1939::AddressClaimConfig& config) noexcept {
  if (!local_.configure(config)) {
    return false;
  }

  registry_.reset();
  registry_capacity_exhausted_ = false;
  configured_ = true;
  status_ = NetworkManagementStatus::ok;
  return true;
}

NetworkManagementStatus NetworkManagement::start(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return NetworkManagementStatus::invalid_state;
  }

  status_ = translate(local_.start(now));
  return status();
}

NetworkManagementStatus NetworkManagement::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return NetworkManagementStatus::invalid_state;
  }

  status_ = translate(local_.service_time(now));
  if (status_ == NetworkManagementStatus::ok ||
      status_ == NetworkManagementStatus::no_action) {
    return status();
  }
  return status_;
}

void NetworkManagement::on_can_frame(
    const transport::ReceivedCanFrame& frame) noexcept {
  if (!configured_) {
    return;
  }

  const auto registry_status =
      registry_.observe(frame.frame);
  if (registry_status == RegistryStatus::capacity_exhausted) {
    registry_capacity_exhausted_ = true;
  }

  local_.on_can_frame(frame);
  update_local_status();
}

NetworkManagementStatus
NetworkManagement::on_transport_message(
    const j1939::TpMessage& message,
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return NetworkManagementStatus::invalid_state;
  }

  status_ =
      translate(local_.on_transport_message(message, now));
  if (status_ == NetworkManagementStatus::ok ||
      status_ == NetworkManagementStatus::no_action) {
    return status();
  }
  return status_;
}

bool NetworkManagement::try_take_tx(
    transport::CanFrame& frame) noexcept {
  return local_.try_take_tx(frame);
}

LookupStatus NetworkManagement::find_by_name(
    const std::uint64_t name,
    ControlFunctionRecord& record) const noexcept {
  return registry_.find_by_name(name, record);
}

LookupStatus NetworkManagement::find_by_address(
    const std::uint8_t source_address,
    ControlFunctionRecord& record) const noexcept {
  return registry_.find_by_address(source_address, record);
}

j1939::AddressClaimState
NetworkManagement::local_address_claim_state()
    const noexcept {
  return local_.address_claim_state();
}

std::uint8_t NetworkManagement::local_address() const noexcept {
  return local_.current_address();
}

NetworkManagementStatus NetworkManagement::status() const noexcept {
  const auto local_status = translate(local_.status());
  if (local_status == NetworkManagementStatus::queue_overflow ||
      local_status == NetworkManagementStatus::protocol_fault ||
      local_status == NetworkManagementStatus::invalid_state) {
    return local_status;
  }

  if (registry_capacity_exhausted_) {
    return NetworkManagementStatus::registry_capacity_exhausted;
  }

  return local_status == NetworkManagementStatus::no_action
             ? NetworkManagementStatus::ok
             : local_status;
}

NetworkManagementCounters
NetworkManagement::counters() const noexcept {
  NetworkManagementCounters result{};
  result.local = local_.counters();
  result.registry = registry_.counters();
  return result;
}

std::size_t NetworkManagement::registered_control_functions()
    const noexcept {
  return registry_.size();
}

std::size_t NetworkManagement::pending_tx_count() const noexcept {
  return local_.pending_tx_count();
}

transport::CanFilter NetworkManagement::rx_filter() noexcept {
  return j1939::NetworkManager::rx_filter();
}

NetworkManagementStatus NetworkManagement::translate(
    const j1939::NetworkManagerStatus status) noexcept {
  switch (status) {
    case j1939::NetworkManagerStatus::ok:
      return NetworkManagementStatus::ok;
    case j1939::NetworkManagerStatus::no_action:
      return NetworkManagementStatus::no_action;
    case j1939::NetworkManagerStatus::invalid_state:
      return NetworkManagementStatus::invalid_state;
    case j1939::NetworkManagerStatus::queue_overflow:
      return NetworkManagementStatus::queue_overflow;
    case j1939::NetworkManagerStatus::protocol_fault:
      return NetworkManagementStatus::protocol_fault;
  }

  return NetworkManagementStatus::protocol_fault;
}

void NetworkManagement::update_local_status() noexcept {
  const auto translated = translate(local_.status());
  if (translated == NetworkManagementStatus::queue_overflow ||
      translated == NetworkManagementStatus::protocol_fault ||
      translated == NetworkManagementStatus::invalid_state) {
    status_ = translated;
  } else {
    status_ = NetworkManagementStatus::ok;
  }
}

}  // namespace ecu::core::v2::protocol::isobus
