#include "ecu/platform/linux/v2/socketcan_link_info.hpp"

#include <linux/can/netlink.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace ecu::platform::linux::v2 {
namespace {

bool parse_can_data(
    rtattr* data,
    int data_length,
    SocketCanLinkInfo& info) noexcept {
  bool found_bittiming_const = false;
  bool found_data_bittiming_const = false;
  std::uint32_t ctrlmode_supported = 0U;

  for (auto* attr = data;
       RTA_OK(attr, data_length);
       attr = RTA_NEXT(attr, data_length)) {
    const auto type = attr->rta_type & NLA_TYPE_MASK;
    switch (type) {
      case IFLA_CAN_BITTIMING:
        if (RTA_PAYLOAD(attr) >= sizeof(can_bittiming)) {
          const auto* value =
              static_cast<const can_bittiming*>(
                  RTA_DATA(attr));
          info.nominal_bitrate = value->bitrate;
        }
        break;

      case IFLA_CAN_BITTIMING_CONST:
        found_bittiming_const = true;
        break;

      case IFLA_CAN_DATA_BITTIMING:
        if (RTA_PAYLOAD(attr) >= sizeof(can_bittiming)) {
          const auto* value =
              static_cast<const can_bittiming*>(
                  RTA_DATA(attr));
          info.data_bitrate = value->bitrate;
        }
        break;

      case IFLA_CAN_DATA_BITTIMING_CONST:
        found_data_bittiming_const = true;
        break;

      case IFLA_CAN_CTRLMODE:
        if (RTA_PAYLOAD(attr) >= sizeof(can_ctrlmode)) {
          const auto* value =
              static_cast<const can_ctrlmode*>(
                  RTA_DATA(attr));
          info.fd_enabled =
              (value->flags & CAN_CTRLMODE_FD) != 0U;
          info.listen_only_enabled =
              (value->flags & CAN_CTRLMODE_LISTENONLY) !=
              0U;
          ctrlmode_supported |= value->mask;
        }
        break;

      case IFLA_CAN_STATE:
        if (RTA_PAYLOAD(attr) >= sizeof(std::uint32_t)) {
          std::uint32_t state = 0U;
          std::memcpy(
              &state,
              RTA_DATA(attr),
              sizeof(state));
          info.bus_off = state == CAN_STATE_BUS_OFF;
        }
        break;

      case IFLA_CAN_CTRLMODE_EXT: {
        int nested_length = RTA_PAYLOAD(attr);
        auto* nested =
            static_cast<rtattr*>(RTA_DATA(attr));
        for (; RTA_OK(nested, nested_length);
             nested = RTA_NEXT(nested, nested_length)) {
          const auto nested_type =
              nested->rta_type & NLA_TYPE_MASK;
          if (nested_type ==
                  IFLA_CAN_CTRLMODE_SUPPORTED &&
              RTA_PAYLOAD(nested) >=
                  sizeof(std::uint32_t)) {
            std::uint32_t supported = 0U;
            std::memcpy(
                &supported,
                RTA_DATA(nested),
                sizeof(supported));
            ctrlmode_supported |= supported;
          }
        }
        break;
      }

      default:
        break;
    }
  }

  info.capabilities.classic_can =
      found_bittiming_const;
  info.capabilities.can_fd =
      found_data_bittiming_const ||
      (ctrlmode_supported & CAN_CTRLMODE_FD) != 0U;
  info.capabilities.bit_rate_switch =
      found_data_bittiming_const;
  info.capabilities.listen_only =
      (ctrlmode_supported & CAN_CTRLMODE_LISTENONLY) !=
      0U;
  info.capabilities.max_payload_bytes =
      info.capabilities.can_fd ? 64U : 8U;

  return found_bittiming_const;
}

bool parse_link_info(
    rtattr* link_info,
    int link_info_length,
    SocketCanLinkInfo& info) noexcept {
  bool is_can = false;
  rtattr* can_data = nullptr;
  int can_data_length = 0;

  for (auto* attr = link_info;
       RTA_OK(attr, link_info_length);
       attr = RTA_NEXT(attr, link_info_length)) {
    const auto type = attr->rta_type & NLA_TYPE_MASK;
    if (type == IFLA_INFO_KIND) {
      const auto* kind =
          static_cast<const char*>(RTA_DATA(attr));
      is_can = std::strcmp(kind, "can") == 0;
    } else if (type == IFLA_INFO_DATA) {
      can_data =
          static_cast<rtattr*>(RTA_DATA(attr));
      can_data_length = RTA_PAYLOAD(attr);
    }
  }

  if (!is_can || can_data == nullptr) {
    return false;
  }

  return parse_can_data(
      can_data,
      can_data_length,
      info);
}

SocketCanLinkQueryResult parse_link_message(
    nlmsghdr* message,
    const unsigned int interface_index) noexcept {
  SocketCanLinkQueryResult result{};

  if (message->nlmsg_type != RTM_NEWLINK) {
    return result;
  }

  auto* ifinfo =
      static_cast<ifinfomsg*>(NLMSG_DATA(message));
  if (static_cast<unsigned int>(
          ifinfo->ifi_index) != interface_index) {
    return result;
  }

  result.info.up =
      (ifinfo->ifi_flags & IFF_UP) != 0U;

  int length =
      static_cast<int>(message->nlmsg_len) -
      static_cast<int>(
          NLMSG_LENGTH(sizeof(*ifinfo)));
  auto* attr = IFLA_RTA(ifinfo);

  for (; RTA_OK(attr, length);
       attr = RTA_NEXT(attr, length)) {
    const auto type = attr->rta_type & NLA_TYPE_MASK;
    if (type != IFLA_LINKINFO) {
      continue;
    }

    int nested_length = RTA_PAYLOAD(attr);
    auto* nested =
        static_cast<rtattr*>(RTA_DATA(attr));

    result.status =
        parse_link_info(
            nested,
            nested_length,
            result.info)
            ? SocketCanLinkQueryStatus::ok
            : SocketCanLinkQueryStatus::not_can;
    return result;
  }

  result.status = SocketCanLinkQueryStatus::not_can;
  return result;
}

}  // namespace

SocketCanLinkQueryResult query_socketcan_link(
    const char* interface_name,
    const int timeout_ms) noexcept {
  if (interface_name == nullptr ||
      interface_name[0] == '\0' ||
      timeout_ms <= 0) {
    return {
        SocketCanLinkQueryStatus::not_found,
        {}};
  }

  const unsigned int interface_index =
      if_nametoindex(interface_name);
  if (interface_index == 0U) {
    return {
        SocketCanLinkQueryStatus::not_found,
        {}};
  }

  const int fd = ::socket(
      AF_NETLINK,
      SOCK_RAW | SOCK_CLOEXEC,
      NETLINK_ROUTE);
  if (fd < 0) {
    return {
        SocketCanLinkQueryStatus::io_error,
        {}};
  }

  sockaddr_nl local{};
  local.nl_family = AF_NETLINK;
  if (::bind(
          fd,
          reinterpret_cast<const sockaddr*>(&local),
          sizeof(local)) < 0) {
    ::close(fd);
    return {
        SocketCanLinkQueryStatus::io_error,
        {}};
  }

  struct Request {
    nlmsghdr header;
    ifinfomsg info;
  } request{};

  request.header.nlmsg_len =
      NLMSG_LENGTH(sizeof(ifinfomsg));
  request.header.nlmsg_type = RTM_GETLINK;
  request.header.nlmsg_flags = NLM_F_REQUEST;
  request.header.nlmsg_seq = 1U;
  request.info.ifi_family = AF_UNSPEC;
  request.info.ifi_index =
      static_cast<int>(interface_index);

  sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  if (::sendto(
          fd,
          &request,
          request.header.nlmsg_len,
          0,
          reinterpret_cast<const sockaddr*>(&kernel),
          sizeof(kernel)) < 0) {
    ::close(fd);
    return {
        SocketCanLinkQueryStatus::io_error,
        {}};
  }

  pollfd descriptor{};
  descriptor.fd = fd;
  descriptor.events = POLLIN;
  const int ready =
      ::poll(&descriptor, 1, timeout_ms);
  if (ready == 0) {
    ::close(fd);
    return {
        SocketCanLinkQueryStatus::timeout,
        {}};
  }
  if (ready < 0 ||
      (descriptor.revents &
       (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    ::close(fd);
    return {
        SocketCanLinkQueryStatus::io_error,
        {}};
  }

  alignas(nlmsghdr) char buffer[8192]{};
  const ssize_t received =
      ::recv(fd, buffer, sizeof(buffer), 0);
  ::close(fd);

  if (received <= 0) {
    return {
        SocketCanLinkQueryStatus::io_error,
        {}};
  }

  int remaining = static_cast<int>(received);
  for (auto* message =
           reinterpret_cast<nlmsghdr*>(buffer);
       remaining >= static_cast<int>(sizeof(nlmsghdr)) &&
       NLMSG_OK(message, static_cast<unsigned int>(remaining));
       message = NLMSG_NEXT(message, remaining)) {
    if (message->nlmsg_type == NLMSG_ERROR) {
      return {
          SocketCanLinkQueryStatus::io_error,
          {}};
    }

    const auto parsed =
        parse_link_message(
            message,
            interface_index);
    if (parsed.status !=
        SocketCanLinkQueryStatus::io_error) {
      return parsed;
    }
  }

  return {
      SocketCanLinkQueryStatus::not_found,
      {}};
}

}  // namespace ecu::platform::linux::v2
