#include "ecu/platform/linux/v2/socketcan_adapter.hpp"

#include "ecu/platform/linux/v2/socketcan_link_info.hpp"

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ecu::platform::linux::v2 {
namespace {

namespace time = ecu::core::v2::time;
namespace transport = ecu::core::v2::transport;

class SocketCanProcessArbiter final
    : public transport::ICanChannelArbiter {
 public:
  [[nodiscard]] bool try_acquire(
      const transport::CanPhysicalChannelId channel,
      const void* const owner_token) noexcept override {
    if (!channel.valid() || owner_token == nullptr) {
      return false;
    }

    const std::size_t start =
        static_cast<std::size_t>(
            channel.value % kSlotCount);

    for (std::size_t offset = 0U;
         offset < kSlotCount;
         ++offset) {
      auto& slot =
          slots_[(start + offset) % kSlotCount];

      std::uint64_t slot_channel =
          slot.channel.load(std::memory_order_acquire);

      if (slot_channel == 0U) {
        std::uint64_t expected = 0U;
        if (slot.channel.compare_exchange_strong(
                expected,
                channel.value,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          slot_channel = channel.value;
        } else {
          slot_channel = expected;
        }
      }

      if (slot_channel != channel.value) {
        continue;
      }

      const void* expected_owner = nullptr;
      if (slot.owner.compare_exchange_strong(
              expected_owner,
              owner_token,
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return true;
      }

      return expected_owner == owner_token;
    }

    return false;
  }

  void release(
      const transport::CanPhysicalChannelId channel,
      const void* const owner_token) noexcept override {
    if (!channel.valid() || owner_token == nullptr) {
      return;
    }

    const std::size_t start =
        static_cast<std::size_t>(
            channel.value % kSlotCount);

    for (std::size_t offset = 0U;
         offset < kSlotCount;
         ++offset) {
      auto& slot =
          slots_[(start + offset) % kSlotCount];
      const auto slot_channel =
          slot.channel.load(std::memory_order_acquire);

      if (slot_channel == 0U) {
        return;
      }
      if (slot_channel != channel.value) {
        continue;
      }

      const void* expected_owner = owner_token;
      static_cast<void>(
          slot.owner.compare_exchange_strong(
              expected_owner,
              nullptr,
              std::memory_order_acq_rel,
              std::memory_order_acquire));
      return;
    }
  }

 private:
  static constexpr std::size_t kSlotCount = 64U;

  struct Slot {
    std::atomic<std::uint64_t> channel{0U};
    std::atomic<const void*> owner{nullptr};
  };

  std::array<Slot, kSlotCount> slots_{};
};

SocketCanProcessArbiter g_socketcan_arbiter{};

[[nodiscard]] transport::CanStatus map_write_error(
    const int error) noexcept {
  switch (error) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ENOBUFS:
      return transport::CanStatus::would_block;
    case EMSGSIZE:
    case EINVAL:
      return transport::CanStatus::invalid_argument;
    default:
      return transport::CanStatus::io_error;
  }
}

[[nodiscard]] transport::CanStatus map_link_query_status(
    const SocketCanLinkQueryStatus status) noexcept {
  switch (status) {
    case SocketCanLinkQueryStatus::ok:
      return transport::CanStatus::ok;
    case SocketCanLinkQueryStatus::not_found:
    case SocketCanLinkQueryStatus::not_can:
      return transport::CanStatus::unsupported;
    case SocketCanLinkQueryStatus::timeout:
    case SocketCanLinkQueryStatus::io_error:
      return transport::CanStatus::io_error;
  }
  return transport::CanStatus::io_error;
}

[[nodiscard]] bool profile_matches(
    const SocketCanLinkInfo& link,
    const transport::CanChannelConfig& config) noexcept {
  if (!link.up ||
      link.nominal_bitrate != config.nominal_bitrate) {
    return false;
  }

  if (config.fd_enabled &&
      (!link.fd_enabled ||
       link.data_bitrate != config.data_bitrate)) {
    return false;
  }

  const bool wants_listen_only =
      config.mode == transport::CanMode::listen_only;
  return link.listen_only_enabled == wants_listen_only;
}

[[nodiscard]] canid_t encode_identifier(
    const transport::CanFrame& source) noexcept {
  canid_t identifier = source.identifier;

  if (source.identifier_format ==
      transport::CanIdentifierFormat::extended_29_bit) {
    identifier |= CAN_EFF_FLAG;
  }
  if (source.type ==
      transport::CanFrameType::remote) {
    identifier |= CAN_RTR_FLAG;
  }

  return identifier;
}

void decode_identifier(
    const canid_t source,
    transport::CanFrame& target) noexcept {
  target.identifier_format =
      (source & CAN_EFF_FLAG) != 0U
          ? transport::CanIdentifierFormat::extended_29_bit
          : transport::CanIdentifierFormat::standard_11_bit;

  target.identifier =
      source &
      (target.identifier_format ==
               transport::CanIdentifierFormat::extended_29_bit
           ? CAN_EFF_MASK
           : CAN_SFF_MASK);

  target.type =
      (source & CAN_RTR_FLAG) != 0U
          ? transport::CanFrameType::remote
          : transport::CanFrameType::data;
}

[[nodiscard]] transport::CanStatus encode_classic(
    const transport::CanFrame& source,
    can_frame& target) noexcept {
  if (!transport::is_valid_can_frame(source) ||
      source.format !=
          transport::CanFrameFormat::classic) {
    return transport::CanStatus::invalid_frame;
  }

  target = {};
  target.can_id = encode_identifier(source);
  target.len = source.length;

  if (source.type == transport::CanFrameType::data &&
      source.length != 0U) {
    std::memcpy(
        target.data,
        source.payload.data(),
        source.length);
  }

  return transport::CanStatus::ok;
}

[[nodiscard]] transport::CanStatus encode_fd(
    const transport::CanFrame& source,
    canfd_frame& target) noexcept {
  if (!transport::is_valid_can_frame(source) ||
      source.format != transport::CanFrameFormat::fd) {
    return transport::CanStatus::invalid_frame;
  }

  target = {};
  target.can_id = encode_identifier(source);
  target.len = source.length;

  if (source.bit_rate_switch) {
    target.flags |= CANFD_BRS;
  }
  if (source.error_state_indicator) {
    target.flags |= CANFD_ESI;
  }

  if (source.length != 0U) {
    std::memcpy(
        target.data,
        source.payload.data(),
        source.length);
  }

  return transport::CanStatus::ok;
}

[[nodiscard]] transport::CanStatus decode_classic(
    const can_frame& source,
    const time::MonotonicClockReading& timestamp,
    transport::CanReceiveResult& target) noexcept {
  if ((source.can_id & CAN_ERR_FLAG) != 0U ||
      source.len > CAN_MAX_DLEN) {
    return transport::CanStatus::invalid_frame;
  }

  target.status = transport::CanStatus::ok;
  target.value.timestamp = timestamp;
  target.value.frame = {};
  target.value.frame.format =
      transport::CanFrameFormat::classic;
  decode_identifier(
      source.can_id,
      target.value.frame);
  target.value.frame.length = source.len;

  if (target.value.frame.type ==
          transport::CanFrameType::data &&
      source.len != 0U) {
    std::memcpy(
        target.value.frame.payload.data(),
        source.data,
        source.len);
  }

  return transport::is_valid_can_frame(
             target.value.frame)
             ? transport::CanStatus::ok
             : transport::CanStatus::invalid_frame;
}

[[nodiscard]] transport::CanStatus decode_fd(
    const canfd_frame& source,
    const time::MonotonicClockReading& timestamp,
    transport::CanReceiveResult& target) noexcept {
  if ((source.can_id & CAN_ERR_FLAG) != 0U ||
      source.len > CANFD_MAX_DLEN) {
    return transport::CanStatus::invalid_frame;
  }

  target.status = transport::CanStatus::ok;
  target.value.timestamp = timestamp;
  target.value.frame = {};
  target.value.frame.format =
      transport::CanFrameFormat::fd;
  decode_identifier(
      source.can_id,
      target.value.frame);

  if (target.value.frame.type ==
      transport::CanFrameType::remote) {
    return transport::CanStatus::invalid_frame;
  }

  target.value.frame.length = source.len;
  target.value.frame.bit_rate_switch =
      (source.flags & CANFD_BRS) != 0U;
  target.value.frame.error_state_indicator =
      (source.flags & CANFD_ESI) != 0U;

  if (source.len != 0U) {
    std::memcpy(
        target.value.frame.payload.data(),
        source.data,
        source.len);
  }

  return transport::is_valid_can_frame(
             target.value.frame)
             ? transport::CanStatus::ok
             : transport::CanStatus::invalid_frame;
}

[[nodiscard]] std::uint32_t drop_delta(
    const std::uint32_t current,
    const std::uint32_t previous) noexcept {
  if (current >= previous) {
    return current - previous;
  }

  return
      (std::numeric_limits<std::uint32_t>::max)() -
      previous +
      current +
      1U;
}

}  // namespace

SocketCanAdapter::SocketCanAdapter(
    const char* const interface_name,
    const time::IMonotonicClock& clock,
    const transport::CanDriverExecutionContract
        execution_contract) noexcept
    : clock_(clock),
      clock_properties_(clock.properties()),
      execution_(execution_contract) {
  if (interface_name == nullptr ||
      interface_name[0] == '\0' ||
      std::strlen(interface_name) >=
          interface_name_.size() ||
      !time::is_valid_clock_properties(
          clock_properties_) ||
      !transport::is_valid_can_driver_execution_contract(
          execution_) ||
      clock_properties_.max_uncertainty >
          execution_.max_rx_timestamp_uncertainty) {
    return;
  }

  std::memcpy(
      interface_name_.data(),
      interface_name,
      std::strlen(interface_name) + 1U);

  const unsigned int interface_index =
      if_nametoindex(interface_name_.data());
  if (interface_index == 0U) {
    return;
  }

  const auto query =
      query_socketcan_link(interface_name_.data());
  if (query.status !=
      SocketCanLinkQueryStatus::ok) {
    return;
  }

  channel_id_.value =
      static_cast<std::uint64_t>(interface_index);
  capabilities_ = query.info.capabilities;
  valid_ =
      channel_id_.valid() &&
      capabilities_.classic_can;
}

SocketCanAdapter::~SocketCanAdapter() {
  close();
}

bool SocketCanAdapter::valid() const noexcept {
  return valid_;
}

transport::CanPhysicalChannelId
SocketCanAdapter::physical_channel_id() const noexcept {
  return channel_id_;
}

transport::ICanChannelArbiter&
SocketCanAdapter::channel_arbiter() noexcept {
  return g_socketcan_arbiter;
}

transport::CanDriverExecutionContract
SocketCanAdapter::execution_contract() const noexcept {
  return execution_;
}

transport::CanCapabilities
SocketCanAdapter::capabilities() const noexcept {
  return valid_
             ? capabilities_
             : transport::CanCapabilities{};
}

transport::CanStatus SocketCanAdapter::open(
    const transport::CanChannelConfig& config) noexcept {
  if (!valid_) {
    return transport::CanStatus::invalid_state;
  }
  if (is_open()) {
    return transport::CanStatus::already_open;
  }
  if (!transport::is_valid_can_channel_config(config) ||
      config.timestamp_domain !=
          clock_properties_.domain) {
    return transport::CanStatus::invalid_argument;
  }

  const auto query =
      query_socketcan_link(interface_name_.data());
  if (query.status !=
      SocketCanLinkQueryStatus::ok) {
    return map_link_query_status(query.status);
  }
  if (query.info.bus_off) {
    return transport::CanStatus::bus_off;
  }
  if (!transport::capabilities_support(
          query.info.capabilities,
          config)) {
    return transport::CanStatus::unsupported;
  }
  if (!profile_matches(query.info, config)) {
    return transport::CanStatus::unsupported;
  }

  const int fd = ::socket(
      PF_CAN,
      SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
      CAN_RAW);
  if (fd < 0) {
    return transport::CanStatus::io_error;
  }

  const int receive_own_messages = 0;
  if (::setsockopt(
          fd,
          SOL_CAN_RAW,
          CAN_RAW_RECV_OWN_MSGS,
          &receive_own_messages,
          sizeof(receive_own_messages)) < 0) {
    ::close(fd);
    return transport::CanStatus::io_error;
  }

  const can_err_mask_t error_mask = CAN_ERR_BUSOFF;
  if (::setsockopt(
          fd,
          SOL_CAN_RAW,
          CAN_RAW_ERR_FILTER,
          &error_mask,
          sizeof(error_mask)) < 0) {
    ::close(fd);
    return transport::CanStatus::io_error;
  }

  const int receive_overflow = 1;
  if (::setsockopt(
          fd,
          SOL_SOCKET,
          SO_RXQ_OVFL,
          &receive_overflow,
          sizeof(receive_overflow)) < 0) {
    ::close(fd);
    return transport::CanStatus::io_error;
  }

  if (config.fd_enabled) {
    const int enable_fd = 1;
    if (::setsockopt(
            fd,
            SOL_CAN_RAW,
            CAN_RAW_FD_FRAMES,
            &enable_fd,
            sizeof(enable_fd)) < 0) {
      ::close(fd);
      return transport::CanStatus::unsupported;
    }
  }

  sockaddr_can address{};
  address.can_family = AF_CAN;
  address.can_ifindex =
      static_cast<int>(channel_id_.value);

  if (::bind(
          fd,
          reinterpret_cast<const sockaddr*>(&address),
          sizeof(address)) < 0) {
    ::close(fd);
    return transport::CanStatus::io_error;
  }

  socket_fd_ = fd;
  active_capabilities_ = query.info.capabilities;
  active_config_ = config;
  rx_drop_total_ = 0U;
  return transport::CanStatus::ok;
}

void SocketCanAdapter::close() noexcept {
  if (socket_fd_ >= 0) {
    ::close(socket_fd_);
    socket_fd_ = -1;
  }

  active_capabilities_ = {};
  active_config_ = {};
  rx_drop_total_ = 0U;
}

bool SocketCanAdapter::is_open() const noexcept {
  return socket_fd_ >= 0;
}

transport::CanStatus SocketCanAdapter::try_send(
    const transport::CanFrame& frame) noexcept {
  if (!is_open()) {
    return transport::CanStatus::not_open;
  }
  if (active_config_.mode ==
      transport::CanMode::listen_only) {
    return transport::CanStatus::unsupported;
  }
  if (!transport::is_valid_can_frame(frame)) {
    return transport::CanStatus::invalid_frame;
  }
  if (!transport::capabilities_support_frame(
          active_capabilities_,
          frame)) {
    return transport::CanStatus::unsupported;
  }

  transport::CanStatus status =
      transport::CanStatus::invalid_frame;

  if (frame.format ==
      transport::CanFrameFormat::fd) {
    if (!active_config_.fd_enabled) {
      return transport::CanStatus::unsupported;
    }

    canfd_frame encoded{};
    status = encode_fd(frame, encoded);
    if (status == transport::CanStatus::ok) {
      const ssize_t written =
          ::write(socket_fd_, &encoded, CANFD_MTU);
      status =
          written == CANFD_MTU
              ? transport::CanStatus::ok
              : (written < 0
                     ? map_write_error(errno)
                     : transport::CanStatus::io_error);
    }
  } else {
    can_frame encoded{};
    status = encode_classic(frame, encoded);
    if (status == transport::CanStatus::ok) {
      const ssize_t written =
          ::write(socket_fd_, &encoded, CAN_MTU);
      status =
          written == CAN_MTU
              ? transport::CanStatus::ok
              : (written < 0
                     ? map_write_error(errno)
                     : transport::CanStatus::io_error);
    }
  }

  if (status == transport::CanStatus::io_error ||
      status == transport::CanStatus::bus_off) {
    close();
  }

  return status;
}

transport::CanReceiveResult
SocketCanAdapter::try_receive() noexcept {
  transport::CanReceiveResult result{};

  if (!is_open()) {
    result.status = transport::CanStatus::not_open;
    return result;
  }

  canfd_frame frame{};
  iovec vector{};
  vector.iov_base = &frame;
  vector.iov_len = sizeof(frame);

  alignas(cmsghdr)
      std::array<std::byte, CMSG_SPACE(sizeof(std::uint32_t))>
          control{};

  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  message.msg_control = control.data();
  message.msg_controllen = control.size();

  const ssize_t received =
      ::recvmsg(socket_fd_, &message, 0);

  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      result.status =
          transport::CanStatus::would_block;
      return result;
    }

    result.status = transport::CanStatus::io_error;
    close();
    return result;
  }

  std::uint32_t current_drop_total = rx_drop_total_;
  for (auto* header = CMSG_FIRSTHDR(&message);
       header != nullptr;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == SOL_SOCKET &&
        header->cmsg_type == SO_RXQ_OVFL &&
        header->cmsg_len >=
            CMSG_LEN(sizeof(std::uint32_t))) {
      std::memcpy(
          &current_drop_total,
          CMSG_DATA(header),
          sizeof(current_drop_total));
    }
  }

  result.dropped_frames_since_last_receive =
      drop_delta(
          current_drop_total,
          rx_drop_total_);
  rx_drop_total_ = current_drop_total;

  const auto timestamp = clock_.read();
  if (!time::is_valid_clock_reading(
          timestamp,
          active_config_.timestamp_domain) ||
      timestamp.uncertainty >
          execution_.max_rx_timestamp_uncertainty) {
    result.status =
        transport::CanStatus::invalid_timestamp;
    close();
    return result;
  }

  if (received == CAN_MTU) {
    const auto* classic =
        reinterpret_cast<const can_frame*>(&frame);

    if ((classic->can_id & CAN_ERR_FLAG) != 0U) {
      result.status =
          (classic->can_id & CAN_ERR_BUSOFF) != 0U
              ? transport::CanStatus::bus_off
              : transport::CanStatus::io_error;
      close();
      return result;
    }

    result.status =
        decode_classic(
            *classic,
            timestamp,
            result);
    return result;
  }

  if (received == CANFD_MTU) {
    if (!active_config_.fd_enabled) {
      result.status =
          transport::CanStatus::invalid_frame;
      return result;
    }

    result.status =
        decode_fd(
            frame,
            timestamp,
            result);
    return result;
  }

  result.status = transport::CanStatus::io_error;
  close();
  return result;
}

}  // namespace ecu::platform::linux::v2
