#pragma once

#include "ecu/core_v2/transport/can_types.hpp"

namespace ecu::core::v2::transport {

class ICanChannelArbiter {
 public:
  virtual ~ICanChannelArbiter() = default;

  // One authoritative arbiter covers every adapter object that can address the
  // same physical channel. Acquisition/release are atomic, non-blocking,
  // allocation-free, never call back into CanBusRuntime and complete within
  // the driver's declared lease acquire/release duration bounds.
  [[nodiscard]] virtual bool try_acquire(
      CanPhysicalChannelId channel,
      const void* owner_token) noexcept = 0;
  virtual void release(
      CanPhysicalChannelId channel,
      const void* owner_token) noexcept = 0;
};

class ICanDriver {
 public:
  virtual ~ICanDriver() = default;

  // The identifier is stable for this adapter object's lifetime and identifies
  // the physical CAN channel, not the wrapper object. Separate wrappers for the
  // same physical channel return the same ID and the same authoritative
  // channel_arbiter().
  [[nodiscard]] virtual CanPhysicalChannelId physical_channel_id()
      const noexcept = 0;
  [[nodiscard]] virtual ICanChannelArbiter& channel_arbiter() noexcept = 0;

  // Metadata queries are stable while closed/running, non-blocking, bounded,
  // allocation-free and never call back into CanBusRuntime.
  [[nodiscard]] virtual CanDriverExecutionContract execution_contract()
      const noexcept = 0;
  [[nodiscard]] virtual CanCapabilities capabilities() const noexcept = 0;

  // Lifecycle calls are bounded by execution_contract(). They never call back
  // into CanBusRuntime. open() starts closed. ok acquires hardware resources;
  // every non-ok result leaves the driver closed, drops partial resources and
  // accepts no TX.
  [[nodiscard]] virtual CanStatus open(
      const CanChannelConfig& config) noexcept = 0;

  // close() is idempotent and bounded. Before returning it prevents any pending
  // accepted TX from being transmitted later and clears the adapter's RX/TX
  // queues for the closed session.
  virtual void close() noexcept = 0;
  [[nodiscard]] virtual bool is_open() const noexcept = 0;

  // Runtime-path contract for try_send()/try_receive():
  // - never sleep, wait for external events, wait on a blocking lock or perform
  //   an unbounded retry;
  // - never allocate dynamically after open();
  // - complete within the declared execution bound;
  // - never call back into CanBusRuntime.
  //
  // try_send(): ok means the frame was accepted exactly once into a bounded,
  // FIFO submission path. would_block means the bounded TX path is full and
  // the frame was not accepted. Every other non-ok result also accepts nothing.
  // A fatal bus_off/io_error/not_open result quarantines or discards pending TX
  // so no accepted frame can be transmitted after the fatal result without a
  // new successful open().
  [[nodiscard]] virtual CanStatus try_send(
      const CanFrame& frame) noexcept = 0;

  // try_receive(): ok returns exactly one frame. would_block means no frame is
  // available immediately. dropped_frames_since_last_receive reports RX loss
  // observed by the adapter even when status is would_block. Every ok frame has
  // a healthy timestamp in the configured timestamp clock domain, captured at
  // the earliest stable adapter/hardware RX boundary. Native device ticks must
  // be converted by the adapter before entering Core.
  //
  // Protocol ingress MUST NOT contain an echo of a frame accepted by this same
  // driver instance through try_send(). Core RX records intentionally carry no
  // local/remote-origin tag, so an own-TX echo would be indistinguishable from
  // another ECU and can corrupt arbitration protocols such as J1939 Address
  // Claiming. A platform that supports local loopback may expose analyzer/trace
  // copies outside the authoritative protocol ingress, but it must suppress
  // own-message echo from try_receive().
  [[nodiscard]] virtual CanReceiveResult try_receive() noexcept = 0;
};

}  // namespace ecu::core::v2::transport
