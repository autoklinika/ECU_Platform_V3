#pragma once

#include "ecu/runtime/can_link.hpp"

#include <chrono>
#include <cstddef>
#include <string>

namespace ecu::platform::linux::runtime {

// Reads the state of one SocketCAN interface over rtnetlink (RTM_GETLINK):
// administrative UP flag, nominal bitrate, listen-only/FD control modes,
// controller error state and, if the driver provides them, error counters.
// Read-only: needs no privileges and never changes the link.
[[nodiscard]] ecu::runtime::CanLinkQuery query_can_link(
    const std::string& interface_name,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{200}) noexcept;

// Parses one RTM_NEWLINK message (nlmsghdr included). Exposed for tests;
// tolerant of missing optional attributes, strict about lengths.
[[nodiscard]] ecu::runtime::CanLinkQuery parse_can_link_message(
    const void* message, std::size_t length) noexcept;

}  // namespace ecu::platform::linux::runtime
