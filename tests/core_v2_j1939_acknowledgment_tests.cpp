#include "ecu/core_v2/protocol/j1939/acknowledgment.hpp"
#include "ecu/core_v2/protocol/j1939/diagnostics.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::j1939;

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

transport::CanFrame acknowledgment_frame(
    const std::uint8_t control,
    const std::uint8_t response_source = 0x80U,
    const std::uint8_t request_source = kNullAddress,
    const std::uint32_t requested_pgn = kDm2Pgn) {
  const std::uint8_t payload[8] = {
      control,
      0xFFU,
      0xFFU,
      0xFFU,
      request_source,
      static_cast<std::uint8_t>(requested_pgn & 0xFFU),
      static_cast<std::uint8_t>((requested_pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((requested_pgn >> 16U) & 0xFFU)};

  transport::CanFrame frame{};
  (void)build_classic_data_frame(
      MessageAddress{
          6U,
          kAcknowledgmentPgn,
          response_source,
          kGlobalAddress},
      payload,
      8U,
      frame);
  return frame;
}

}  // namespace

int main() {
  int failures = 0;

  for (std::uint8_t control = 0U; control <= 3U; ++control) {
    const auto frame = acknowledgment_frame(control);
    AcknowledgmentMessage decoded{};
    failures += require(
        decode_acknowledgment(frame, decoded) &&
            static_cast<std::uint8_t>(decoded.control) == control &&
            decoded.group_function_value == 0xFFU &&
            decoded.request_source_address == kNullAddress &&
            decoded.response_source_address == 0x80U &&
            decoded.requested_pgn == kDm2Pgn,
        "ACK control variants decode");
  }

  {
    auto frame = acknowledgment_frame(1U);
    AcknowledgmentMessage decoded{};

    frame.length = 7U;
    failures += require(
        !decode_acknowledgment(frame, decoded),
        "Acknowledgment requires exact DLC 8");

    frame = acknowledgment_frame(4U);
    failures += require(
        !decode_acknowledgment(frame, decoded),
        "unknown Acknowledgment control rejected");

    frame = acknowledgment_frame(1U);
    frame.payload[2U] = std::byte{0U};
    failures += require(
        !decode_acknowledgment(frame, decoded),
        "Acknowledgment reserved bytes validated");

    frame = acknowledgment_frame(1U);
    frame.payload[4U] = std::byte{0xFFU};
    failures += require(
        !decode_acknowledgment(frame, decoded),
        "global request-source address rejected in Acknowledgment");

    frame = acknowledgment_frame(1U);
    frame.payload[5U] = std::byte{0x01U};
    frame.payload[6U] = std::byte{0xEAU};
    frame.payload[7U] = std::byte{0x00U};
    failures += require(
        !decode_acknowledgment(frame, decoded),
        "non-canonical acknowledged PGN rejected");
  }

  {
    auto frame = acknowledgment_frame(1U);
    AcknowledgmentMessage decoded{};

    frame.identifier =
        (frame.identifier & 0x1FFF00FFU) |
        (static_cast<std::uint32_t>(0x90U) << 8U);
    failures += require(
        !decode_acknowledgment(frame, decoded),
        "Acknowledgment must use global destination");

    frame = acknowledgment_frame(1U);
    frame.identifier =
        (frame.identifier & 0x1FFFFF00U) |
        kNullAddress;
    failures += require(
        !decode_acknowledgment(frame, decoded),
        "Acknowledgment responder must have a claimed source address");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_J1939_ACKNOWLEDGMENT_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
