#include "ecu/core_v2/protocol/isobus/network_management.hpp"
#include "ecu/core_v2/protocol/isobus/working_set.hpp"
#include "ecu/core_v2/protocol/j1939/request.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol;
using namespace ecu::core::v2::protocol::isobus;

constexpr time::MonotonicClockDomainId kDomain{0x117835U};

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

transport::ReceivedCanFrame request_address_claim(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::int64_t timestamp) {
  transport::CanFrame frame{};
  if (!j1939::build_request(
          source_address,
          destination_address,
          j1939::kAddressClaimedPgn,
          frame)) {
    return {};
  }
  return {frame, reading(timestamp)};
}

}  // namespace

int main() {
  int failures = 0;

  {
    const auto fields = name(1U);
    const auto received =
        claimed_frame(fields, 0x80U, 0);

    j1939::AddressClaimedMessage decoded{};
    failures += require(
        j1939::decode_address_claimed(
            received.frame,
            decoded) &&
            decoded.source_address == 0x80U &&
            decoded.raw_name == raw_name(fields),
        "strict Address Claimed codec accepts valid frame");

    auto malformed = received.frame;
    malformed.payload[6U] =
        static_cast<std::byte>(
            std::to_integer<std::uint8_t>(
                malformed.payload[6U]) |
            0x01U);
    failures += require(
        !j1939::decode_address_claimed(
            malformed,
            decoded),
        "Address Claimed codec rejects reserved NAME bit");

    malformed = received.frame;
    malformed.length = 7U;
    failures += require(
        !j1939::decode_address_claimed(
            malformed,
            decoded),
        "Address Claimed codec requires exact DLC");
  }

  {
    ControlFunctionRegistry registry;
    const auto fields = name(10U);
    const auto raw = raw_name(fields);

    failures += require(
        registry.observe(
            claimed_frame(fields, 0x80U, 0).frame) ==
            RegistryStatus::ok &&
            registry.size() == 1U,
        "registry discovers remote control function");

    ControlFunctionRecord record{};
    failures += require(
        registry.find_by_name(raw, record) ==
                LookupStatus::found &&
            record.state == ControlFunctionState::claimed &&
            record.source_address == 0x80U,
        "registry maps NAME to source address");

    failures += require(
        registry.find_by_address(0x80U, record) ==
                LookupStatus::found &&
            record.name == raw,
        "registry maps source address to NAME");

    failures += require(
        registry.observe(
            claimed_frame(fields, 0x80U, 1).frame) ==
            RegistryStatus::no_action &&
            registry.size() == 1U,
        "repeated identical claim is idempotent");

    failures += require(
        registry.observe(
            claimed_frame(fields, 0x81U, 2).frame) ==
            RegistryStatus::ok &&
            registry.find_by_address(0x80U, record) ==
                LookupStatus::not_found &&
            registry.find_by_address(0x81U, record) ==
                LookupStatus::found &&
            registry.counters().address_changes == 1U,
        "same NAME can move to a new source address");

    failures += require(
        registry.observe(
            claimed_frame(
                fields,
                j1939::kNullAddress,
                3).frame) ==
            RegistryStatus::ok &&
            registry.find_by_name(raw, record) ==
                LookupStatus::found &&
            record.state ==
                ControlFunctionState::cannot_claim &&
            record.source_address == j1939::kNullAddress &&
            registry.find_by_address(0x81U, record) ==
                LookupStatus::not_found &&
            registry.counters().cannot_claim_reports == 1U,
        "Cannot Claim is represented without stale address");
  }

  {
    ControlFunctionRegistry registry;
    const auto first = name(20U);
    const auto second = name(21U);

    failures += require(
        registry.observe(
            claimed_frame(first, 0x90U, 0).frame) ==
                RegistryStatus::ok &&
            registry.observe(
                claimed_frame(second, 0x90U, 1).frame) ==
                RegistryStatus::address_conflict,
        "duplicate source address is reported");

    ControlFunctionRecord record{};
    failures += require(
        registry.find_by_address(0x90U, record) ==
                LookupStatus::ambiguous &&
            registry.counters().address_conflicts == 1U,
        "conflicting address lookup fails ambiguous");

    failures += require(
        registry.observe(
            claimed_frame(second, 0x91U, 2).frame) ==
                RegistryStatus::ok &&
            registry.find_by_address(0x90U, record) ==
                LookupStatus::found &&
            record.name == raw_name(first) &&
            registry.find_by_address(0x91U, record) ==
                LookupStatus::found &&
            record.name == raw_name(second),
        "address conflict resolves after claimant moves");
  }

  {
    ControlFunctionRegistry registry;
    auto malformed =
        claimed_frame(name(30U), 0x80U, 0).frame;
    malformed.payload[6U] =
        static_cast<std::byte>(
            std::to_integer<std::uint8_t>(
                malformed.payload[6U]) |
            0x01U);

    failures += require(
        registry.observe(malformed) ==
                RegistryStatus::malformed_frame &&
            registry.size() == 0U &&
            registry.counters().malformed_claims == 1U,
        "malformed NAME never enters registry");

    transport::CanFrame unrelated{};
    failures += require(
        build_working_set_master(0x80U, 1U, unrelated) &&
            registry.observe(unrelated) ==
                RegistryStatus::no_action &&
            registry.size() == 0U,
        "unrelated ISOBUS traffic is ignored");
  }

  {
    ControlFunctionRegistry registry;
    bool fill_ok = true;
    for (std::size_t index = 0U;
         index < ControlFunctionRegistry::kCapacity;
         ++index) {
      const auto identity =
          static_cast<std::uint32_t>(100U + index);
      const auto address =
          static_cast<std::uint8_t>(0x40U + index);
      fill_ok =
          fill_ok &&
          registry.observe(
              claimed_frame(
                  name(identity),
                  address,
                  static_cast<std::int64_t>(index))
                  .frame) ==
              RegistryStatus::ok;
    }

    failures += require(
        fill_ok &&
            registry.size() ==
                ControlFunctionRegistry::kCapacity,
        "registry fills fixed capacity without heap growth");

    failures += require(
        registry.observe(
            claimed_frame(name(1000U), 0xA0U, 1000).frame) ==
                RegistryStatus::capacity_exhausted &&
            registry.size() ==
                ControlFunctionRegistry::kCapacity &&
            registry.counters().capacity_exhaustions == 1U,
        "registry capacity exhaustion is explicit");
  }

  {
    NetworkManagement manager;
    const auto local_name = name(200U);
    failures += require(
        manager.configure(config(local_name, 0x20U)) &&
            manager.start(reading(0)) ==
                NetworkManagementStatus::ok,
        "ISOBUS network management starts via J1939 engine");

    transport::CanFrame tx{};
    j1939::AddressClaimedMessage local_claim{};
    failures += require(
        manager.try_take_tx(tx) &&
            j1939::decode_address_claimed(
                tx,
                local_claim) &&
            local_claim.source_address == 0x20U &&
            local_claim.raw_name == raw_name(local_name),
        "ISOBUS layer preserves local Address Claim TX");

    const auto remote = name(300U);
    manager.on_can_frame(
        claimed_frame(remote, 0x80U, 1));

    ControlFunctionRecord record{};
    failures += require(
        manager.status() == NetworkManagementStatus::ok &&
            manager.local_address() == 0x20U &&
            manager.registered_control_functions() == 1U &&
            manager.find_by_name(
                raw_name(remote),
                record) == LookupStatus::found &&
            record.source_address == 0x80U,
        "ISOBUS layer composes local manager and remote registry");
  }

  {
    NetworkManagement manager;
    const auto local_name = name(20U, true);
    auto cfg = config(local_name, 0x80U);
    cfg.alternative_addresses[0U] = 0x81U;
    cfg.alternative_count = 1U;

    failures += require(
        manager.configure(cfg) &&
            manager.start(reading(0)) ==
                NetworkManagementStatus::ok,
        "ISOBUS conflict setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain ISOBUS conflict initial claim");

    const auto higher_priority_remote = name(10U);
    manager.on_can_frame(
        claimed_frame(
            higher_priority_remote,
            0x80U,
            1000000));

    j1939::AddressClaimedMessage replacement{};
    ControlFunctionRecord record{};
    failures += require(
        manager.local_address() == 0x81U &&
            manager.local_address_claim_state() ==
                j1939::AddressClaimState::claiming &&
            manager.try_take_tx(tx) &&
            j1939::decode_address_claimed(tx, replacement) &&
            replacement.source_address == 0x81U &&
            manager.find_by_name(
                raw_name(higher_priority_remote),
                record) == LookupStatus::found &&
            record.source_address == 0x80U &&
            manager.counters().local.address_conflicts == 1U,
        "ISOBUS registry observes conflict while J1939 engine arbitrates");
  }

  {
    NetworkManagement manager;
    failures += require(
        manager.configure(config(name(500U), 0x20U)) &&
            manager.start(reading(0)) ==
                NetworkManagementStatus::ok,
        "registry overflow wrapper setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain registry overflow wrapper claim");

    for (std::size_t index = 0U;
         index < ControlFunctionRegistry::kCapacity + 1U;
         ++index) {
      const auto identity =
          static_cast<std::uint32_t>(1000U + index);
      const auto address =
          static_cast<std::uint8_t>(0x40U + (index % 64U));
      manager.on_can_frame(
          claimed_frame(
              name(identity),
              address,
              static_cast<std::int64_t>(index + 1U)));
    }

    failures += require(
        manager.status() ==
                NetworkManagementStatus::
                    registry_capacity_exhausted &&
            manager.registered_control_functions() ==
                ControlFunctionRegistry::kCapacity,
        "network manager exposes incomplete remote topology");

    manager.on_can_frame(
        request_address_claim(
            0xA0U,
            j1939::kGlobalAddress,
            1000));
    j1939::AddressClaimedMessage response{};
    failures += require(
        manager.try_take_tx(tx) &&
            j1939::decode_address_claimed(tx, response) &&
            response.source_address == 0x20U,
        "local address safety remains responsive after registry overflow");
  }

  if (failures == 0) {
    std::cout
        << "CORE_V2_ISOBUS_NETWORK_MANAGEMENT=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
