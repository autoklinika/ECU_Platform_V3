#include "ecu/runtime/can_owner.hpp"

namespace ecu::runtime {
namespace {

[[nodiscard]] CanOwnerResult from_link(const LinkStatus status) noexcept {
  switch (status) {
    case LinkStatus::ok: return CanOwnerResult::ok;
    case LinkStatus::not_found: return CanOwnerResult::link_unavailable;
    case LinkStatus::not_can: return CanOwnerResult::link_unavailable;
    case LinkStatus::command_failed: return CanOwnerResult::command_failed;
    case LinkStatus::timeout: return CanOwnerResult::command_failed;
    case LinkStatus::io_error: return CanOwnerResult::link_unavailable;
  }
  return CanOwnerResult::link_unavailable;
}

[[nodiscard]] bool matches(const CanLinkState& s,
                           const CanLinkConfig& c) noexcept {
  return s.present && s.up && !s.bus_off && !s.fd_enabled &&
         s.bitrate == c.bitrate &&
         s.listen_only == (c.mode == CanLinkMode::listen_only);
}

}  // namespace

const char* to_string(const CanOwnerState state) noexcept {
  switch (state) {
    case CanOwnerState::released: return "released";
    case CanOwnerState::safe: return "safe";
    case CanOwnerState::configured: return "configured";
    case CanOwnerState::fault: return "fault";
  }
  return "invalid";
}

const char* to_string(const CanOwnerResult result) noexcept {
  switch (result) {
    case CanOwnerResult::ok: return "ok";
    case CanOwnerResult::lock_busy: return "lock_busy";
    case CanOwnerResult::lock_error: return "lock_error";
    case CanOwnerResult::not_owner: return "not_owner";
    case CanOwnerResult::invalid_config: return "invalid_config";
    case CanOwnerResult::link_unavailable: return "link_unavailable";
    case CanOwnerResult::command_failed: return "command_failed";
    case CanOwnerResult::verification_failed: return "verification_failed";
    case CanOwnerResult::safe_state_failed: return "safe_state_failed";
  }
  return "invalid";
}

CanOwner::CanOwner(IExclusiveLock& lock, ICanLinkControl& link) noexcept
    : lock_(lock), link_(link) {}

CanOwner::~CanOwner() { release(); }

CanOwnerResult CanOwner::finish(const CanOwnerResult result) noexcept {
  last_ = result;
  return result;
}

CanOwnerResult CanOwner::force_down() noexcept {
  // DOWN is attempted even if the previous command failed: the goal is to
  // leave the bus silent whatever happened before.
  const auto down = link_.set_down();
  const auto query = link_.query();
  observed_ = query.state;
  if (query.status == LinkStatus::not_found ||
      query.status == LinkStatus::not_can) {
    // A missing interface cannot transmit: treat as safe, report it.
    state_ = CanOwnerState::safe;
    active_ = {};
    return CanOwnerResult::link_unavailable;
  }
  if (down != LinkStatus::ok || query.status != LinkStatus::ok ||
      query.state.up) {
    state_ = CanOwnerState::fault;
    active_ = {};
    return CanOwnerResult::safe_state_failed;
  }
  state_ = CanOwnerState::safe;
  active_ = {};
  return CanOwnerResult::ok;
}

CanOwnerResult CanOwner::acquire() noexcept {
  if (owns_link()) return finish(CanOwnerResult::ok);
  switch (lock_.try_acquire()) {
    case LockStatus::acquired: break;
    case LockStatus::busy: return finish(CanOwnerResult::lock_busy);
    case LockStatus::error: return finish(CanOwnerResult::lock_error);
  }
  // A previous owner may have died with the link UP. We now hold the lock,
  // so nobody else can legitimately be using it: force it DOWN.
  state_ = CanOwnerState::fault;
  return finish(force_down());
}

CanOwnerResult CanOwner::enter_safe_state() noexcept {
  if (!owns_link()) return finish(CanOwnerResult::not_owner);
  return finish(force_down());
}

CanOwnerResult CanOwner::configure(const CanLinkConfig& config) noexcept {
  if (!owns_link()) return finish(CanOwnerResult::not_owner);
  if (!is_supported_bitrate(config.bitrate))
    return finish(CanOwnerResult::invalid_config);
  if (state_ == CanOwnerState::configured && active_ == config) {
    const auto query = link_.query();
    observed_ = query.state;
    if (query.status == LinkStatus::ok && matches(query.state, config))
      return finish(CanOwnerResult::ok);
  }

  // Always pass through a verified DOWN state before changing the link.
  if (const auto down = force_down(); down != CanOwnerResult::ok)
    return finish(down);

  auto result = from_link(link_.configure(config));
  if (result == CanOwnerResult::ok) result = from_link(link_.set_up());
  if (result == CanOwnerResult::ok) {
    const auto query = link_.query();
    observed_ = query.state;
    if (query.status != LinkStatus::ok) {
      result = from_link(query.status);
    } else if (!matches(query.state, config)) {
      result = CanOwnerResult::verification_failed;
    }
  }
  if (result != CanOwnerResult::ok) {
    const auto saved_observation = observed_;
    const auto safe = force_down();
    observed_ = saved_observation;
    return finish(safe == CanOwnerResult::safe_state_failed ? safe : result);
  }
  state_ = CanOwnerState::configured;
  active_ = config;
  return finish(CanOwnerResult::ok);
}

void CanOwner::release() noexcept {
  if (!owns_link()) return;
  last_ = force_down();
  lock_.release();
  state_ = CanOwnerState::released;
  active_ = {};
}

}  // namespace ecu::runtime
