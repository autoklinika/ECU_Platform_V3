#include "ecu/core_v2/protocol/j1939/diagnostics.hpp"
#include "ecu/core_v2/protocol/j1939/request.hpp"

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

}  // namespace

int main() {
  int failures = 0;

  {
    transport::CanFrame frame{};
    failures += require(
        build_request(
            kNullAddress,
            kGlobalAddress,
            kDm2Pgn,
            frame) &&
            frame.identifier == 0x18EAFFFEU &&
            frame.length == 3U &&
            frame.payload[0U] == std::byte{0xCBU} &&
            frame.payload[1U] == std::byte{0xFEU} &&
            frame.payload[2U] == std::byte{0x00U},
        "global DM2 Request supports NULL source before claim");

    RequestMessage decoded{};
    failures += require(
        decode_request(frame, decoded) &&
            decoded.source_address == kNullAddress &&
            decoded.destination_address == kGlobalAddress &&
            decoded.requested_pgn == kDm2Pgn,
        "global Request round trip");
  }

  {
    transport::CanFrame frame{};
    failures += require(
        build_request(0x80U, 0x90U, kAddressClaimedPgn, frame),
        "destination-specific Request builds");

    RequestMessage decoded{};
    failures += require(
        decode_request(frame, decoded) &&
            decoded.source_address == 0x80U &&
            decoded.destination_address == 0x90U &&
            decoded.requested_pgn == kAddressClaimedPgn,
        "destination-specific Request decodes");
  }

  {
    transport::CanFrame frame{};
    failures += require(
        !build_request(
            kGlobalAddress,
            kGlobalAddress,
            kDm2Pgn,
            frame),
        "global address cannot be Request source");
    failures += require(
        !build_request(0x80U, kGlobalAddress, 0xEA01U, frame),
        "non-canonical PDU1 requested PGN rejected");
    failures += require(
        !build_request(
            0x80U,
            kGlobalAddress,
            kMaxPgn + 1U,
            frame),
        "out-of-range requested PGN rejected");
  }

  {
    transport::CanFrame frame{};
    failures += require(
        build_request(0x80U, kGlobalAddress, kDm2Pgn, frame),
        "malformed Request setup");

    RequestMessage decoded{};
    frame.length = 8U;
    failures += require(
        !decode_request(frame, decoded),
        "Request decoder requires exact DLC 3");

    failures += require(
        build_request(0x80U, kGlobalAddress, kDm2Pgn, frame),
        "restore Request for malformed PGN test");
    frame.payload[0U] = std::byte{0x01U};
    frame.payload[1U] = std::byte{0xEAU};
    frame.payload[2U] = std::byte{0x00U};
    failures += require(
        !decode_request(frame, decoded),
        "non-canonical requested PDU1 PGN rejected on RX");

    failures += require(
        build_request(0x80U, kGlobalAddress, kDm2Pgn, frame),
        "restore Request for source test");
    frame.identifier =
        (frame.identifier & 0x1FFFFF00U) |
        kGlobalAddress;
    failures += require(
        !decode_request(frame, decoded),
        "global Request source rejected on RX");
  }

  failures += require(
      is_canonical_pgn(kRequestPgn) &&
          is_canonical_pgn(kAddressClaimedPgn) &&
          is_canonical_pgn(kDm1Pgn) &&
          is_canonical_pgn(kDm2Pgn) &&
          !is_canonical_pgn(0xEA01U),
      "canonical PGN classification");

  if (failures == 0) {
    std::cout << "CORE_V2_J1939_REQUEST_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
