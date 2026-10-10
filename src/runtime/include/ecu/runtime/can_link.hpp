#pragma once

// Portable contracts for owning one physical CAN link.
//
// Platform code (SocketCAN, J2534, a simulator) implements these interfaces.
// Nothing here depends on an operating system. The link's UP/DOWN state is
// NEVER used as an ownership mutex: ownership is IExclusiveLock only.

#include <cstdint>

namespace ecu::runtime {

enum class CanLinkMode : std::uint8_t {
  normal,       // may transmit (ACK, UDS, J1939)
  listen_only,  // receive only; never ACKs or transmits on the bus
};

struct CanLinkConfig {
  std::uint32_t bitrate{0U};
  CanLinkMode mode{CanLinkMode::listen_only};
};

[[nodiscard]] constexpr bool operator==(const CanLinkConfig& a,
                                        const CanLinkConfig& b) noexcept {
  return a.bitrate == b.bitrate && a.mode == b.mode;
}

// Classic CAN bitrates accepted by the bench. Anything else is a config error.
[[nodiscard]] constexpr bool is_supported_bitrate(
    const std::uint32_t bitrate) noexcept {
  return bitrate == 125000U || bitrate == 250000U || bitrate == 500000U ||
         bitrate == 1000000U;
}

// Controller error state as defined by ISO 11898-1 fault confinement.
enum class CanBusState : std::uint8_t {
  unknown,
  error_active,
  error_warning,
  error_passive,
  bus_off,
  stopped,  // controller not running (link down)
};

struct CanLinkState {
  bool present{false};
  bool up{false};
  bool bus_off{false};
  bool fd_enabled{false};
  bool listen_only{false};
  std::uint32_t bitrate{0U};
  CanBusState bus_state{CanBusState::unknown};
  // Transmit/receive error counters; valid only if has_error_counters.
  bool has_error_counters{false};
  std::uint16_t tx_errors{0U};
  std::uint16_t rx_errors{0U};
};

enum class LinkStatus : std::uint8_t {
  ok,
  not_found,        // interface does not exist
  not_can,          // interface exists but is not a CAN controller
  command_failed,   // platform refused the change
  timeout,          // platform did not answer within its bound
  io_error,
};

struct CanLinkQuery {
  LinkStatus status{LinkStatus::io_error};
  CanLinkState state{};
};

// Privileged link configuration. Every call is bounded in time by the
// implementation. configure() is only called while the link is down.
class ICanLinkControl {
 public:
  virtual ~ICanLinkControl() = default;
  [[nodiscard]] virtual LinkStatus set_down() noexcept = 0;
  [[nodiscard]] virtual LinkStatus configure(
      const CanLinkConfig& config) noexcept = 0;
  [[nodiscard]] virtual LinkStatus set_up() noexcept = 0;
  [[nodiscard]] virtual CanLinkQuery query() noexcept = 0;
};

enum class LockStatus : std::uint8_t {
  acquired,
  busy,   // another owner holds the link
  error,  // lock could not be evaluated (permissions, missing directory)
};

// System-wide exclusive ownership of one physical link. Implementations must
// release automatically if the owning process dies (e.g. flock on Linux).
class IExclusiveLock {
 public:
  virtual ~IExclusiveLock() = default;
  [[nodiscard]] virtual LockStatus try_acquire() noexcept = 0;
  virtual void release() noexcept = 0;
  [[nodiscard]] virtual bool held() const noexcept = 0;
};

}  // namespace ecu::runtime
