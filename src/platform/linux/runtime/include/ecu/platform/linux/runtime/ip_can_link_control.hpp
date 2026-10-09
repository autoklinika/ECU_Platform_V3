#pragma once

#include "ecu/runtime/can_link.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace ecu::platform::linux::runtime {

// Configures a SocketCAN link with iproute2, using exactly the command lines
// already proven on the CM5 by ECU Platform V2:
//
//   ip link set <if> down
//   ip link set <if> type can bitrate <N> fd off listen-only on|off
//   ip link set <if> up
//
// The program is spawned directly (no shell), with an empty environment,
// output discarded, and killed if it exceeds the timeout. It runs only when
// the link configuration changes, never per measurement.
//
// State is read back over netlink (query_socketcan_link from Linux V2).
class IpCanLinkControl final : public ecu::runtime::ICanLinkControl {
 public:
  using QueryFunction = std::function<ecu::runtime::CanLinkQuery()>;

  IpCanLinkControl(std::string interface_name,
                   std::string ip_program = "/usr/sbin/ip",
                   std::chrono::milliseconds timeout =
                       std::chrono::milliseconds{3000});

  // Test seam: replaces the netlink state query.
  void set_query_function(QueryFunction query) { query_ = std::move(query); }

  [[nodiscard]] ecu::runtime::LinkStatus set_down() noexcept override;
  [[nodiscard]] ecu::runtime::LinkStatus configure(
      const ecu::runtime::CanLinkConfig& config) noexcept override;
  [[nodiscard]] ecu::runtime::LinkStatus set_up() noexcept override;
  [[nodiscard]] ecu::runtime::CanLinkQuery query() noexcept override;

  [[nodiscard]] const std::string& interface_name() const noexcept {
    return interface_;
  }

 private:
  [[nodiscard]] ecu::runtime::LinkStatus run(
      const std::vector<std::string>& args) noexcept;

  std::string interface_;
  std::string ip_;
  std::chrono::milliseconds timeout_;
  QueryFunction query_;
};

// Runs `program args...` without a shell. Returns ok on exit status 0,
// command_failed on any other exit, timeout if killed after `timeout`.
[[nodiscard]] ecu::runtime::LinkStatus run_bounded(
    const std::string& program, const std::vector<std::string>& args,
    std::chrono::milliseconds timeout) noexcept;

// Interface names accepted by the bench (Linux IFNAMSIZ, safe characters).
[[nodiscard]] bool valid_interface_name(const std::string& name) noexcept;

}  // namespace ecu::platform::linux::runtime
