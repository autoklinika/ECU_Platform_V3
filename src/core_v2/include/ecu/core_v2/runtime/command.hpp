#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

using CommandId = std::uint64_t;
using CorrelationId = std::uint64_t;
using CommandTypeId = std::uint32_t;

enum class CommandPriority : std::uint8_t {
  normal,
  high,
  safety_critical,
};

enum class CommandOutcome : std::uint8_t {
  accepted,
  rejected,
  running,
  succeeded,
  failed,
  cancelled,
};

struct CommandHeader {
  CommandId id{0U};
  CommandTypeId type{0U};
  CorrelationId correlation_id{0U};
  CommandPriority priority{CommandPriority::normal};
  time::MonotonicTime issued_at{0};
};

struct CommandView {
  CommandHeader header{};
  const std::byte* payload{nullptr};
  std::size_t payload_size{0U};
};

struct CommandResult {
  CommandOutcome outcome{CommandOutcome::rejected};
  std::uint32_t error_code{0U};
};

[[nodiscard]] constexpr bool is_valid_command_header(
    const CommandHeader& header) noexcept {
  return header.id != 0U &&
         header.type != 0U &&
         header.issued_at.count() >= 0;
}

[[nodiscard]] constexpr bool is_valid_command_view(
    const CommandView& command) noexcept {
  return is_valid_command_header(command.header) &&
         (command.payload_size == 0U ||
          command.payload != nullptr);
}

}  // namespace ecu::core::v2::runtime
