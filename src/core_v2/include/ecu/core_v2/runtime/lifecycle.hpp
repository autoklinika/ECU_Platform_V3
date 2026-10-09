#pragma once

#include <cstdint>

namespace ecu::core::v2::runtime {

enum class LifecycleState : std::uint8_t {
  stopped,
  starting,
  running,
  stopping,
  faulted,
};

enum class LifecycleStatus : std::uint8_t {
  ok,
  no_action,
  invalid_transition,
};

class LifecycleStateMachine final {
 public:
  [[nodiscard]] LifecycleState state() const noexcept {
    return state_;
  }

  [[nodiscard]] LifecycleStatus begin_start() noexcept {
    if (state_ == LifecycleState::starting ||
        state_ == LifecycleState::running) {
      return LifecycleStatus::no_action;
    }
    if (state_ != LifecycleState::stopped) {
      return LifecycleStatus::invalid_transition;
    }
    state_ = LifecycleState::starting;
    return LifecycleStatus::ok;
  }

  [[nodiscard]] LifecycleStatus mark_running() noexcept {
    if (state_ == LifecycleState::running) {
      return LifecycleStatus::no_action;
    }
    if (state_ != LifecycleState::starting) {
      return LifecycleStatus::invalid_transition;
    }
    state_ = LifecycleState::running;
    return LifecycleStatus::ok;
  }

  [[nodiscard]] LifecycleStatus begin_stop() noexcept {
    if (state_ == LifecycleState::stopping ||
        state_ == LifecycleState::stopped) {
      return LifecycleStatus::no_action;
    }
    if (state_ != LifecycleState::running &&
        state_ != LifecycleState::starting) {
      return LifecycleStatus::invalid_transition;
    }
    state_ = LifecycleState::stopping;
    return LifecycleStatus::ok;
  }

  [[nodiscard]] LifecycleStatus mark_stopped() noexcept {
    if (state_ == LifecycleState::stopped) {
      return LifecycleStatus::no_action;
    }
    if (state_ != LifecycleState::stopping) {
      return LifecycleStatus::invalid_transition;
    }
    state_ = LifecycleState::stopped;
    return LifecycleStatus::ok;
  }

  void mark_faulted() noexcept {
    state_ = LifecycleState::faulted;
  }

  [[nodiscard]] LifecycleStatus reset_fault() noexcept {
    if (state_ != LifecycleState::faulted) {
      return LifecycleStatus::invalid_transition;
    }
    state_ = LifecycleState::stopped;
    return LifecycleStatus::ok;
  }

 private:
  LifecycleState state_{LifecycleState::stopped};
};

class ILifecycleComponent {
 public:
  [[nodiscard]] virtual LifecycleState state() const noexcept = 0;
  [[nodiscard]] virtual LifecycleStatus start() noexcept = 0;
  [[nodiscard]] virtual LifecycleStatus service() noexcept = 0;
  [[nodiscard]] virtual LifecycleStatus stop() noexcept = 0;

 protected:
  ~ILifecycleComponent() = default;
};

}  // namespace ecu::core::v2::runtime
