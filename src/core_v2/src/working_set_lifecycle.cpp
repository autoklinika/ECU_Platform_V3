#include "ecu/core_v2/protocol/isobus/working_set_lifecycle.hpp"

#include <limits>

namespace ecu::core::v2::protocol::isobus {

namespace {

constexpr std::size_t kInvalidSlot =
    WorkingSetLifecycle::kWorkingSetCapacity;

void saturating_increment(std::uint32_t& value) noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  if (value != maximum) {
    ++value;
  }
}

}  // namespace

void WorkingSetLifecycle::reset() noexcept {
  slots_ = {};
  size_ = 0U;
  next_generation_ = 1U;
  counters_ = {};
}

WorkingSetLifecycleStatus WorkingSetLifecycle::observe(
    const transport::CanFrame& frame,
    const NetworkManagement& network) noexcept {
  j1939::IdentifierFields fields{};
  if (!j1939::decode_classic_frame_identifier(frame, fields)) {
    return WorkingSetLifecycleStatus::no_action;
  }

  const auto pgn = j1939::parameter_group_number(fields);
  if (pgn == kWorkingSetMasterPgn) {
    saturating_increment(counters_.master_messages);

    WorkingSetMasterMessage message{};
    if (!decode_working_set_master(frame, message)) {
      saturating_increment(counters_.malformed_frames);
      return WorkingSetLifecycleStatus::malformed_frame;
    }

    std::uint64_t master_name = 0U;
    const bool master_name_known =
        resolve_master_from_source(
            message.source_address,
            network,
            master_name);

    std::size_t slot_index = kInvalidSlot;
    if (master_name_known) {
      slot_index = find_master_slot(master_name);
    }
    if (slot_index == kInvalidSlot) {
      slot_index = find_source_slot(message.source_address);
    }

    bool reset_existing = false;
    if (slot_index == kInvalidSlot) {
      slot_index = find_free_slot();
      if (slot_index == kInvalidSlot) {
        saturating_increment(counters_.capacity_exhaustions);
        return WorkingSetLifecycleStatus::capacity_exhausted;
      }
      slots_[slot_index].in_use = true;
      ++size_;
      saturating_increment(counters_.declarations_started);
    } else {
      reset_existing = true;
    }

    if (reset_existing) {
      saturating_increment(counters_.declaration_resets);
    }

    begin_declaration(
        slots_[slot_index],
        message,
        master_name_known,
        master_name);
    evaluate_slot(slots_[slot_index], network);

    return slots_[slot_index].state == WorkingSetState::conflict
               ? WorkingSetLifecycleStatus::conflict
               : WorkingSetLifecycleStatus::ok;
  }

  if (pgn == kWorkingSetMemberPgn) {
    saturating_increment(counters_.member_messages);

    WorkingSetMemberMessage message{};
    if (!decode_working_set_member(frame, message)) {
      saturating_increment(counters_.malformed_frames);
      return WorkingSetLifecycleStatus::malformed_frame;
    }

    const auto slot_index =
        find_source_slot(message.source_address);
    if (slot_index == kInvalidSlot) {
      saturating_increment(counters_.orphan_members);
      return WorkingSetLifecycleStatus::orphan_member;
    }

    auto& slot = slots_[slot_index];
    if (contains_member(slot, message.member_name)) {
      saturating_increment(counters_.duplicate_members);
      evaluate_slot(slot, network);
      return slot.state == WorkingSetState::conflict
                 ? WorkingSetLifecycleStatus::conflict
                 : WorkingSetLifecycleStatus::no_action;
    }

    if (slot.master_name_known &&
        slot.master_name == message.member_name) {
      slot.structural_conflict = true;
      saturating_increment(counters_.structural_conflicts);
      evaluate_slot(slot, network);
      return WorkingSetLifecycleStatus::conflict;
    }

    const auto expected_members =
        static_cast<std::size_t>(
            slot.declared_total_members - 1U);
    if (static_cast<std::size_t>(slot.member_count) >=
        expected_members) {
      slot.structural_conflict = true;
      saturating_increment(counters_.structural_conflicts);
      evaluate_slot(slot, network);
      return WorkingSetLifecycleStatus::conflict;
    }

    slot.member_names[slot.member_count] = message.member_name;
    ++slot.member_count;
    evaluate_slot(slot, network);

    return slot.state == WorkingSetState::conflict
               ? WorkingSetLifecycleStatus::conflict
               : WorkingSetLifecycleStatus::ok;
  }

  return WorkingSetLifecycleStatus::no_action;
}

void WorkingSetLifecycle::refresh(
    const NetworkManagement& network) noexcept {
  for (auto& slot : slots_) {
    if (slot.in_use) {
      evaluate_slot(slot, network);
    }
  }
}

WorkingSetLookupStatus
WorkingSetLifecycle::find_by_master_name(
    const std::uint64_t master_name,
    WorkingSetSummary& value) const noexcept {
  std::size_t matches = 0U;
  for (const auto& slot : slots_) {
    if (!slot.in_use ||
        !slot.master_name_known ||
        slot.master_name != master_name) {
      continue;
    }

    if (matches == 0U) {
      value = summary(slot);
    }
    ++matches;
    if (matches > 1U) {
      return WorkingSetLookupStatus::ambiguous;
    }
  }

  return matches == 1U ? WorkingSetLookupStatus::found
                       : WorkingSetLookupStatus::not_found;
}

WorkingSetLookupStatus
WorkingSetLifecycle::find_by_master_address(
    const std::uint8_t source_address,
    WorkingSetSummary& value) const noexcept {
  if (!j1939::is_claimable_address(source_address)) {
    return WorkingSetLookupStatus::not_found;
  }

  std::size_t matches = 0U;
  for (const auto& slot : slots_) {
    if (!slot.in_use ||
        slot.master_source_address != source_address) {
      continue;
    }

    if (matches == 0U) {
      value = summary(slot);
    }
    ++matches;
    if (matches > 1U) {
      return WorkingSetLookupStatus::ambiguous;
    }
  }

  return matches == 1U ? WorkingSetLookupStatus::found
                       : WorkingSetLookupStatus::not_found;
}

bool WorkingSetLifecycle::member_name(
    const std::uint64_t master_name,
    const std::size_t member_index,
    std::uint64_t& name) const noexcept {
  const auto slot_index = find_master_slot(master_name);
  if (slot_index == kInvalidSlot ||
      member_index >=
          static_cast<std::size_t>(
              slots_[slot_index].member_count)) {
    return false;
  }

  name = slots_[slot_index].member_names[member_index];
  return true;
}

std::size_t WorkingSetLifecycle::size() const noexcept {
  return size_;
}

WorkingSetLifecycleCounters
WorkingSetLifecycle::counters() const noexcept {
  return counters_;
}

std::size_t WorkingSetLifecycle::find_source_slot(
    const std::uint8_t source_address) const noexcept {
  for (std::size_t index = 0U;
       index < slots_.size();
       ++index) {
    if (slots_[index].in_use &&
        slots_[index].master_source_address ==
            source_address) {
      return index;
    }
  }
  return kInvalidSlot;
}

std::size_t WorkingSetLifecycle::find_master_slot(
    const std::uint64_t master_name) const noexcept {
  for (std::size_t index = 0U;
       index < slots_.size();
       ++index) {
    if (slots_[index].in_use &&
        slots_[index].master_name_known &&
        slots_[index].master_name == master_name) {
      return index;
    }
  }
  return kInvalidSlot;
}

std::size_t WorkingSetLifecycle::find_free_slot()
    const noexcept {
  for (std::size_t index = 0U;
       index < slots_.size();
       ++index) {
    if (!slots_[index].in_use) {
      return index;
    }
  }
  return kInvalidSlot;
}

bool WorkingSetLifecycle::contains_member(
    const Slot& slot,
    const std::uint64_t name) const noexcept {
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(slot.member_count);
       ++index) {
    if (slot.member_names[index] == name) {
      return true;
    }
  }
  return false;
}

bool WorkingSetLifecycle::resolve_master_from_source(
    const std::uint8_t source_address,
    const NetworkManagement& network,
    std::uint64_t& master_name) const noexcept {
  ControlFunctionRecord record{};
  if (network.find_by_address(source_address, record) !=
          LookupStatus::found ||
      record.state != ControlFunctionState::claimed) {
    return false;
  }

  master_name = record.name;
  return true;
}

void WorkingSetLifecycle::begin_declaration(
    Slot& slot,
    const WorkingSetMasterMessage& message,
    const bool master_name_known,
    const std::uint64_t master_name) noexcept {
  slot.master_name_known = master_name_known;
  slot.master_name = master_name_known ? master_name : 0U;
  slot.master_source_address = message.source_address;
  slot.declared_total_members = message.member_count;
  slot.member_count = 0U;
  slot.resolved_control_functions = 0U;
  slot.structural_conflict = false;
  slot.completion_reported = false;
  slot.state = WorkingSetState::assembling;
  slot.member_names = {};
  slot.generation = next_generation_;

  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  next_generation_ =
      next_generation_ == maximum
          ? 1U
          : next_generation_ + 1U;
}

void WorkingSetLifecycle::evaluate_slot(
    Slot& slot,
    const NetworkManagement& network) noexcept {
  if (slot.structural_conflict) {
    slot.state = WorkingSetState::conflict;
    slot.resolved_control_functions = 0U;
    slot.completion_reported = false;
    return;
  }

  ControlFunctionRecord master{};
  if (!slot.master_name_known) {
    const auto lookup = network.find_by_address(
        slot.master_source_address,
        master);
    if (lookup == LookupStatus::ambiguous) {
      slot.state = WorkingSetState::conflict;
      slot.resolved_control_functions = 0U;
      slot.completion_reported = false;
      return;
    }
    if (lookup == LookupStatus::found &&
        master.state == ControlFunctionState::claimed) {
      slot.master_name_known = true;
      slot.master_name = master.name;
    }
  }

  bool master_resolved = false;
  if (slot.master_name_known) {
    const auto by_name =
        network.find_by_name(slot.master_name, master);
    if (by_name == LookupStatus::found &&
        master.state == ControlFunctionState::claimed) {
      ControlFunctionRecord by_address{};
      const auto address_lookup =
          network.find_by_address(
              master.source_address,
              by_address);
      if (address_lookup == LookupStatus::ambiguous ||
          (address_lookup == LookupStatus::found &&
           by_address.name != slot.master_name)) {
        slot.state = WorkingSetState::conflict;
        slot.resolved_control_functions = 0U;
        slot.completion_reported = false;
        return;
      }

      if (master.source_address !=
          slot.master_source_address) {
        slot.state = WorkingSetState::stale_master_address;
        slot.resolved_control_functions = 0U;
        slot.completion_reported = false;
        return;
      }

      master_resolved =
          address_lookup == LookupStatus::found;
    } else {
      ControlFunctionRecord current_at_source{};
      if (network.find_by_address(
              slot.master_source_address,
              current_at_source) == LookupStatus::found &&
          current_at_source.name != slot.master_name) {
        slot.state = WorkingSetState::stale_master_address;
        slot.resolved_control_functions = 0U;
        slot.completion_reported = false;
        return;
      }
    }
  }

  std::uint8_t resolved =
      master_resolved ? static_cast<std::uint8_t>(1U)
                      : static_cast<std::uint8_t>(0U);

  bool member_conflict = false;
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(slot.member_count);
       ++index) {
    const auto member_name = slot.member_names[index];
    if (slot.master_name_known &&
        member_name == slot.master_name) {
      member_conflict = true;
      break;
    }

    ControlFunctionRecord member{};
    if (network.find_by_name(member_name, member) !=
            LookupStatus::found ||
        member.state != ControlFunctionState::claimed) {
      continue;
    }

    ControlFunctionRecord by_address{};
    const auto address_lookup =
        network.find_by_address(
            member.source_address,
            by_address);
    if (address_lookup == LookupStatus::ambiguous ||
        (address_lookup == LookupStatus::found &&
         by_address.name != member_name)) {
      member_conflict = true;
      break;
    }

    if (address_lookup == LookupStatus::found &&
        resolved <
            (std::numeric_limits<std::uint8_t>::max)()) {
      ++resolved;
    }
  }

  if (member_conflict) {
    slot.state = WorkingSetState::conflict;
    slot.resolved_control_functions = resolved;
    slot.completion_reported = false;
    return;
  }

  slot.resolved_control_functions = resolved;

  const auto expected_member_messages =
      static_cast<std::uint8_t>(
          slot.declared_total_members - 1U);
  if (slot.member_count < expected_member_messages) {
    slot.state = WorkingSetState::assembling;
    slot.completion_reported = false;
    return;
  }

  if (slot.member_count > expected_member_messages) {
    slot.state = WorkingSetState::conflict;
    slot.completion_reported = false;
    return;
  }

  if (resolved == slot.declared_total_members) {
    slot.state = WorkingSetState::complete;
    if (!slot.completion_reported) {
      saturating_increment(
          counters_.completion_transitions);
      slot.completion_reported = true;
    }
    return;
  }

  slot.completion_reported = false;
  slot.state =
      network.status() ==
              NetworkManagementStatus::
                  registry_capacity_exhausted
          ? WorkingSetState::network_incomplete
          : WorkingSetState::incomplete;
}

WorkingSetSummary WorkingSetLifecycle::summary(
    const Slot& slot) const noexcept {
  WorkingSetSummary result{};
  result.in_use = slot.in_use;
  result.master_name_known = slot.master_name_known;
  result.master_name = slot.master_name;
  result.master_source_address = slot.master_source_address;
  result.declared_total_members = slot.declared_total_members;
  result.received_member_messages = slot.member_count;
  result.resolved_control_functions =
      slot.resolved_control_functions;
  result.generation = slot.generation;
  result.state = slot.state;
  return result;
}

}  // namespace ecu::core::v2::protocol::isobus
