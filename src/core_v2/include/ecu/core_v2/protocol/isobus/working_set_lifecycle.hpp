#pragma once

#include "ecu/core_v2/protocol/isobus/network_management.hpp"
#include "ecu/core_v2/protocol/isobus/working_set.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::isobus {

enum class WorkingSetState : std::uint8_t {
  assembling,
  incomplete,
  complete,
  conflict,
  network_incomplete,
  stale_master_address,
};

enum class WorkingSetLifecycleStatus : std::uint8_t {
  ok,
  no_action,
  malformed_frame,
  orphan_member,
  capacity_exhausted,
  conflict,
};

enum class WorkingSetLookupStatus : std::uint8_t {
  found,
  not_found,
  ambiguous,
};

struct WorkingSetSummary {
  bool in_use{false};
  bool master_name_known{false};
  std::uint64_t master_name{0U};
  std::uint8_t master_source_address{j1939::kNullAddress};
  std::uint8_t declared_total_members{0U};
  std::uint8_t received_member_messages{0U};
  std::uint8_t resolved_control_functions{0U};
  std::uint32_t generation{0U};
  WorkingSetState state{WorkingSetState::assembling};
};

struct WorkingSetLifecycleCounters {
  std::uint32_t master_messages{0U};
  std::uint32_t member_messages{0U};
  std::uint32_t declarations_started{0U};
  std::uint32_t declaration_resets{0U};
  std::uint32_t duplicate_members{0U};
  std::uint32_t orphan_members{0U};
  std::uint32_t malformed_frames{0U};
  std::uint32_t structural_conflicts{0U};
  std::uint32_t capacity_exhaustions{0U};
  std::uint32_t completion_transitions{0U};
};

class WorkingSetLifecycle final {
 public:
  static constexpr std::size_t kWorkingSetCapacity = 16U;
  static constexpr std::size_t kMemberCapacity =
      static_cast<std::size_t>(kMaxWorkingSetMembers - 1U);

  void reset() noexcept;

  [[nodiscard]] WorkingSetLifecycleStatus observe(
      const transport::CanFrame& frame,
      const NetworkManagement& network) noexcept;

  void refresh(const NetworkManagement& network) noexcept;

  [[nodiscard]] WorkingSetLookupStatus find_by_master_name(
      std::uint64_t master_name,
      WorkingSetSummary& summary) const noexcept;

  [[nodiscard]] WorkingSetLookupStatus find_by_master_address(
      std::uint8_t source_address,
      WorkingSetSummary& summary) const noexcept;

  [[nodiscard]] bool member_name(
      std::uint64_t master_name,
      std::size_t member_index,
      std::uint64_t& name) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] WorkingSetLifecycleCounters counters() const noexcept;

 private:
  struct Slot {
    bool in_use{false};
    bool master_name_known{false};
    bool structural_conflict{false};
    bool completion_reported{false};
    std::uint64_t master_name{0U};
    std::uint8_t master_source_address{j1939::kNullAddress};
    std::uint8_t declared_total_members{0U};
    std::uint8_t member_count{0U};
    std::uint8_t resolved_control_functions{0U};
    std::uint32_t generation{0U};
    WorkingSetState state{WorkingSetState::assembling};
    std::array<std::uint64_t, kMemberCapacity> member_names{};
  };

  [[nodiscard]] std::size_t find_source_slot(
      std::uint8_t source_address) const noexcept;
  [[nodiscard]] std::size_t find_master_slot(
      std::uint64_t master_name) const noexcept;
  [[nodiscard]] std::size_t find_free_slot() const noexcept;
  [[nodiscard]] bool contains_member(
      const Slot& slot,
      std::uint64_t name) const noexcept;
  [[nodiscard]] bool resolve_master_from_source(
      std::uint8_t source_address,
      const NetworkManagement& network,
      std::uint64_t& master_name) const noexcept;
  void begin_declaration(
      Slot& slot,
      const WorkingSetMasterMessage& message,
      bool master_name_known,
      std::uint64_t master_name) noexcept;
  void evaluate_slot(
      Slot& slot,
      const NetworkManagement& network) noexcept;
  [[nodiscard]] WorkingSetSummary summary(
      const Slot& slot) const noexcept;

  std::array<Slot, kWorkingSetCapacity> slots_{};
  std::size_t size_{0U};
  std::uint32_t next_generation_{1U};
  WorkingSetLifecycleCounters counters_{};
};

}  // namespace ecu::core::v2::protocol::isobus
