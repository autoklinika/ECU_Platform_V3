#include "ecu/core_v2/protocol/j1939/diagnostics.hpp"

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

std::array<std::uint8_t, 4U> encoded_dtc(
    const std::uint32_t spn,
    const std::uint8_t fmi,
    const std::uint8_t occurrence_count,
    const bool conversion_method = false) {
  return {
      static_cast<std::uint8_t>(spn & 0xFFU),
      static_cast<std::uint8_t>((spn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(
          (((spn >> 16U) & 0x07U) << 5U) |
          (static_cast<std::uint32_t>(fmi) & 0x1FU)),
      static_cast<std::uint8_t>(
          (conversion_method ? 0x80U : 0U) |
          (occurrence_count & 0x7FU))};
}

transport::CanFrame single_dtc_list(
    const std::uint32_t pgn,
    const std::uint8_t source_address,
    const bool with_dtc,
    const bool conversion_method = false) {
  std::uint8_t payload[8] = {
      0x47U,
      0x73U,
      0U,
      0U,
      0U,
      0U,
      0xFFU,
      0xFFU};

  if (with_dtc) {
    const auto dtc =
        encoded_dtc(
            0x52345U,
            5U,
            12U,
            conversion_method);
    for (std::size_t i = 0U; i < dtc.size(); ++i) {
      payload[2U + i] = dtc[i];
    }
  }

  transport::CanFrame frame{};
  static_cast<void>(
      build_classic_data_frame(
          MessageAddress{
              6U,
              pgn,
              source_address,
              kGlobalAddress},
          payload,
          8U,
          frame));
  return frame;
}

TpMessage transported_dtc_list(
    const std::uint32_t pgn,
    const bool destination_specific = false) {
  TpMessage message{};
  message.pgn = pgn;
  message.source_address = 0x80U;
  message.destination_address =
      destination_specific
          ? 0x90U
          : kGlobalAddress;
  message.size = 10U;
  message.broadcast = !destination_specific;
  message.data[0U] = std::byte{0x47U};
  message.data[1U] = std::byte{0x73U};

  const auto first =
      encoded_dtc(0x52345U, 5U, 12U);
  const auto second =
      encoded_dtc(0x10203U, 7U, 2U);
  for (std::size_t i = 0U; i < first.size(); ++i) {
    message.data[2U + i] =
        static_cast<std::byte>(first[i]);
    message.data[6U + i] =
        static_cast<std::byte>(second[i]);
  }
  return message;
}

transport::CanFrame dm4_no_dtc(
    const std::uint8_t source_address = 0x80U) {
  const std::uint8_t payload[8] = {
      0x00U,
      0x00U,
      0x00U,
      0x00U,
      0x00U,
      0xFFU,
      0xFFU,
      0xFFU};

  transport::CanFrame frame{};
  static_cast<void>(
      build_classic_data_frame(
          MessageAddress{
              6U,
              kDm4Pgn,
              source_address,
              kGlobalAddress},
          payload,
          8U,
          frame));
  return frame;
}

void append_dm4_record(
    TpMessage& message,
    std::size_t& offset,
    const std::uint32_t spn,
    const std::uint8_t fmi,
    const std::uint8_t occurrence_count,
    const std::uint8_t manufacturer_bytes,
    const bool conversion_method = false) {
  const auto payload_length =
      static_cast<std::uint8_t>(
          kDm4StandardPayloadBytes +
          manufacturer_bytes);

  message.data[offset] =
      static_cast<std::byte>(payload_length);

  const auto dtc =
      encoded_dtc(
          spn,
          fmi,
          occurrence_count,
          conversion_method);
  for (std::size_t i = 0U; i < dtc.size(); ++i) {
    message.data[offset + 1U + i] =
        static_cast<std::byte>(dtc[i]);
  }

  message.data[offset + 5U] = std::byte{0x03U};
  message.data[offset + 6U] = std::byte{0x44U};
  message.data[offset + 7U] = std::byte{0x34U};
  message.data[offset + 8U] = std::byte{0x12U};
  message.data[offset + 9U] = std::byte{0x55U};
  message.data[offset + 10U] = std::byte{0x66U};
  message.data[offset + 11U] = std::byte{0x78U};
  message.data[offset + 12U] = std::byte{0x56U};

  for (std::uint8_t index = 0U;
       index < manufacturer_bytes;
       ++index) {
    message.data[
        offset +
        13U +
        static_cast<std::size_t>(index)] =
        static_cast<std::byte>(
            static_cast<std::uint8_t>(
                0xA0U + index));
  }

  offset +=
      static_cast<std::size_t>(payload_length) + 1U;
}

TpMessage transported_dm4(
    const bool destination_specific = false,
    const bool second_conversion_method = false) {
  TpMessage message{};
  message.pgn = kDm4Pgn;
  message.source_address = 0x80U;
  message.destination_address =
      destination_specific
          ? 0x90U
          : kGlobalAddress;
  message.broadcast = !destination_specific;

  std::size_t offset = 0U;
  append_dm4_record(
      message,
      offset,
      0x52345U,
      5U,
      12U,
      0U);
  append_dm4_record(
      message,
      offset,
      0x10203U,
      7U,
      2U,
      2U,
      second_conversion_method);
  message.size =
      static_cast<std::uint16_t>(offset);
  return message;
}

transport::CanFrame dm5_frame(
    const std::uint8_t source_address = 0x80U) {
  const std::uint8_t payload[8] = {
      3U,
      4U,
      0x12U,
      0xA5U,
      0x34U,
      0x12U,
      0x78U,
      0x56U};

  transport::CanFrame frame{};
  static_cast<void>(
      build_classic_data_frame(
          MessageAddress{
              6U,
              kDm5Pgn,
              source_address,
              kGlobalAddress},
          payload,
          8U,
          frame));
  return frame;
}

}  // namespace

int main() {
  int failures = 0;

  {
    const auto frame =
        single_dtc_list(
            kDm1Pgn,
            0x80U,
            false);
    DiagnosticMessageInfo info{};
    failures += require(
        decode_dm_frame(frame, info) &&
            info.pgn == kDm1Pgn &&
            info.source_address == 0x80U &&
            !info.transported &&
            !info.destination_specific &&
            info.destination_address ==
                kGlobalAddress &&
            info.dtc_count == 0U,
        "single-frame DM1 without DTC decodes");

    failures += require(
        info.lamps.malfunction_indicator ==
                DiagnosticLampStatus::on &&
            info.lamps.red_stop ==
                DiagnosticLampStatus::off &&
            info.lamps.amber_warning ==
                DiagnosticLampStatus::on &&
            info.lamps.protect ==
                DiagnosticLampStatus::not_available &&
            info.lamps.malfunction_indicator_flash ==
                DiagnosticLampFlash::fast &&
            info.lamps.red_stop_flash ==
                DiagnosticLampFlash::not_available &&
            info.lamps.amber_warning_flash ==
                DiagnosticLampFlash::slow &&
            info.lamps.protect_flash ==
                DiagnosticLampFlash::not_available,
        "DM lamp and flash bit pairs decode");

    DiagnosticTroubleCode dtc{};
    failures += require(
        decode_dtc(frame, 0U, dtc) ==
            DiagnosticDecodeStatus::invalid_argument,
        "zero-DTC DM1 has no decodable DTC");
  }

  {
    const auto frame =
        single_dtc_list(
            kDm1Pgn,
            0x80U,
            true);
    DiagnosticMessageInfo info{};
    failures += require(
        decode_dm_frame(frame, info) &&
            info.dtc_count == 1U,
        "single-frame DM1 with one DTC decodes");

    DiagnosticTroubleCode dtc{};
    failures += require(
        decode_dtc(frame, 0U, dtc) ==
                DiagnosticDecodeStatus::ok &&
            dtc.spn == 0x52345U &&
            dtc.fmi == 5U &&
            dtc.occurrence_count == 12U &&
            !dtc.conversion_method,
        "current-method DTC fields decode");
    failures += require(
        decode_dtc(frame, 1U, dtc) ==
            DiagnosticDecodeStatus::invalid_argument,
        "DTC index is bounded");
  }

  {
    const auto legacy =
        single_dtc_list(
            kDm1Pgn,
            0x80U,
            true,
            true);
    DiagnosticTroubleCode dtc{};
    failures += require(
        decode_dtc(legacy, 0U, dtc) ==
                DiagnosticDecodeStatus::
                    unsupported_conversion_method &&
            dtc.spn == 0U &&
            dtc.fmi == 5U &&
            dtc.occurrence_count == 12U &&
            dtc.conversion_method,
        "legacy conversion method fails closed without inventing SPN");
  }

  {
    auto malformed =
        single_dtc_list(
            kDm1Pgn,
            0x80U,
            true);
    malformed.length = 7U;
    DiagnosticMessageInfo info{};
    failures += require(
        !decode_dm_frame(malformed, info),
        "single-frame DTC list requires exact DLC 8");

    malformed =
        single_dtc_list(
            kDm1Pgn,
            0x80U,
            true);
    malformed.payload[7U] = std::byte{0U};
    failures += require(
        !decode_dm_frame(malformed, info),
        "single-frame DTC list filler bytes are validated");

    malformed =
        single_dtc_list(
            kDm1Pgn,
            0x80U,
            true);
    malformed.identifier =
        (malformed.identifier & 0x1FFFFF00U) |
        kGlobalAddress;
    failures += require(
        !decode_dm_frame(malformed, info),
        "global source address is rejected for DTC list");
  }

  {
    const auto message =
        transported_dtc_list(kDm1Pgn);
    DiagnosticMessageInfo info{};
    failures += require(
        decode_dm_transport(message, info) &&
            info.pgn == kDm1Pgn &&
            info.source_address == 0x80U &&
            info.transported &&
            !info.destination_specific &&
            info.destination_address ==
                kGlobalAddress &&
            info.dtc_count == 2U,
        "transported DM1 with two DTCs decodes");

    DiagnosticTroubleCode first{};
    DiagnosticTroubleCode second{};
    failures += require(
        decode_dtc(message, 0U, first) ==
                DiagnosticDecodeStatus::ok &&
            first.spn == 0x52345U &&
            first.fmi == 5U &&
            first.occurrence_count == 12U &&
            decode_dtc(message, 1U, second) ==
                DiagnosticDecodeStatus::ok &&
            second.spn == 0x10203U &&
            second.fmi == 7U &&
            second.occurrence_count == 2U,
        "transported DTC records decode independently");
  }

  {
    const auto message =
        transported_dtc_list(
            kDm2Pgn,
            true);
    DiagnosticMessageInfo info{};
    failures += require(
        decode_dm_transport(message, info) &&
            info.pgn == kDm2Pgn &&
            info.dtc_count == 2U &&
            info.destination_specific &&
            info.destination_address == 0x90U,
        "destination-specific RTS/CTS DM2 response decodes");
  }

  {
    constexpr std::array<std::uint32_t, 2U>
        extended_dtc_pgns{
            kDm6Pgn,
            kDm12Pgn};

    for (const auto pgn : extended_dtc_pgns) {
      const auto single =
          single_dtc_list(
              pgn,
              0x81U,
              true);
      DiagnosticMessageInfo single_info{};
      failures += require(
          decode_dm_frame(single, single_info) &&
              single_info.pgn == pgn &&
              single_info.dtc_count == 1U,
          "DM6/DM12 single-frame DTC list decodes");

      const auto transported =
          transported_dtc_list(
              pgn,
              true);
      DiagnosticMessageInfo transport_info{};
      failures += require(
          decode_dm_transport(
              transported,
              transport_info) &&
              transport_info.pgn == pgn &&
              transport_info.dtc_count == 2U &&
              transport_info.destination_specific,
          "DM6/DM12 transported DTC list decodes");
    }
  }

  {
    auto malformed =
        transported_dtc_list(kDm1Pgn);
    DiagnosticMessageInfo info{};

    malformed.size = 9U;
    failures += require(
        !decode_dm_transport(malformed, info),
        "transported DTC-list size must be 2 plus whole DTC records");

    malformed =
        transported_dtc_list(kDm1Pgn);
    malformed.broadcast = true;
    malformed.destination_address = 0x90U;
    failures += require(
        !decode_dm_transport(malformed, info),
        "broadcast DTC list requires global destination");

    malformed =
        transported_dtc_list(
            kDm1Pgn,
            true);
    malformed.destination_address =
        malformed.source_address;
    failures += require(
        !decode_dm_transport(malformed, info),
        "peer DTC list rejects self destination");

    malformed =
        transported_dtc_list(0xFEC0U);
    failures += require(
        !decode_dm_transport(malformed, info),
        "unrelated transported PGN is rejected");
  }

  {
    const auto frame = dm5_frame();
    DiagnosticReadiness1 readiness{};
    failures += require(
        decode_dm5_frame(frame, readiness) &&
            readiness.source_address == 0x80U &&
            readiness.active_dtc_count == 3U &&
            readiness.previously_active_dtc_count == 4U &&
            readiness.obd_compliance == 0x12U &&
            readiness.continuously_monitored_support_status ==
                0xA5U &&
            readiness.noncontinuously_monitored_support_byte5 ==
                0x34U &&
            readiness.noncontinuously_monitored_support_byte6 ==
                0x12U &&
            readiness.noncontinuously_monitored_status_byte7 ==
                0x78U &&
            readiness.noncontinuously_monitored_status_byte8 ==
                0x56U,
        "DM5 readiness fields decode without reinterpretation");

    auto malformed = frame;
    malformed.length = 7U;
    failures += require(
        !decode_dm5_frame(malformed, readiness),
        "DM5 requires exact DLC 8");

    malformed = frame;
    malformed.identifier =
        (malformed.identifier & 0x1FFFFF00U) |
        kGlobalAddress;
    failures += require(
        !decode_dm5_frame(malformed, readiness),
        "DM5 rejects global source");

    malformed =
        single_dtc_list(
            kDm1Pgn,
            0x80U,
            false);
    failures += require(
        !decode_dm5_frame(malformed, readiness),
        "DM5 rejects unrelated PGN");
  }

  {
    const auto frame = dm4_no_dtc();
    DiagnosticFreezeFrameInfo info{};
    failures += require(
        decode_dm4_frame(frame, info) &&
            info.source_address == 0x80U &&
            info.no_dtc &&
            info.record_count == 0U &&
            !info.transported,
        "DM4 no-DTC single-frame response decodes");

    auto malformed = frame;
    malformed.payload[4U] = std::byte{1U};
    failures += require(
        !decode_dm4_frame(malformed, info),
        "DM4 no-DTC zero fields are validated");

    malformed = frame;
    malformed.payload[7U] = std::byte{0U};
    failures += require(
        !decode_dm4_frame(malformed, info),
        "DM4 no-DTC FF padding is validated");
  }

  {
    const auto message = transported_dm4();
    DiagnosticFreezeFrameInfo info{};
    failures += require(
        decode_dm4_transport(message, info) &&
            info.source_address == 0x80U &&
            info.record_count == 2U &&
            info.transported &&
            !info.destination_specific &&
            !info.no_dtc,
        "transported DM4 record boundaries decode");

    DiagnosticFreezeFrameRecord first{};
    DiagnosticFreezeFrameRecord second{};
    failures += require(
        decode_dm4_record(
            message,
            0U,
            first) ==
                DiagnosticDecodeStatus::ok &&
            first.dtc.spn == 0x52345U &&
            first.dtc.fmi == 5U &&
            first.dtc.occurrence_count == 12U &&
            first.engine_torque_mode_raw == 0x03U &&
            first.boost_pressure_raw == 0x44U &&
            first.engine_speed_raw == 0x1234U &&
            first.engine_load_raw == 0x55U &&
            first.coolant_temperature_raw == 0x66U &&
            first.vehicle_speed_raw == 0x5678U &&
            first.manufacturer_data_length == 0U,
        "DM4 standard freeze-frame raw values decode");

    failures += require(
        decode_dm4_record(
            message,
            1U,
            second) ==
                DiagnosticDecodeStatus::ok &&
            second.dtc.spn == 0x10203U &&
            second.manufacturer_data_length == 2U &&
            message.data[
                second.manufacturer_data_offset] ==
                std::byte{0xA0U} &&
            message.data[
                second.manufacturer_data_offset + 1U] ==
                std::byte{0xA1U},
        "DM4 manufacturer-specific tail remains bounded and addressable");

    failures += require(
        decode_dm4_record(
            message,
            2U,
            second) ==
            DiagnosticDecodeStatus::invalid_argument,
        "DM4 record index is bounded");
  }

  {
    const auto message =
        transported_dm4(true);
    DiagnosticFreezeFrameInfo info{};
    failures += require(
        decode_dm4_transport(message, info) &&
            info.destination_specific &&
            info.destination_address == 0x90U,
        "destination-specific RTS/CTS DM4 response decodes");
  }

  {
    const auto message =
        transported_dm4(
            false,
            true);
    DiagnosticFreezeFrameRecord second{};
    failures += require(
        decode_dm4_record(
            message,
            1U,
            second) ==
                DiagnosticDecodeStatus::
                    unsupported_conversion_method &&
            second.dtc.spn == 0U &&
            second.dtc.conversion_method &&
            second.manufacturer_data_length == 2U,
        "DM4 legacy DTC conversion fails closed while preserving snapshot bounds");
  }

  {
    auto malformed = transported_dm4();
    DiagnosticFreezeFrameInfo info{};

    malformed.data[0U] = std::byte{11U};
    failures += require(
        !decode_dm4_transport(malformed, info),
        "DM4 record below standard minimum is rejected");

    malformed = transported_dm4();
    --malformed.size;
    failures += require(
        !decode_dm4_transport(malformed, info),
        "truncated DM4 record is rejected");

    malformed = transported_dm4();
    malformed.pgn = kDm5Pgn;
    failures += require(
        !decode_dm4_transport(malformed, info),
        "DM4 parser rejects unrelated PGN");

    malformed = transported_dm4(true);
    malformed.destination_address =
        malformed.source_address;
    failures += require(
        !decode_dm4_transport(malformed, info),
        "DM4 peer transport rejects self destination");
  }

  failures += require(
      sizeof(DiagnosticMessageInfo) <= 64U,
      "diagnostic message metadata remains small and bounded");
  failures += require(
      sizeof(DiagnosticFreezeFrameRecord) <= 64U,
      "freeze-frame record metadata remains small and bounded");

  if (failures == 0) {
    std::cout
        << "CORE_V2_J1939_DIAGNOSTICS_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
