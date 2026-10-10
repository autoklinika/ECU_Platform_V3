#include "ecu/platform/linux/runtime/netlink_can_link_query.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <linux/can/netlink.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ecu::platform::linux::runtime {
namespace {

using ecu::runtime::CanBusState;
using ecu::runtime::CanLinkQuery;
using ecu::runtime::LinkStatus;

// Iterates rtattr records in [data, data + length) without trusting lengths.
template <typename Visit>
void for_each_attribute(const unsigned char* data, std::size_t length,
                        Visit&& visit) {
  while (length >= sizeof(rtattr)) {
    rtattr header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.rta_len < sizeof(rtattr) || header.rta_len > length) return;
    visit(header.rta_type & NLA_TYPE_MASK, data + RTA_LENGTH(0),
          static_cast<std::size_t>(header.rta_len - RTA_LENGTH(0)));
    const std::size_t step = RTA_ALIGN(header.rta_len);
    if (step >= length) return;
    data += step;
    length -= step;
  }
}

template <typename T>
bool read_value(const unsigned char* data, std::size_t length, T& out) {
  if (length < sizeof(T)) return false;
  std::memcpy(&out, data, sizeof(T));
  return true;
}

CanBusState map_state(const std::uint32_t state) noexcept {
  switch (state) {
    case CAN_STATE_ERROR_ACTIVE: return CanBusState::error_active;
    case CAN_STATE_ERROR_WARNING: return CanBusState::error_warning;
    case CAN_STATE_ERROR_PASSIVE: return CanBusState::error_passive;
    case CAN_STATE_BUS_OFF: return CanBusState::bus_off;
    case CAN_STATE_STOPPED: return CanBusState::stopped;
    default: return CanBusState::unknown;
  }
}

class Socket {
 public:
  Socket() : fd_(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)) {}
  ~Socket() {
    if (fd_ >= 0) ::close(fd_);
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  [[nodiscard]] int fd() const noexcept { return fd_; }

 private:
  int fd_;
};

}  // namespace

CanLinkQuery parse_can_link_message(const void* message,
                                    const std::size_t length) noexcept {
  CanLinkQuery out{};
  out.status = LinkStatus::io_error;
  const auto* bytes = static_cast<const unsigned char*>(message);
  if (message == nullptr || length < NLMSG_LENGTH(sizeof(ifinfomsg))) return out;
  nlmsghdr header{};
  std::memcpy(&header, bytes, sizeof(header));
  if (header.nlmsg_type != RTM_NEWLINK || header.nlmsg_len > length ||
      header.nlmsg_len < NLMSG_LENGTH(sizeof(ifinfomsg)))
    return out;
  ifinfomsg info{};
  std::memcpy(&info, bytes + NLMSG_LENGTH(0), sizeof(info));

  const std::size_t attributes_offset = NLMSG_LENGTH(NLMSG_ALIGN(sizeof(ifinfomsg)));
  const std::size_t attributes_length =
      header.nlmsg_len > attributes_offset ? header.nlmsg_len - attributes_offset : 0U;

  bool is_can = false;
  bool has_bittiming = false;
  auto& state = out.state;
  state.present = true;
  state.up = (info.ifi_flags & IFF_UP) != 0U;

  for_each_attribute(bytes + attributes_offset, attributes_length,
                     [&](unsigned type, const unsigned char* data, std::size_t size) {
    if (type != IFLA_LINKINFO) return;
    for_each_attribute(data, size, [&](unsigned info_type, const unsigned char* info_data,
                                       std::size_t info_size) {
      if (info_type == IFLA_INFO_KIND) {
        is_can = info_size >= 3U && std::memcmp(info_data, "can", 3) == 0 &&
                 (info_size == 3U || info_data[3] == '\0');
      } else if (info_type == IFLA_INFO_DATA) {
        for_each_attribute(info_data, info_size, [&](unsigned can_type,
                                                     const unsigned char* can_data,
                                                     std::size_t can_size) {
          switch (can_type) {
            case IFLA_CAN_BITTIMING: {
              can_bittiming timing{};
              if (read_value(can_data, can_size, timing)) {
                state.bitrate = timing.bitrate;
                has_bittiming = true;
              }
              break;
            }
            case IFLA_CAN_CTRLMODE: {
              can_ctrlmode mode{};
              if (read_value(can_data, can_size, mode)) {
                state.listen_only = (mode.flags & CAN_CTRLMODE_LISTENONLY) != 0U;
                state.fd_enabled = (mode.flags & CAN_CTRLMODE_FD) != 0U;
              }
              break;
            }
            case IFLA_CAN_STATE: {
              std::uint32_t value = 0U;
              if (read_value(can_data, can_size, value)) state.bus_state = map_state(value);
              break;
            }
            case IFLA_CAN_BERR_COUNTER: {
              can_berr_counter counters{};
              if (read_value(can_data, can_size, counters)) {
                state.has_error_counters = true;
                state.tx_errors = counters.txerr;
                state.rx_errors = counters.rxerr;
              }
              break;
            }
            default:
              break;
          }
        });
      }
    });
  });

  if (!is_can) {
    out.status = LinkStatus::not_can;
    out.state = {};
    out.state.present = true;
    return out;
  }
  (void)has_bittiming;  // an unconfigured link legitimately reports bitrate 0
  state.bus_off = state.bus_state == CanBusState::bus_off;
  out.status = LinkStatus::ok;
  return out;
}

CanLinkQuery query_can_link(const std::string& interface_name,
                            const std::chrono::milliseconds timeout) noexcept {
  CanLinkQuery out{};
  if (interface_name.empty() || interface_name.size() >= IFNAMSIZ) {
    out.status = LinkStatus::not_found;
    return out;
  }
  Socket socket_holder;
  const int fd = socket_holder.fd();
  if (fd < 0) return out;

  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct Request {
    nlmsghdr header;
    ifinfomsg info;
    unsigned char attributes[RTA_SPACE(IFNAMSIZ)];
  } request{};
  constexpr std::uint32_t kSequence = 0x45435533U;  // "ECU3"
  const std::size_t name_size = interface_name.size() + 1U;
  request.header.nlmsg_type = RTM_GETLINK;
  request.header.nlmsg_flags = NLM_F_REQUEST;
  request.header.nlmsg_seq = kSequence;
  request.info.ifi_family = AF_UNSPEC;
  rtattr name{};
  name.rta_type = IFLA_IFNAME;
  name.rta_len = static_cast<unsigned short>(RTA_LENGTH(name_size));
  std::memcpy(request.attributes, &name, sizeof(name));
  std::memcpy(request.attributes + RTA_LENGTH(0), interface_name.c_str(), name_size);
  request.header.nlmsg_len = static_cast<std::uint32_t>(
      NLMSG_LENGTH(sizeof(ifinfomsg)) + RTA_SPACE(name_size));

  sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  if (::sendto(fd, &request, request.header.nlmsg_len, 0,
               reinterpret_cast<const sockaddr*>(&kernel), sizeof(kernel)) < 0) {
    out.status = errno == EAGAIN ? LinkStatus::timeout : LinkStatus::io_error;
    return out;
  }

  alignas(nlmsghdr) std::array<unsigned char, 16384> buffer{};
  for (int attempt = 0; attempt < 8; ++attempt) {
    const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      out.status = (errno == EAGAIN || errno == EWOULDBLOCK) ? LinkStatus::timeout
                                                             : LinkStatus::io_error;
      return out;
    }
    std::size_t remaining = static_cast<std::size_t>(received);
    const unsigned char* cursor = buffer.data();
    while (remaining >= sizeof(nlmsghdr)) {
      nlmsghdr header{};
      std::memcpy(&header, cursor, sizeof(header));
      if (header.nlmsg_len < sizeof(nlmsghdr) || header.nlmsg_len > remaining) break;
      if (header.nlmsg_seq == kSequence) {
        if (header.nlmsg_type == NLMSG_ERROR) {
          nlmsgerr error{};
          if (header.nlmsg_len >= NLMSG_LENGTH(sizeof(nlmsgerr)))
            std::memcpy(&error, cursor + NLMSG_LENGTH(0), sizeof(error));
          out.status = error.error == -ENODEV ? LinkStatus::not_found : LinkStatus::io_error;
          return out;
        }
        if (header.nlmsg_type == RTM_NEWLINK)
          return parse_can_link_message(cursor, header.nlmsg_len);
      }
      const std::size_t step = NLMSG_ALIGN(header.nlmsg_len);
      if (step >= remaining) break;
      cursor += step;
      remaining -= step;
    }
  }
  out.status = LinkStatus::io_error;
  return out;
}

}  // namespace ecu::platform::linux::runtime
