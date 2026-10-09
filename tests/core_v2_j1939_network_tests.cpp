#include "ecu/core_v2/protocol/j1939/address_claim.hpp"
#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"
#include "ecu/core_v2/protocol/j1939/j1939_name.hpp"
#include "ecu/core_v2/protocol/j1939/network_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::j1939;

constexpr time::MonotonicClockDomainId kDomain{0x1939U};

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

time::MonotonicClockReading reading(
    const std::int64_t nanoseconds,
    const std::int64_t uncertainty = 0) {
  return {
      time::MonotonicClockStatus::ok,
      kDomain,
      time::MonotonicTime{nanoseconds},
      time::MonotonicDuration{uncertainty}};
}

NameFields name(
    const std::uint32_t identity,
    const bool arbitrary = false) {
  NameFields fields{};
  fields.identity_number = identity;
  fields.manufacturer_code = 0x123U;
  fields.ecu_instance = 0U;
  fields.function_instance = 0U;
  fields.function = 0x17U;
  fields.vehicle_system = 0x01U;
  fields.vehicle_system_instance = 0U;
  fields.industry_group = 0x02U;
  fields.arbitrary_address_capable = arbitrary;
  return fields;
}

AddressClaimConfig config(
    const NameFields& fields,
    const std::uint8_t preferred) {
  AddressClaimConfig value{};
  value.name = fields;
  value.preferred_address = preferred;
  value.timestamp_domain = kDomain;
  value.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  return value;
}

transport::ReceivedCanFrame claimed_frame(
    const NameFields& fields,
    const std::uint8_t source,
    const std::int64_t timestamp) {
  transport::ReceivedCanFrame received{};
  std::array<std::byte, 8U> payload{};
  const bool encoded = encode_name_payload(fields, payload);
  std::uint32_t identifier = 0U;
  const bool identifier_ok = encode_identifier(
      MessageAddress{
          6U,
          kAddressClaimedPgn,
          source,
          kGlobalAddress},
      identifier);
  if (!encoded || !identifier_ok) {
    return received;
  }

  received.frame.identifier = identifier;
  received.frame.identifier_format =
      transport::CanIdentifierFormat::extended_29_bit;
  received.frame.format = transport::CanFrameFormat::classic;
  received.frame.type = transport::CanFrameType::data;
  received.frame.length = 8U;
  for (std::size_t i = 0U; i < payload.size(); ++i) {
    received.frame.payload[i] = payload[i];
  }
  received.timestamp = reading(timestamp);
  return received;
}

transport::ReceivedCanFrame request_address_claim(
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::int64_t timestamp) {
  transport::ReceivedCanFrame received{};
  const std::uint8_t payload[3] = {
      static_cast<std::uint8_t>(kAddressClaimedPgn & 0xFFU),
      static_cast<std::uint8_t>((kAddressClaimedPgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((kAddressClaimedPgn >> 16U) & 0xFFU)};
  const bool built = build_classic_data_frame(
      MessageAddress{
          6U,
          kRequestPgn,
          source,
          destination},
      payload,
      3U,
      received.frame);
  if (!built) {
    return received;
  }
  received.timestamp = reading(timestamp);
  return received;
}

TpMessage commanded_address_message(
    const NameFields& target,
    const std::uint8_t new_address,
    const std::uint8_t source = 0xA0U) {
  TpMessage message{};
  message.pgn = kCommandedAddressPgn;
  message.source_address = source;
  message.destination_address = kGlobalAddress;
  message.size = 9U;
  message.broadcast = true;

  std::array<std::byte, 8U> payload{};
  if (!encode_name_payload(target, payload)) {
    return {};
  }
  for (std::size_t i = 0U; i < payload.size(); ++i) {
    message.data[i] = payload[i];
  }
  message.data[8U] = static_cast<std::byte>(new_address);
  return message;
}

bool frame_source(
    const transport::CanFrame& frame,
    std::uint8_t& source,
    std::uint32_t& pgn) {
  IdentifierFields fields{};
  if (!decode_classic_frame_identifier(frame, fields)) {
    return false;
  }
  source = fields.source_address;
  pgn = parameter_group_number(fields);
  return true;
}

}  // namespace

int main() {
  int failures = 0;

  {
    IdentifierFields fields{};
    failures += require(
        decode_identifier(0x18FEAE30U, fields) &&
            fields.priority == 6U &&
            !fields.reserved &&
            !fields.data_page &&
            fields.pdu_format == 0xFEU &&
            fields.pdu_specific == 0xAEU &&
            fields.source_address == 0x30U &&
            !is_pdu1(fields) &&
            parameter_group_number(fields) == 0xFEAEU,
        "J1939 PDU2 decode and PGN");

    failures += require(
        decode_identifier(0x18DA30F9U, fields) &&
            is_pdu1(fields) &&
            parameter_group_number(fields) == 0xDA00U,
        "J1939 PDU1 decode and PGN");

    std::uint8_t destination = 0U;
    failures += require(
        destination_address(fields, destination) &&
            destination == 0x30U,
        "J1939 PDU1 destination");

    std::uint32_t identifier = 0U;
    failures += require(
        encode_identifier(
            MessageAddress{6U, 0xDA00U, 0xF9U, 0x30U},
            identifier) &&
            identifier == 0x18DA30F9U,
        "J1939 PDU1 encode");
    failures += require(
        encode_identifier(
            MessageAddress{6U, 0xFEAEU, 0x30U, kGlobalAddress},
            identifier) &&
            identifier == 0x18FEAE30U,
        "J1939 PDU2 encode");
    failures += require(
        !encode_identifier(
            MessageAddress{6U, 0xDA01U, 0xF9U, 0x30U},
            identifier),
        "PDU1 low PGN byte rejected");
    failures += require(
        !encode_identifier(
            MessageAddress{6U, 0xFEAEU, 0x30U, 0x30U},
            identifier),
        "PDU2 destination ambiguity rejected");
    failures += require(
        !decode_identifier(0x02000000U, fields),
        "reserved identifier bit rejected");
    failures += require(
        !decode_identifier(0x20000000U, fields),
        "identifier above 29 bits rejected");
    failures += require(
        !decode_identifier(0x18FEAEFFU, fields),
        "global address rejected as J1939 source");
    failures += require(
        !encode_identifier(
            MessageAddress{
                6U, 0xFEAEU, kGlobalAddress, kGlobalAddress},
            identifier),
        "global address rejected by J1939 encoder as source");
  }

  {
    NameFields fields = name(0x12345U, true);
    fields.ecu_instance = 5U;
    fields.function_instance = 17U;
    fields.vehicle_system_instance = 9U;

    std::uint64_t raw = 0U;
    failures += require(
        encode_name(fields, raw),
        "J1939 NAME encode");
    const auto decoded = decode_name(raw);
    failures += require(
        decoded.identity_number == fields.identity_number &&
            decoded.manufacturer_code == fields.manufacturer_code &&
            decoded.ecu_instance == fields.ecu_instance &&
            decoded.function_instance == fields.function_instance &&
            decoded.function == fields.function &&
            decoded.vehicle_system == fields.vehicle_system &&
            decoded.vehicle_system_instance ==
                fields.vehicle_system_instance &&
            decoded.industry_group == fields.industry_group &&
            decoded.arbitrary_address_capable,
        "J1939 NAME round trip");

    std::array<std::byte, 8U> payload{};
    failures += require(
        encode_name_payload(fields, payload) &&
            decode_name_payload(payload) == raw,
        "J1939 NAME little-endian payload round trip");

    auto invalid = fields;
    invalid.reserved = true;
    failures += require(
        !is_valid_name(invalid) &&
            !encode_name(invalid, raw),
        "J1939 NAME reserved bit rejected");

    std::uint64_t low = 0U;
    std::uint64_t high = 0U;
    failures += require(
        encode_name(name(1U), low) &&
            encode_name(name(2U), high) &&
            name_has_higher_priority(low, high),
        "lower numeric NAME wins arbitration");
  }

  {
    AddressClaimEngine engine;
    auto cfg = config(name(10U), 0x80U);
    failures += require(
        engine.configure(cfg),
        "address claim configuration");
    const auto started = engine.begin(reading(0));
    failures += require(
        started.status == AddressClaimStatus::ok &&
            started.tx_kind == AddressClaimTxKind::address_claim &&
            engine.state() == AddressClaimState::claiming &&
            engine.current_address() == 0x80U,
        "preferred address starts claiming");
    failures += require(
        engine.poll(reading(249999999)).status ==
                AddressClaimStatus::no_action &&
            engine.state() == AddressClaimState::claiming,
        "250 ms claim window not shortened");
    const auto stable = engine.poll(reading(250000000));
    failures += require(
        stable.status == AddressClaimStatus::ok &&
            stable.became_claimed &&
            engine.state() == AddressClaimState::claimed,
        "claim completes after 250 ms");
  }

  {
    AddressClaimEngine engine;
    failures += require(
        engine.configure(config(name(10U), 0x20U)),
        "immediate address config");
    const auto started = engine.begin(reading(0));
    failures += require(
        started.became_claimed &&
            engine.state() == AddressClaimState::claimed,
        "0..127 address can become active immediately");
  }

  {
    AddressClaimEngine engine;
    auto bad = config(name(10U), 0x80U);
    bad.alternative_addresses[0] = 0x81U;
    bad.alternative_count = 1U;
    failures += require(
        !engine.configure(bad),
        "non-arbitrary NAME cannot configure fallback addresses");

    auto duplicate = config(name(10U, true), 0x80U);
    duplicate.alternative_addresses[0] = 0x81U;
    duplicate.alternative_addresses[1] = 0x81U;
    duplicate.alternative_count = 2U;
    failures += require(
        !engine.configure(duplicate),
        "duplicate fallback addresses rejected");
  }

  {
    NetworkManager manager;
    auto cfg = config(name(10U), 0x80U);
    failures += require(
        manager.configure(cfg) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "network manager start");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "initial address claim queued");

    manager.on_can_frame(
        claimed_frame(name(20U), 0x80U, 1000000));
    std::uint8_t source = 0U;
    std::uint32_t pgn = 0U;
    failures += require(
        manager.try_take_tx(tx) &&
            frame_source(tx, source, pgn) &&
            source == 0x80U &&
            pgn == kAddressClaimedPgn &&
            manager.current_address() == 0x80U &&
            manager.counters().address_conflicts == 1U,
        "higher-priority local NAME reasserts address and records conflict");
  }

  {
    NetworkManager manager;
    auto cfg = config(name(20U, true), 0x80U);
    cfg.alternative_addresses[0] = 0x81U;
    cfg.alternative_count = 1U;
    failures += require(
        manager.configure(cfg) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "arbitrary address manager start");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain initial arbitrary claim");

    manager.on_can_frame(
        claimed_frame(name(10U), 0x80U, 1000000));
    std::uint8_t source = 0U;
    std::uint32_t pgn = 0U;
    const auto counters = manager.counters();
    failures += require(
        manager.current_address() == 0x81U &&
            manager.address_claim_state() ==
                AddressClaimState::claiming &&
            manager.try_take_tx(tx) &&
            frame_source(tx, source, pgn) &&
            source == 0x81U &&
            pgn == kAddressClaimedPgn &&
            counters.address_conflicts == 1U &&
            counters.address_losses == 1U,
        "lower remote NAME forces bounded fallback address");
  }

  {
    NetworkManager manager;
    failures += require(
        manager.configure(config(name(20U), 0x80U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "single-address manager start");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain single-address initial claim");

    manager.on_can_frame(
        claimed_frame(name(10U), 0x80U, 1000000));
    std::uint8_t source = 0U;
    std::uint32_t pgn = 0U;
    failures += require(
        manager.address_claim_state() ==
                AddressClaimState::cannot_claim &&
            manager.current_address() == kNullAddress &&
            manager.try_take_tx(tx) &&
            frame_source(tx, source, pgn) &&
            source == kNullAddress &&
            pgn == kAddressClaimedPgn,
        "address loss without fallback sends Cannot Claim");
  }

  {
    NetworkManager manager;
    failures += require(
        manager.configure(config(name(10U), 0x20U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "request response manager start");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain immediate claim");

    manager.on_can_frame(
        request_address_claim(
            0xA0U, kGlobalAddress, 1000000));
    std::uint8_t source = 0U;
    std::uint32_t pgn = 0U;
    failures += require(
        manager.try_take_tx(tx) &&
            frame_source(tx, source, pgn) &&
            source == 0x20U &&
            pgn == kAddressClaimedPgn,
        "Request for Address Claimed gets response");
  }

  {
    NetworkManager manager;
    failures += require(
        manager.configure(config(name(10U), 0x20U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "strict Request DLC setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain strict Request DLC setup claim");

    auto malformed_request =
        request_address_claim(
            0xA0U, kGlobalAddress, 1000000);
    malformed_request.frame.length = 8U;
    manager.on_can_frame(malformed_request);
    failures += require(
        manager.pending_tx_count() == 0U &&
            manager.counters().malformed_management_frames == 1U,
        "Request PGN requires exact three-byte payload");
  }

  {
    NetworkManager manager;
    const auto local_name = name(10U);
    failures += require(
        manager.configure(config(local_name, 0x20U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "Commanded Address disabled setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain Commanded Address disabled setup claim");

    const auto command =
        commanded_address_message(local_name, 0x30U);
    failures += require(
        manager.on_transport_message(
            command, reading(1000000)) ==
                NetworkManagerStatus::no_action &&
            manager.current_address() == 0x20U &&
            manager.counters().commanded_address_messages == 1U &&
            manager.counters().commanded_address_applied == 0U,
        "Commanded Address is disabled by default");
  }

  {
    NetworkManager manager;
    const auto local_name = name(10U);
    auto cfg = config(local_name, 0x20U);
    cfg.accept_commanded_address = true;
    failures += require(
        manager.configure(cfg) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "Commanded Address enabled setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain Commanded Address enabled setup claim");

    const auto command =
        commanded_address_message(local_name, 0x80U);
    failures += require(
        manager.on_transport_message(
            command, reading(1000000)) ==
                NetworkManagerStatus::ok &&
            manager.current_address() == 0x80U &&
            manager.address_claim_state() ==
                AddressClaimState::claiming &&
            manager.counters().commanded_address_messages == 1U &&
            manager.counters().commanded_address_applied == 1U &&
            manager.counters().address_conflicts == 0U,
        "matching Commanded Address starts claim at requested source");

    std::uint8_t source = 0U;
    std::uint32_t pgn = 0U;
    failures += require(
        manager.try_take_tx(tx) &&
            frame_source(tx, source, pgn) &&
            source == 0x80U &&
            pgn == kAddressClaimedPgn,
        "Commanded Address queues Address Claimed response");

    failures += require(
        manager.service_time(reading(251000000)) ==
                NetworkManagerStatus::ok &&
            manager.address_claim_state() ==
                AddressClaimState::claimed,
        "commanded address honors 250 ms stabilization window");
  }

  {
    NetworkManager manager;
    const auto local_name = name(10U);
    auto cfg = config(local_name, 0x20U);
    cfg.accept_commanded_address = true;
    failures += require(
        manager.configure(cfg) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "Commanded Address malformed setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain malformed Commanded Address setup claim");

    auto malformed =
        commanded_address_message(local_name, kNullAddress);
    failures += require(
        manager.on_transport_message(
            malformed, reading(1000000)) ==
                NetworkManagerStatus::no_action &&
            manager.current_address() == 0x20U &&
            manager.counters().malformed_management_frames == 1U &&
            manager.counters().commanded_address_applied == 0U,
        "Commanded Address rejects NULL target address");

    const auto other =
        commanded_address_message(name(99U), 0x30U);
    failures += require(
        manager.on_transport_message(
            other, reading(2000000)) ==
                NetworkManagerStatus::no_action &&
            manager.current_address() == 0x20U &&
            manager.counters().commanded_address_messages == 2U &&
            manager.counters().commanded_address_applied == 0U,
        "Commanded Address for another NAME is ignored");
  }

  {
    NetworkManager manager;
    auto cfg = config(name(20U), 0x80U);
    cfg.cannot_claim_response_delay =
        time::MonotonicDuration{100000000};
    failures += require(
        manager.configure(cfg) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "delayed Cannot Claim setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain delayed setup claim");

    manager.on_can_frame(
        claimed_frame(name(10U), 0x80U, 1000000));
    failures += require(
        manager.try_take_tx(tx),
        "drain immediate Cannot Claim after collision");

    manager.on_can_frame(
        request_address_claim(
            0xA0U, kGlobalAddress, 2000000));
    failures += require(
        manager.pending_tx_count() == 0U &&
            manager.service_time(reading(101999999)) ==
                NetworkManagerStatus::no_action &&
            manager.pending_tx_count() == 0U,
        "Cannot Claim response delay enforced");
    failures += require(
        manager.service_time(reading(102000000)) ==
                NetworkManagerStatus::ok &&
            manager.try_take_tx(tx),
        "delayed Cannot Claim response released");
    std::uint8_t source = 0U;
    std::uint32_t pgn = 0U;
    failures += require(
        frame_source(tx, source, pgn) &&
            source == kNullAddress &&
            pgn == kAddressClaimedPgn,
        "delayed response uses NULL source");
  }

  {
    NetworkManager manager;
    failures += require(
        manager.configure(config(name(10U), 0x80U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "duplicate NAME setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain duplicate NAME setup claim");

    manager.on_can_frame(
        claimed_frame(name(10U), 0x80U, 1000000));
    failures += require(
        manager.status() ==
                NetworkManagerStatus::protocol_fault &&
            manager.address_claim_state() ==
                AddressClaimState::faulted &&
            manager.pending_tx_count() == 0U,
        "duplicate NAME fails closed");
  }

  {
    NetworkManager manager;
    failures += require(
        manager.configure(config(name(10U), 0x80U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "malformed management setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain malformed setup claim");

    auto malformed =
        claimed_frame(name(20U), 0x80U, 1000000);
    malformed.frame.length = 7U;
    manager.on_can_frame(malformed);
    failures += require(
        manager.status() == NetworkManagerStatus::ok &&
            manager.counters().malformed_management_frames == 1U,
        "malformed management frame observed without global fault");
  }

  {
    NetworkManager manager;
    failures += require(
        manager.configure(config(name(10U), 0x20U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "invalid Address Claimed NAME setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain invalid Address Claimed NAME setup claim");

    auto malformed =
        claimed_frame(name(20U), 0x80U, 1000000);
    malformed.frame.payload[6U] =
        static_cast<std::byte>(
            std::to_integer<std::uint8_t>(
                malformed.frame.payload[6U]) |
            0x01U);
    manager.on_can_frame(malformed);
    failures += require(
        manager.status() == NetworkManagerStatus::ok &&
            manager.counters().malformed_management_frames == 1U &&
            manager.counters().address_conflicts == 0U &&
            manager.current_address() == 0x20U,
        "Address Claimed invalid NAME is rejected before arbitration");
  }

  {
    NetworkManager manager;
    failures += require(
        manager.configure(config(name(10U), 0x20U)) &&
            manager.start(reading(0)) == NetworkManagerStatus::ok,
        "queue overflow setup");
    transport::CanFrame tx{};
    failures += require(
        manager.try_take_tx(tx),
        "drain queue overflow setup claim");

    for (std::uint32_t i = 0U;
         i < NetworkManager::kTxQueueCapacity + 1U;
         ++i) {
      manager.on_can_frame(
          request_address_claim(
              0xA0U,
              kGlobalAddress,
              1000000 + static_cast<std::int64_t>(i)));
    }

    failures += require(
        manager.status() ==
                NetworkManagerStatus::queue_overflow &&
            manager.pending_tx_count() ==
                NetworkManager::kTxQueueCapacity &&
            manager.counters().tx_queue_overflows == 1U,
        "deferred TX queue overflow fails closed");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_J1939_NETWORK_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
