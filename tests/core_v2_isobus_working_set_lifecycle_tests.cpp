#include "ecu/core_v2/protocol/isobus/working_set_lifecycle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol;
using namespace ecu::core::v2::protocol::isobus;

constexpr time::MonotonicClockDomainId kDomain{0x117837U};
constexpr std::uint8_t kLocalAddress = 0x20U;

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

time::MonotonicClockReading reading(
    const std::int64_t ns) {
  return {
      time::MonotonicClockStatus::ok,
      kDomain,
      time::MonotonicTime{ns},
      time::MonotonicDuration{0}};
}

j1939::NameFields name(
    const std::uint32_t identity,
    const bool arbitrary = true) {
  j1939::NameFields fields{};
  fields.identity_number = identity;
  fields.manufacturer_code = 0x321U;
  fields.ecu_instance = 1U;
  fields.function_instance = 1U;
  fields.function = 130U;
  fields.vehicle_system = 7U;
  fields.vehicle_system_instance = 1U;
  fields.industry_group = 2U;
  fields.arbitrary_address_capable = arbitrary;
  return fields;
}

std::uint64_t raw_name(const j1939::NameFields& fields) {
  std::uint64_t raw = 0U;
  (void)j1939::encode_name(fields, raw);
  return raw;
}

j1939::AddressClaimConfig config(
    const j1939::NameFields& fields,
    const std::uint8_t preferred_address) {
  j1939::AddressClaimConfig value{};
  value.name = fields;
  value.preferred_address = preferred_address;
  value.timestamp_domain = kDomain;
  value.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  return value;
}

transport::ReceivedCanFrame claimed_frame(
    const j1939::NameFields& fields,
    const std::uint8_t source_address,
    const std::int64_t timestamp) {
  std::array<std::byte, 8U> payload{};
  transport::CanFrame frame{};
  if (!j1939::encode_name_payload(fields, payload)) {
    return {};
  }

  std::array<std::uint8_t, 8U> bytes{};
  for (std::size_t index = 0U;
       index < bytes.size();
       ++index) {
    bytes[index] =
        std::to_integer<std::uint8_t>(payload[index]);
  }

  if (!j1939::build_classic_data_frame(
          j1939::MessageAddress{
              6U,
              j1939::kAddressClaimedPgn,
              source_address,
              j1939::kGlobalAddress},
          bytes.data(),
          static_cast<std::uint8_t>(bytes.size()),
          frame)) {
    return {};
  }

  return {frame, reading(timestamp)};
}

bool setup_network(NetworkManagement& network) {
  if (!network.configure(config(name(1U), kLocalAddress)) ||
      network.start(reading(0)) !=
          NetworkManagementStatus::ok) {
    return false;
  }

  transport::CanFrame frame{};
  return network.try_take_tx(frame);
}

transport::CanFrame master_frame(
    const std::uint8_t master_address,
    const std::uint8_t total_members) {
  transport::CanFrame frame{};
  (void)build_working_set_master(
      master_address,
      total_members,
      frame);
  return frame;
}

transport::CanFrame member_frame(
    const std::uint8_t master_address,
    const j1939::NameFields& member) {
  transport::CanFrame frame{};
  (void)build_working_set_member(
      master_address,
      raw_name(member),
      frame);
  return frame;
}

}  // namespace

int main() {
  int failures = 0;

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(100U);
    const auto member1 = name(101U);
    const auto member2 = name(102U);
    failures += require(
        setup_network(network),
        "complete working-set network setup");

    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    network.on_can_frame(claimed_frame(member1, 0x91U, 2));
    network.on_can_frame(claimed_frame(member2, 0x92U, 3));

    failures += require(
        lifecycle.observe(
            master_frame(0x90U, 3U),
            network) ==
            WorkingSetLifecycleStatus::ok,
        "working-set declaration starts");

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::assembling &&
            summary.declared_total_members == 3U &&
            summary.received_member_messages == 0U &&
            summary.resolved_control_functions == 1U,
        "master is included in declared member count");

    failures += require(
        lifecycle.observe(
            member_frame(0x90U, member1),
            network) ==
                WorkingSetLifecycleStatus::ok &&
            lifecycle.find_by_master_name(
                raw_name(master),
                summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::assembling &&
            summary.received_member_messages == 1U &&
            summary.resolved_control_functions == 2U,
        "first non-master member keeps set assembling");

    failures += require(
        lifecycle.observe(
            member_frame(0x90U, member2),
            network) ==
                WorkingSetLifecycleStatus::ok &&
            lifecycle.find_by_master_name(
                raw_name(master),
                summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::complete &&
            summary.received_member_messages == 2U &&
            summary.resolved_control_functions == 3U &&
            lifecycle.counters().completion_transitions == 1U,
        "member-count minus one messages complete set");

    std::uint64_t observed_name = 0U;
    failures += require(
        lifecycle.member_name(
            raw_name(master),
            0U,
            observed_name) &&
            observed_name == raw_name(member1) &&
            lifecycle.member_name(
                raw_name(master),
                1U,
                observed_name) &&
            observed_name == raw_name(member2) &&
            !lifecycle.member_name(
                raw_name(master),
                2U,
                observed_name),
        "working-set member NAME list is bounded and ordered");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(110U);
    failures += require(
        setup_network(network),
        "single-CF working-set network setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));

    failures += require(
        lifecycle.observe(
            master_frame(0x90U, 1U),
            network) ==
                WorkingSetLifecycleStatus::ok,
        "single-CF working-set declaration accepted");

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::complete &&
            summary.declared_total_members == 1U &&
            summary.received_member_messages == 0U &&
            summary.resolved_control_functions == 1U,
        "single-CF working set needs no WSMEM");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(120U);
    const auto member = name(121U);
    failures += require(
        setup_network(network),
        "late-member network setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));

    failures += require(
        lifecycle.observe(
            master_frame(0x90U, 2U),
            network) ==
                WorkingSetLifecycleStatus::ok &&
            lifecycle.observe(
                member_frame(0x90U, member),
                network) ==
                WorkingSetLifecycleStatus::ok,
        "unresolved member declaration accepted");

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::incomplete &&
            summary.received_member_messages == 1U &&
            summary.resolved_control_functions == 1U,
        "complete declaration waits for member address claim");

    network.on_can_frame(claimed_frame(member, 0x91U, 2));
    lifecycle.refresh(network);
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::complete &&
            summary.resolved_control_functions == 2U,
        "late Address Claim resolves working set");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(130U);
    const auto member = name(131U);
    failures += require(
        setup_network(network),
        "Cannot Claim lifecycle setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    network.on_can_frame(claimed_frame(member, 0x91U, 2));

    failures += require(
        lifecycle.observe(
            master_frame(0x90U, 2U),
            network) ==
                WorkingSetLifecycleStatus::ok &&
            lifecycle.observe(
                member_frame(0x90U, member),
                network) ==
                WorkingSetLifecycleStatus::ok,
        "Cannot Claim complete setup");

    network.on_can_frame(
        claimed_frame(member, j1939::kNullAddress, 3));
    lifecycle.refresh(network);

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::incomplete &&
            summary.resolved_control_functions == 1U,
        "member Cannot Claim invalidates completeness without losing membership");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(140U);
    const auto member1 = name(141U);
    failures += require(
        setup_network(network),
        "duplicate-member setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    network.on_can_frame(claimed_frame(member1, 0x91U, 2));

    (void)lifecycle.observe(master_frame(0x90U, 3U), network);
    failures += require(
        lifecycle.observe(
            member_frame(0x90U, member1),
            network) ==
                WorkingSetLifecycleStatus::ok &&
            lifecycle.observe(
                member_frame(0x90U, member1),
                network) ==
                WorkingSetLifecycleStatus::no_action,
        "repeated WSMEM is idempotent");

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.received_member_messages == 1U &&
            summary.state == WorkingSetState::assembling &&
            lifecycle.counters().duplicate_members == 1U,
        "duplicate WSMEM does not satisfy declared count");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(150U);
    const auto member1 = name(151U);
    const auto member2 = name(152U);
    failures += require(
        setup_network(network),
        "excess-member setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    network.on_can_frame(claimed_frame(member1, 0x91U, 2));
    network.on_can_frame(claimed_frame(member2, 0x92U, 3));

    (void)lifecycle.observe(master_frame(0x90U, 2U), network);
    failures += require(
        lifecycle.observe(
            member_frame(0x90U, member1),
            network) ==
                WorkingSetLifecycleStatus::ok &&
            lifecycle.observe(
                member_frame(0x90U, member2),
                network) ==
                WorkingSetLifecycleStatus::conflict,
        "extra distinct WSMEM fails structural consistency");

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::conflict &&
            lifecycle.counters().structural_conflicts == 1U,
        "excess member conflict is latched until new declaration");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(160U);
    failures += require(
        setup_network(network),
        "master-as-member setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));

    (void)lifecycle.observe(master_frame(0x90U, 2U), network);
    failures += require(
        lifecycle.observe(
            member_frame(0x90U, master),
            network) ==
                WorkingSetLifecycleStatus::conflict,
        "master cannot consume a non-master member slot");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    failures += require(
        setup_network(network),
        "orphan-member setup");

    failures += require(
        lifecycle.observe(
            member_frame(0x90U, name(171U)),
            network) ==
                WorkingSetLifecycleStatus::orphan_member &&
            lifecycle.size() == 0U &&
            lifecycle.counters().orphan_members == 1U,
        "WSMEM without WSMSTR is not guessed into a working set");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(180U);
    const auto member = name(181U);
    failures += require(
        setup_network(network),
        "master-address migration setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    network.on_can_frame(claimed_frame(member, 0x91U, 2));

    (void)lifecycle.observe(master_frame(0x90U, 2U), network);
    (void)lifecycle.observe(member_frame(0x90U, member), network);

    WorkingSetSummary before{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            before) ==
                WorkingSetLookupStatus::found &&
            before.state == WorkingSetState::complete,
        "master-address migration starts complete");

    network.on_can_frame(claimed_frame(master, 0x93U, 3));
    lifecycle.refresh(network);

    WorkingSetSummary stale{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            stale) ==
                WorkingSetLookupStatus::found &&
            stale.state ==
                WorkingSetState::stale_master_address &&
            stale.master_source_address == 0x90U,
        "master address change invalidates old declaration");

    failures += require(
        lifecycle.observe(
            master_frame(0x93U, 2U),
            network) ==
                WorkingSetLifecycleStatus::ok &&
            lifecycle.observe(
                member_frame(0x93U, member),
                network) ==
                WorkingSetLifecycleStatus::ok,
        "new WSMSTR restarts declaration after master move");

    WorkingSetSummary current{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            current) ==
                WorkingSetLookupStatus::found &&
            current.state == WorkingSetState::complete &&
            current.master_source_address == 0x93U &&
            current.generation != before.generation &&
            lifecycle.size() == 1U &&
            lifecycle.counters().declaration_resets == 1U,
        "master move reuses bounded slot with new generation");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(190U);
    const auto member = name(191U);
    const auto intruder = name(192U);
    failures += require(
        setup_network(network),
        "ambiguous-member-address setup");
    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    network.on_can_frame(claimed_frame(member, 0x91U, 2));
    network.on_can_frame(claimed_frame(intruder, 0x91U, 3));

    (void)lifecycle.observe(master_frame(0x90U, 2U), network);
    failures += require(
        lifecycle.observe(
            member_frame(0x90U, member),
            network) ==
                WorkingSetLifecycleStatus::conflict,
        "ambiguous member address prevents complete set");

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state == WorkingSetState::conflict,
        "network address conflict propagates into working set");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    const auto master = name(200U);
    const auto missing_member = name(999U);
    failures += require(
        setup_network(network),
        "network-incomplete working-set setup");

    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    for (std::size_t index = 0U;
         index < ControlFunctionRegistry::kCapacity - 1U;
         ++index) {
      const auto identity =
          static_cast<std::uint32_t>(300U + index);
      const auto address =
          static_cast<std::uint8_t>(0x40U + index);
      network.on_can_frame(
          claimed_frame(
              name(identity),
              address,
              static_cast<std::int64_t>(index + 2U)));
    }

    network.on_can_frame(
        claimed_frame(missing_member, 0xA0U, 1000));
    failures += require(
        network.status() ==
            NetworkManagementStatus::
                registry_capacity_exhausted,
        "network registry overflow is visible to lifecycle");

    (void)lifecycle.observe(master_frame(0x90U, 2U), network);
    (void)lifecycle.observe(
        member_frame(0x90U, missing_member),
        network);

    WorkingSetSummary summary{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            summary) ==
                WorkingSetLookupStatus::found &&
            summary.state ==
                WorkingSetState::network_incomplete &&
            summary.resolved_control_functions == 1U,
        "registry overflow distinguishes unknown topology from normal incomplete set");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    failures += require(
        setup_network(network),
        "working-set capacity setup");

    bool fill_ok = true;
    for (std::size_t index = 0U;
         index < WorkingSetLifecycle::kWorkingSetCapacity;
         ++index) {
      const auto master =
          name(static_cast<std::uint32_t>(500U + index));
      const auto address =
          static_cast<std::uint8_t>(0x80U + index);
      network.on_can_frame(
          claimed_frame(
              master,
              address,
              static_cast<std::int64_t>(index + 1U)));
      fill_ok =
          fill_ok &&
          lifecycle.observe(
              master_frame(address, 1U),
              network) ==
              WorkingSetLifecycleStatus::ok;
    }

    failures += require(
        fill_ok &&
            lifecycle.size() ==
                WorkingSetLifecycle::kWorkingSetCapacity,
        "working-set table fills fixed capacity");

    const auto extra_master = name(900U);
    network.on_can_frame(
        claimed_frame(extra_master, 0xA0U, 1000));
    failures += require(
        lifecycle.observe(
            master_frame(0xA0U, 1U),
            network) ==
                WorkingSetLifecycleStatus::
                    capacity_exhausted &&
            lifecycle.size() ==
                WorkingSetLifecycle::kWorkingSetCapacity &&
            lifecycle.counters().capacity_exhaustions == 1U,
        "working-set capacity exhaustion is explicit");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    failures += require(
        setup_network(network),
        "unresolved-master setup");

    const auto master = name(950U);
    failures += require(
        lifecycle.observe(
            master_frame(0x90U, 1U),
            network) ==
                WorkingSetLifecycleStatus::ok,
        "WSMSTR can be retained before Address Claim");

    WorkingSetSummary pending{};
    failures += require(
        lifecycle.find_by_master_address(
            0x90U,
            pending) ==
                WorkingSetLookupStatus::found &&
            !pending.master_name_known &&
            pending.state == WorkingSetState::incomplete,
        "unresolved master remains explicit");

    network.on_can_frame(claimed_frame(master, 0x90U, 1));
    lifecycle.refresh(network);

    WorkingSetSummary resolved{};
    failures += require(
        lifecycle.find_by_master_name(
            raw_name(master),
            resolved) ==
                WorkingSetLookupStatus::found &&
            resolved.master_name_known &&
            resolved.state == WorkingSetState::complete,
        "later master Address Claim binds pending declaration");
  }

  {
    NetworkManagement network;
    WorkingSetLifecycle lifecycle;
    failures += require(
        setup_network(network),
        "malformed lifecycle setup");

    auto malformed = master_frame(0x90U, 2U);
    malformed.payload[7U] = std::byte{0x00U};
    failures += require(
        lifecycle.observe(malformed, network) ==
                WorkingSetLifecycleStatus::malformed_frame &&
            lifecycle.size() == 0U &&
            lifecycle.counters().malformed_frames == 1U,
        "malformed WSMSTR never creates lifecycle state");
  }

  if (failures == 0) {
    std::cout
        << "CORE_V2_ISOBUS_WORKING_SET_LIFECYCLE=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
