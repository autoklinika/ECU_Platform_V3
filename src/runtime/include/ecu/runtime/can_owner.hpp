#pragma once

#include "ecu/runtime/can_link.hpp"

#include <cstdint>

namespace ecu::runtime {

enum class CanOwnerState : std::uint8_t {
  released,    // lock not held; the link is not touched
  safe,        // lock held; link verified DOWN
  configured,  // lock held; link verified UP with active_config()
  fault,       // lock held; safe state could not be verified
};

enum class CanOwnerResult : std::uint8_t {
  ok,
  lock_busy,
  lock_error,
  not_owner,
  invalid_config,
  link_unavailable,
  command_failed,
  verification_failed,
  safe_state_failed,
};

[[nodiscard]] const char* to_string(CanOwnerState state) noexcept;
[[nodiscard]] const char* to_string(CanOwnerResult result) noexcept;

// The single authority over one physical CAN link.
//
// Rules enforced here:
//  - the link is never touched without holding the exclusive lock;
//  - acquiring ownership always forces the link DOWN first, so a link left UP
//    by a crashed process can never leak into a new session;
//  - every change is verified by reading the link state back;
//  - any failure ends in the safe state (DOWN) or, if that cannot be verified,
//    in `fault`, which refuses configure() until safe state is re-established;
//  - the destructor returns the link to the safe state and releases the lock.
//
// Not thread-safe: owned and driven by the runtime's engine thread.
class CanOwner {
 public:
  CanOwner(IExclusiveLock& lock, ICanLinkControl& link) noexcept;
  ~CanOwner();
  CanOwner(const CanOwner&) = delete;
  CanOwner& operator=(const CanOwner&) = delete;

  [[nodiscard]] CanOwnerResult acquire() noexcept;
  [[nodiscard]] CanOwnerResult configure(const CanLinkConfig& config) noexcept;
  [[nodiscard]] CanOwnerResult enter_safe_state() noexcept;
  void release() noexcept;

  [[nodiscard]] CanOwnerState state() const noexcept { return state_; }
  [[nodiscard]] bool owns_link() const noexcept {
    return state_ != CanOwnerState::released;
  }
  // Valid only in the `configured` state.
  [[nodiscard]] CanLinkConfig active_config() const noexcept {
    return active_;
  }
  [[nodiscard]] CanOwnerResult last_result() const noexcept { return last_; }
  [[nodiscard]] CanLinkState last_observed() const noexcept {
    return observed_;
  }

 private:
  [[nodiscard]] CanOwnerResult finish(CanOwnerResult result) noexcept;
  [[nodiscard]] CanOwnerResult force_down() noexcept;

  IExclusiveLock& lock_;
  ICanLinkControl& link_;
  CanOwnerState state_{CanOwnerState::released};
  CanLinkConfig active_{};
  CanOwnerResult last_{CanOwnerResult::ok};
  CanLinkState observed_{};
};

}  // namespace ecu::runtime
