#include "ecu/core_v2/protocol/isobus/working_set.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol;
using namespace ecu::core::v2::protocol::isobus;

constexpr std::uint8_t kMasterAddress = 0x80U;

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

std::uint64_t valid_member_name() {
  j1939::NameFields fields{};
  fields.identity_number = 0x12345U;
  fields.manufacturer_code = 0x321U;
  fields.ecu_instance = 1U;
  fields.function_instance = 2U;
  fields.function = 130U;
  fields.vehicle_system = 7U;
  fields.vehicle_system_instance = 1U;
  fields.industry_group = 2U;
  fields.arbitrary_address_capable = true;

  std::uint64_t raw_name = 0U;
  if (!j1939::encode_name(fields, raw_name)) {
    return 0U;
  }
  return raw_name;
}

bool payload_is_ff_from(
    const transport::CanFrame& frame,
    const std::size_t first) {
  for (std::size_t index = first;
       index < 8U;
       ++index) {
    if (std::to_integer<std::uint8_t>(
            frame.payload[index]) != 0xFFU) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  int failures = 0;

  {
    transport::CanFrame frame{};
    const std::uint8_t member_count = 3U;
    failures += require(
        build_working_set_master(
            kMasterAddress,
            member_count,
            frame),
        "working-set master builds");

    j1939::IdentifierFields fields{};
    failures += require(
        frame.length == 8U &&
            j1939::decode_classic_frame_identifier(
                frame,
                fields) &&
            j1939::parameter_group_number(fields) ==
                kWorkingSetMasterPgn &&
            fields.priority == kWorkingSetPriority &&
            fields.source_address == kMasterAddress &&
            std::to_integer<std::uint8_t>(
                frame.payload[0U]) == member_count &&
            payload_is_ff_from(frame, 1U),
        "working-set master wire format");

    WorkingSetMasterMessage decoded{};
    failures += require(
        decode_working_set_master(frame, decoded) &&
            decoded.source_address == kMasterAddress &&
            decoded.member_count == member_count,
        "working-set master round trip");
  }

  {
    transport::CanFrame frame{};
    const std::uint8_t minimum =
        kMinWorkingSetMembers;
    const std::uint8_t maximum =
        kMaxWorkingSetMembers;
    const std::uint8_t below_minimum = 0U;
    const std::uint8_t above_maximum = 251U;

    failures += require(
        build_working_set_master(
            kMasterAddress,
            minimum,
            frame) &&
            build_working_set_master(
                kMasterAddress,
                maximum,
                frame) &&
            !build_working_set_master(
                kMasterAddress,
                below_minimum,
                frame) &&
            !build_working_set_master(
                kMasterAddress,
                above_maximum,
                frame),
        "working-set member-count boundaries");
  }

  {
    transport::CanFrame frame{};
    const std::uint8_t member_count = 2U;
    (void)build_working_set_master(
        kMasterAddress,
        member_count,
        frame);

    WorkingSetMasterMessage decoded{};
    frame.payload[7U] = std::byte{0x00U};
    failures += require(
        !decode_working_set_master(frame, decoded),
        "working-set master reserved bytes fail closed");

    (void)build_working_set_master(
        kMasterAddress,
        member_count,
        frame);
    frame.length = 7U;
    failures += require(
        !decode_working_set_master(frame, decoded),
        "working-set master requires exact DLC");
  }

  {
    transport::CanFrame frame{};
    const std::uint8_t member_count = 2U;
    failures += require(
        !build_working_set_master(
            j1939::kNullAddress,
            member_count,
            frame) &&
            !build_working_set_master(
                j1939::kGlobalAddress,
                member_count,
                frame),
        "working-set master requires claimed source");
  }

  {
    const auto raw_name = valid_member_name();
    transport::CanFrame frame{};
    failures += require(
        raw_name != 0U &&
            build_working_set_member(
                kMasterAddress,
                raw_name,
                frame),
        "working-set member builds");

    j1939::IdentifierFields fields{};
    WorkingSetMemberMessage decoded{};
    failures += require(
        frame.length == 8U &&
            j1939::decode_classic_frame_identifier(
                frame,
                fields) &&
            j1939::parameter_group_number(fields) ==
                kWorkingSetMemberPgn &&
            fields.priority == kWorkingSetPriority &&
            fields.source_address == kMasterAddress &&
            decode_working_set_member(frame, decoded) &&
            decoded.source_address == kMasterAddress &&
            decoded.member_name == raw_name,
        "working-set member round trip");
  }

  {
    transport::CanFrame frame{};
    const auto invalid_name =
        static_cast<std::uint64_t>(1U) << 48U;
    failures += require(
        !build_working_set_member(
            kMasterAddress,
            invalid_name,
            frame),
        "working-set member rejects invalid NAME");

    std::array<std::uint8_t, 8U> payload{};
    for (std::size_t index = 0U;
         index < payload.size();
         ++index) {
      payload[index] = static_cast<std::uint8_t>(
          (invalid_name >> (index * 8U)) & 0xFFU);
    }
    failures += require(
        j1939::build_classic_data_frame(
            j1939::MessageAddress{
                kWorkingSetPriority,
                kWorkingSetMemberPgn,
                kMasterAddress,
                j1939::kGlobalAddress},
            payload.data(),
            static_cast<std::uint8_t>(payload.size()),
            frame),
        "invalid-NAME test frame builds");

    WorkingSetMemberMessage decoded{};
    failures += require(
        !decode_working_set_member(frame, decoded),
        "working-set member invalid NAME fails closed");
  }

  {
    transport::CanFrame frame{};
    const auto raw_name = valid_member_name();
    failures += require(
        !build_working_set_member(
            j1939::kNullAddress,
            raw_name,
            frame) &&
            !build_working_set_member(
                j1939::kGlobalAddress,
                raw_name,
                frame),
        "working-set member requires claimed master source");
  }

  {
    transport::CanFrame frame{};
    const auto raw_name = valid_member_name();
    (void)build_working_set_member(
        kMasterAddress,
        raw_name,
        frame);

    WorkingSetMasterMessage master{};
    failures += require(
        !decode_working_set_master(frame, master),
        "working-set PGNs cannot cross-decode");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_ISOBUS_WORKING_SET=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
