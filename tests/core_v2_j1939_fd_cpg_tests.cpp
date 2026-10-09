#include "ecu/core_v2/protocol/j1939/fd_contained_pg.hpp"

#include <array>
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
    std::array<std::byte, kContainedPgHeaderBytes>
        header{};
    failures += require(
        encode_no_assurance_contained_pg_header(
            0x00F100U,
            4U,
            header) &&
            header[0U] == std::byte{0x40U} &&
            header[1U] == std::byte{0xF1U} &&
            header[2U] == std::byte{0x00U} &&
            header[3U] == std::byte{0x04U},
        "public no-assurance C-PG header vector");

    ContainedPgHeader decoded{};
    failures += require(
        decode_no_assurance_contained_pg_header(
            header,
            decoded) ==
                ContainedPgDecodeStatus::ok &&
            decoded.type_of_service ==
                kContainedPgTosSaeNoAssurance &&
            decoded.trailer_format ==
                kContainedPgTrailerNone &&
            decoded.pgn == 0x00F100U &&
            decoded.payload_length == 4U,
        "C-PG header round trip");
  }

  {
    std::array<std::byte, kContainedPgHeaderBytes>
        header{};
    failures += require(
        encode_no_assurance_contained_pg_header(
            0x01F234U,
            60U,
            header) &&
            header[0U] == std::byte{0x41U} &&
            header[1U] == std::byte{0xF2U} &&
            header[2U] == std::byte{0x34U} &&
            header[3U] == std::byte{60U},
        "C-PG carries high PGN bits and 60-byte maximum");
    failures += require(
        !encode_no_assurance_contained_pg_header(
            0x00EA01U,
            1U,
            header),
        "non-canonical PDU1 C-PG PGN rejected");
    failures += require(
        !encode_no_assurance_contained_pg_header(
            0x00F100U,
            61U,
            header),
        "C-PG payload above 60 bytes rejected");
  }

  {
    ExtendedMultiPgBuilder builder;
    const std::array<std::byte, 4U> first{
        std::byte{0x11U},
        std::byte{0x22U},
        std::byte{0x33U},
        std::byte{0x44U}};
    const std::array<std::byte, 3U> second{
        std::byte{0xAAU},
        std::byte{0xBBU},
        std::byte{0xCCU}};

    failures += require(
        builder.begin(
            3U,
            0x31U,
            kGlobalAddress) &&
            builder.outer_format() ==
                MultiPgOuterFormat::feff &&
            builder.append(
                0x00F100U,
                first.data(),
                static_cast<std::uint8_t>(
                    first.size())) &&
            builder.append(
                0x00F200U,
                second.data(),
                static_cast<std::uint8_t>(
                    second.size())) &&
            builder.used_payload_bytes() == 15U,
        "two C-PGs fit one FEFF Multi-PG builder");

    transport::CanFrame frame{};
    failures += require(
        builder.finalize(frame) &&
            !builder.active() &&
            frame.identifier == 0x0C25FF31U &&
            frame.identifier_format ==
                transport::CanIdentifierFormat::
                    extended_29_bit &&
            frame.format ==
                transport::CanFrameFormat::fd &&
            frame.type ==
                transport::CanFrameType::data &&
            frame.bit_rate_switch &&
            !frame.error_state_indicator &&
            frame.length == 16U,
        "FEFF Multi-PG envelope matches public vector");

    const std::array<std::uint8_t, 16U> expected{
        0x40U, 0xF1U, 0x00U, 0x04U,
        0x11U, 0x22U, 0x33U, 0x44U,
        0x40U, 0xF2U, 0x00U, 0x03U,
        0xAAU, 0xBBU, 0xCCU, 0x00U};
    bool payload_matches = true;
    for (std::size_t index = 0U;
         index < expected.size();
         ++index) {
      if (frame.payload[index] !=
          static_cast<std::byte>(
              expected[index])) {
        payload_matches = false;
      }
    }
    failures += require(
        payload_matches,
        "FEFF Multi-PG payload matches public vector");

    MultiPgEnvelope envelope{};
    failures += require(
        decode_multi_pg_envelope(
            frame,
            envelope) &&
            envelope.format ==
                MultiPgOuterFormat::feff &&
            envelope.priority == 3U &&
            envelope.source_address == 0x31U &&
            envelope.destination_address ==
                kGlobalAddress,
        "FEFF Multi-PG envelope decodes");

    MultiPgEnvelope extended_only{};
    failures += require(
        decode_extended_multi_pg_envelope(
            frame,
            extended_only) &&
            extended_only.format ==
                MultiPgOuterFormat::feff,
        "FEFF compatibility decoder remains available");

    ContainedPgView view{};
    std::size_t next = 0U;
    failures += require(
        decode_contained_pg_at(
            frame,
            0U,
            view,
            next) ==
                ContainedPgDecodeStatus::ok &&
            view.pgn == 0x00F100U &&
            view.payload_length == 4U &&
            view.payload_offset == 4U &&
            next == 8U,
        "first C-PG decodes from FEFF Multi-PG");

    failures += require(
        decode_contained_pg_at(
            frame,
            next,
            view,
            next) ==
                ContainedPgDecodeStatus::ok &&
            view.pgn == 0x00F200U &&
            view.payload_length == 3U &&
            view.payload_offset == 12U &&
            next == 15U,
        "second C-PG decodes from FEFF Multi-PG");

    failures += require(
        decode_contained_pg_at(
            frame,
            next,
            view,
            next) ==
            ContainedPgDecodeStatus::padding,
        "FEFF DLC alignment padding terminates C-PG iteration");
  }

  {
    ExtendedMultiPgBuilder builder;
    const std::array<std::byte, 4U> first{
        std::byte{0x11U},
        std::byte{0x22U},
        std::byte{0x33U},
        std::byte{0x44U}};
    const std::array<std::byte, 3U> second{
        std::byte{0xAAU},
        std::byte{0xBBU},
        std::byte{0xCCU}};

    failures += require(
        builder.begin_fbff(0x31U) &&
            builder.outer_format() ==
                MultiPgOuterFormat::fbff &&
            builder.append(
                0x00F100U,
                first.data(),
                static_cast<std::uint8_t>(
                    first.size())) &&
            builder.append(
                0x00F200U,
                second.data(),
                static_cast<std::uint8_t>(
                    second.size())),
        "FBFF global Multi-PG builder accepts C-PGs");

    transport::CanFrame frame{};
    failures += require(
        builder.finalize(frame) &&
            frame.identifier == 0x31U &&
            frame.identifier_format ==
                transport::CanIdentifierFormat::
                    standard_11_bit &&
            frame.format ==
                transport::CanFrameFormat::fd &&
            frame.bit_rate_switch &&
            frame.length == 16U,
        "FBFF uses AppPI 000 plus source address");

    MultiPgEnvelope envelope{};
    failures += require(
        decode_multi_pg_envelope(
            frame,
            envelope) &&
            envelope.format ==
                MultiPgOuterFormat::fbff &&
            envelope.priority ==
                kFbffApplicationPriority &&
            envelope.source_address == 0x31U &&
            envelope.destination_address ==
                kGlobalAddress,
        "FBFF global envelope decodes");

    MultiPgEnvelope extended_only{};
    failures += require(
        !decode_extended_multi_pg_envelope(
            frame,
            extended_only),
        "FEFF-only compatibility helper rejects FBFF");

    ContainedPgView view{};
    std::size_t next = 0U;
    failures += require(
        decode_contained_pg_at(
            frame,
            0U,
            view,
            next) ==
                ContainedPgDecodeStatus::ok &&
            view.pgn == 0x00F100U &&
            next == 8U &&
            decode_contained_pg_at(
                frame,
                next,
                view,
                next) ==
                ContainedPgDecodeStatus::ok &&
            view.pgn == 0x00F200U &&
            next == 15U &&
            decode_contained_pg_at(
                frame,
                next,
                view,
                next) ==
                ContainedPgDecodeStatus::padding,
        "C-PG iterator is outer-format neutral");
  }

  {
    ExtendedMultiPgBuilder builder;
    std::array<std::byte, 21U> payload{};
    failures += require(
        builder.begin_fbff(0x80U) &&
            builder.append(
                0x00F100U,
                payload.data(),
                static_cast<std::uint8_t>(
                    payload.size())),
        "long-padding FBFF setup");

    transport::CanFrame frame{};
    failures += require(
        builder.finalize(frame) &&
            frame.length == 32U &&
            builder.used_payload_bytes() == 25U &&
            frame.payload[25U] ==
                std::byte{0U} &&
            frame.payload[26U] ==
                std::byte{0U} &&
            frame.payload[27U] ==
                std::byte{0U} &&
            frame.payload[28U] ==
                std::byte{0xAAU} &&
            frame.payload[31U] ==
                std::byte{0xAAU},
        "J1939-22 padding service fills long DLC gap deterministically");

    ContainedPgView view{};
    std::size_t next = 0U;
    failures += require(
        decode_contained_pg_at(
            frame,
            0U,
            view,
            next) ==
                ContainedPgDecodeStatus::ok &&
            next == 25U &&
            decode_contained_pg_at(
                frame,
                next,
                view,
                next) ==
                ContainedPgDecodeStatus::padding,
        "long padding service decodes");

    auto malformed = frame;
    malformed.payload[27U] =
        std::byte{0xAAU};
    failures += require(
        decode_contained_pg_at(
            malformed,
            25U,
            view,
            next) ==
            ContainedPgDecodeStatus::invalid_argument,
        "padding service rejects malformed zero prefix");

    malformed = frame;
    malformed.payload[30U] =
        std::byte{0U};
    failures += require(
        decode_contained_pg_at(
            malformed,
            25U,
            view,
            next) ==
            ContainedPgDecodeStatus::invalid_argument,
        "padding service rejects malformed 0xAA tail");
  }

  {
    ExtendedMultiPgBuilder builder;
    std::array<std::byte, 60U> payload{};
    for (std::size_t index = 0U;
         index < payload.size();
         ++index) {
      payload[index] =
          static_cast<std::byte>(
              static_cast<std::uint8_t>(
                  index));
    }

    failures += require(
        builder.begin(
            6U,
            0x80U,
            0x90U) &&
            builder.append(
                0x00EA00U,
                payload.data(),
                static_cast<std::uint8_t>(
                    payload.size())),
        "destination-specific FEFF PDU1 C-PG uses outer destination");

    transport::CanFrame frame{};
    failures += require(
        builder.finalize(frame) &&
            frame.length == 64U,
        "60-byte C-PG exactly fills 64-byte CAN FD payload");

    MultiPgEnvelope envelope{};
    failures += require(
        decode_multi_pg_envelope(
            frame,
            envelope) &&
            envelope.format ==
                MultiPgOuterFormat::feff &&
            envelope.destination_address ==
                0x90U,
        "addressed FEFF Multi-PG preserves one outer destination");

    ContainedPgView view{};
    std::size_t next = 0U;
    failures += require(
        decode_contained_pg_at(
            frame,
            0U,
            view,
            next) ==
                ContainedPgDecodeStatus::ok &&
            view.pgn == 0x00EA00U &&
            view.payload_length == 60U &&
            next == 64U,
        "maximum no-assurance C-PG decodes");
  }

  {
    ExtendedMultiPgBuilder builder;
    const std::byte one{0x55U};
    failures += require(
        !builder.begin(
            6U,
            kNullAddress,
            kGlobalAddress),
        "NULL source rejected for FEFF Multi-PG");
    failures += require(
        !builder.begin(
            6U,
            kGlobalAddress,
            kGlobalAddress),
        "global source rejected for FEFF Multi-PG");
    failures += require(
        !builder.begin(
            6U,
            0x80U,
            kNullAddress),
        "NULL destination rejected for FEFF Multi-PG");
    failures += require(
        !builder.begin_fbff(kNullAddress) &&
            !builder.begin_fbff(
                kGlobalAddress),
        "FBFF rejects NULL/global source");

    failures += require(
        builder.begin(
            6U,
            0x80U,
            kGlobalAddress),
        "valid FEFF builder starts");
    failures += require(
        !builder.append(
            0x00EA01U,
            &one,
            1U),
        "builder rejects non-canonical PDU1 PGN");

    transport::CanFrame empty{};
    ExtendedMultiPgBuilder empty_builder;
    failures += require(
        empty_builder.begin_fbff(0x80U) &&
            !empty_builder.finalize(empty),
        "empty FBFF Multi-PG cannot be emitted");
  }

  {
    ExtendedMultiPgBuilder builder;
    std::array<std::byte, 60U> payload{};
    failures += require(
        builder.begin(
            6U,
            0x80U,
            kGlobalAddress) &&
            builder.append(
                0x00F100U,
                payload.data(),
                static_cast<std::uint8_t>(
                    payload.size())) &&
            !builder.append(
                0x00F200U,
                payload.data(),
                1U),
        "Multi-PG builder rejects payload overflow");

    transport::CanFrame frame{};
    failures += require(
        builder.finalize(frame) &&
            frame.length == 64U,
        "failed overflow append leaves prior valid C-PG intact");
  }

  {
    std::array<std::byte, kContainedPgHeaderBytes>
        header{
            std::byte{0x20U},
            std::byte{0xF1U},
            std::byte{0x00U},
            std::byte{0x01U}};
    ContainedPgHeader decoded{};
    failures += require(
        decode_no_assurance_contained_pg_header(
            header,
            decoded) ==
            ContainedPgDecodeStatus::unsupported_profile,
        "non-SAE-no-assurance TOS fails closed");

    header[0U] = std::byte{0x44U};
    failures += require(
        decode_no_assurance_contained_pg_header(
            header,
            decoded) ==
            ContainedPgDecodeStatus::unsupported_profile,
        "nonzero trailer format fails closed");
  }

  {
    ExtendedMultiPgBuilder builder;
    const std::byte payload{0x01U};
    transport::CanFrame frame{};
    failures += require(
        builder.begin(
            6U,
            0x80U,
            kGlobalAddress) &&
            builder.append(
                0x00F100U,
                &payload,
                1U) &&
            builder.finalize(frame),
        "envelope mutation setup");

    MultiPgEnvelope envelope{};

    frame.bit_rate_switch = false;
    failures += require(
        !decode_multi_pg_envelope(
            frame,
            envelope),
        "supported J1939-22 profile requires BRS");

    frame.bit_rate_switch = true;
    frame.identifier_format =
        transport::CanIdentifierFormat::standard_11_bit;
    frame.identifier = 0x131U;
    failures += require(
        !decode_multi_pg_envelope(
            frame,
            envelope),
        "FBFF rejects nonzero AppPI bits");

    frame.identifier = kNullAddress;
    failures += require(
        !decode_multi_pg_envelope(
            frame,
            envelope),
        "FBFF rejects NULL source address");
  }

  if (failures == 0) {
    std::cout
        << "CORE_V2_J1939_FD_CPG_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
