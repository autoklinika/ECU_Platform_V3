#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <limits>

namespace ecu::core::v2::transport {

bool CanFilter::valid() const noexcept {
  const auto limit = match_extended ? 0x1FFFFFFFU : 0x7FFU;
  return (match_standard || match_extended) &&
         identifier <= limit && mask <= limit;
}

bool CanFilter::matches(const CanFrame& frame) const noexcept {
  if (!valid() || !is_valid_can_frame(frame)) {
    return false;
  }

  const bool format_allowed =
      (frame.identifier_format == CanIdentifierFormat::standard_11_bit &&
       match_standard) ||
      (frame.identifier_format == CanIdentifierFormat::extended_29_bit &&
       match_extended);

  return format_allowed &&
         (frame.identifier & mask) == (identifier & mask);
}

CanBusRuntime::CanBusRuntime(ICanDriver& driver) noexcept
    : driver_(driver) {}

CanBusRuntime::~CanBusRuntime() noexcept {
  // Terminal cleanup cannot leave an arbiter lease pointing at a destroyed
  // owner token. Reentrancy here violates the adapter contract and cannot be
  // reported to a caller, so destruction still completes cleanup.
  close_session_keep_lease();
  external_reentry_detected_ = false;
  stop_pending_ = false;
  release_lease_only();
}

CanSubscriptionResult CanBusRuntime::subscribe(
    const CanFilter& filter,
    ICanFrameSink& sink,
    const CanSinkExecutionContract execution) noexcept {
  if (!configuration_.accepts_registration() ||
      state_ != CanBusState::configuring) {
    return {CanSubscriptionStatus::configuration_frozen, {}};
  }

  if (!filter.valid() ||
      execution.max_callback_duration.count() <= 0) {
    return {CanSubscriptionStatus::invalid_argument, {}};
  }

  for (std::size_t i = 0U; i < subscriptions_.size(); ++i) {
    auto& entry = subscriptions_[i];
    if (entry.used) {
      continue;
    }

    if (!add_callback_budget(execution.max_callback_duration)) {
      return {CanSubscriptionStatus::invalid_argument, {}};
    }

    auto generation = next_generation_++;
    if (generation == 0U) {
      generation = next_generation_++;
    }

    entry.used = true;
    entry.generation = generation;
    entry.filter = filter;
    entry.sink = &sink;
    entry.execution = execution;

    return {
        CanSubscriptionStatus::subscribed,
        CanSubscriptionHandle{
            static_cast<std::uint16_t>(i),
            generation}};
  }

  return {CanSubscriptionStatus::capacity_exhausted, {}};
}

bool CanBusRuntime::freeze_configuration() noexcept {
  if (operation_busy() ||
      state_ != CanBusState::configuring ||
      !configuration_.freeze()) {
    return false;
  }

  state_ = CanBusState::ready;
  return true;
}

CanStatus CanBusRuntime::start(
    const CanChannelConfig& config) noexcept {
  if (operation_busy()) {
    return CanStatus::busy;
  }
  if (state_ == CanBusState::faulted) {
    return CanStatus::faulted;
  }
  if (state_ == CanBusState::running) {
    return CanStatus::already_open;
  }
  if (state_ != CanBusState::ready &&
      state_ != CanBusState::stopped) {
    return CanStatus::invalid_state;
  }
  if (!is_valid_can_channel_config(config)) {
    return CanStatus::invalid_argument;
  }

  external_call_active_ = true;
  const auto channel_id = driver_.physical_channel_id();
  auto& arbiter = driver_.channel_arbiter();
  const auto execution = driver_.execution_contract();
  const auto capabilities = driver_.capabilities();
  external_call_active_ = false;

  if (external_reentry_detected_) {
    latch_external_reentrancy_fault();
    return CanStatus::contract_violation;
  }

  if (!channel_id.valid() ||
      !is_valid_can_driver_execution_contract(execution)) {
    state_ = CanBusState::faulted;
    return CanStatus::contract_violation;
  }

  if (!capabilities_support(capabilities, config)) {
    return CanStatus::unsupported;
  }

  const bool retained_lease = owns_driver_lease_;
  bool acquired = owns_driver_lease_;
  if (owns_driver_lease_) {
    if (channel_id_ != channel_id || arbiter_ != &arbiter) {
      state_ = CanBusState::faulted;
      return CanStatus::contract_violation;
    }
  } else {
    channel_id_ = channel_id;
    arbiter_ = &arbiter;

    external_call_active_ = true;
    acquired = arbiter_->try_acquire(channel_id_, this);
    external_call_active_ = false;

    if (acquired) {
      owns_driver_lease_ = true;
    }

    if (external_reentry_detected_) {
      latch_external_reentrancy_fault();
      return CanStatus::contract_violation;
    }

    if (!acquired) {
      channel_id_ = {};
      arbiter_ = nullptr;
      return CanStatus::busy;
    }
  }

  external_call_active_ = true;
  const bool already_open = driver_.is_open();
  external_call_active_ = false;

  if (external_reentry_detected_) {
    latch_external_reentrancy_fault();
    return CanStatus::contract_violation;
  }

  if (already_open) {
    if (retained_lease) {
      state_ = CanBusState::faulted;
      return CanStatus::contract_violation;
    }
    release_lease_only();
    if (external_reentry_detected_) {
      latch_external_reentrancy_fault();
      return CanStatus::contract_violation;
    }
    return CanStatus::already_open;
  }

  external_call_active_ = true;
  const auto status = driver_.open(config);
  external_call_active_ = false;

  if (status == CanStatus::ok) {
    owns_open_session_ = true;
  }

  if (external_reentry_detected_) {
    latch_external_reentrancy_fault();
    return CanStatus::contract_violation;
  }

  if (status != CanStatus::ok) {
    if (!retained_lease) {
      release_lease_only();
    }
    if (external_reentry_detected_) {
      latch_external_reentrancy_fault();
      return CanStatus::contract_violation;
    }
    latch_fault(status);
    return status;
  }

  active_config_ = config;
  active_capabilities_ = capabilities;
  active_execution_ = execution;
  last_rx_timestamp_ = time::MonotonicTime{0};
  has_last_rx_timestamp_ = false;
  state_ = CanBusState::running;
  return CanStatus::ok;
}

void CanBusRuntime::stop() noexcept {
  if (external_call_active_) {
    external_reentry_detected_ = true;
    stop_pending_ = true;
    return;
  }
  if (dispatching_) {
    stop_pending_ = true;
    return;
  }

  if (state_ == CanBusState::faulted) {
    return;
  }

  // A stopped runtime remains the authoritative owner of its physical
  // channel. Only explicit recovery from fault or destruction releases the
  // physical-channel lease. This prevents ownership churn between sessions.
  close_session_keep_lease();
  if (external_reentry_detected_) {
    latch_external_reentrancy_fault();
    return;
  }
  transition_to_stopped();
}

CanStatus CanBusRuntime::recover() noexcept {
  if (operation_busy()) {
    return CanStatus::busy;
  }
  if (state_ != CanBusState::faulted) {
    return CanStatus::invalid_state;
  }

  close_and_release();
  if (external_reentry_detected_) {
    external_reentry_detected_ = false;
    stop_pending_ = false;
    state_ = CanBusState::faulted;
    return CanStatus::contract_violation;
  }
  transition_to_stopped();
  return CanStatus::ok;
}

CanStatus CanBusRuntime::send(
    const CanFrame& frame) noexcept {
  if (operation_busy()) {
    return CanStatus::busy;
  }
  if (state_ == CanBusState::faulted) {
    return CanStatus::faulted;
  }
  if (state_ != CanBusState::running) {
    return CanStatus::not_open;
  }

  const auto validation = validate_frame(frame);
  if (validation != CanStatus::ok) {
    return validation;
  }
  if (active_config_.mode == CanMode::listen_only) {
    return CanStatus::unsupported;
  }

  external_call_active_ = true;
  const auto status = driver_.try_send(frame);
  external_call_active_ = false;

  if (external_reentry_detected_) {
    latch_external_reentrancy_fault();
    return CanStatus::contract_violation;
  }

  latch_fault(status);
  return status;
}

CanPollResult CanBusRuntime::poll(
    const std::size_t max_frames) noexcept {
  CanPollResult result{};

  if (operation_busy()) {
    result.status = CanStatus::busy;
    return result;
  }
  if (state_ == CanBusState::faulted) {
    result.status = CanStatus::faulted;
    return result;
  }
  if (state_ != CanBusState::running) {
    result.status = CanStatus::not_open;
    return result;
  }
  if (max_frames == 0U) {
    result.status = CanStatus::invalid_argument;
    return result;
  }

  const auto delivery_limit =
      (std::numeric_limits<std::size_t>::max)() /
      kMaxSubscriptions;
  if (max_frames > delivery_limit) {
    result.status = CanStatus::invalid_argument;
    return result;
  }

  const auto declared_budget =
      max_poll_execution_duration(max_frames);
  if (declared_budget.status != CanStatus::ok) {
    result.status = declared_budget.status;
    return result;
  }

  for (std::size_t i = 0U; i < max_frames; ++i) {
    external_call_active_ = true;
    const auto received = driver_.try_receive();
    external_call_active_ = false;

    const auto drop_max =
        (std::numeric_limits<std::uint32_t>::max)();
    if (received.dropped_frames_since_last_receive >
        drop_max - result.rx_dropped) {
      result.rx_dropped = drop_max;
    } else {
      result.rx_dropped +=
          received.dropped_frames_since_last_receive;
    }

    if (stop_pending_) {
      latch_external_reentrancy_fault();
      result.status = CanStatus::contract_violation;
      return result;
    }

    if (received.status == CanStatus::would_block) {
      result.status = CanStatus::ok;
      return result;
    }

    if (received.status != CanStatus::ok) {
      result.status = received.status;
      latch_fault(received.status);
      return result;
    }

    ++result.frames_received;
    result.status = validate_received_frame(received.value);
    if (result.status != CanStatus::ok) {
      latch_fault(result.status);
      return result;
    }

    const auto delivered = dispatch(received.value);
    const auto delivery_max =
        (std::numeric_limits<std::size_t>::max)();
    if (delivered > delivery_max - result.deliveries) {
      state_ = CanBusState::faulted;
      result.status = CanStatus::contract_violation;
      return result;
    }
    result.deliveries += delivered;

    if (stop_pending_) {
      stop_pending_ = false;
      stop();
      if (state_ == CanBusState::faulted) {
        result.status = CanStatus::contract_violation;
      }
      return result;
    }
  }

  result.status = CanStatus::ok;
  return result;
}

CanBusState CanBusRuntime::state() const noexcept {
  return state_;
}

std::size_t CanBusRuntime::subscription_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& entry : subscriptions_) {
    if (entry.used) {
      ++count;
    }
  }
  return count;
}

time::MonotonicDuration
CanBusRuntime::max_single_frame_dispatch_duration() const noexcept {
  return callback_budget_;
}

CanExecutionBudgetResult CanBusRuntime::max_poll_execution_duration(
    const std::size_t max_frames) const noexcept {
  CanExecutionBudgetResult result{};

  if (state_ != CanBusState::running) {
    result.status = CanStatus::invalid_state;
    return result;
  }
  if (max_frames == 0U) {
    result.status = CanStatus::invalid_argument;
    return result;
  }

  const auto max_rep =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();

  const auto receive_count =
      active_execution_.max_try_receive_duration.count();
  const auto callback_count = callback_budget_.count();

  if (receive_count > max_rep - callback_count) {
    result.status = CanStatus::contract_violation;
    return result;
  }

  const auto per_frame = receive_count + callback_count;
  if (per_frame <= 0) {
    result.status = CanStatus::contract_violation;
    return result;
  }

  const auto frame_limit =
      static_cast<std::uint64_t>(max_rep / per_frame);
  if (static_cast<std::uint64_t>(max_frames) > frame_limit) {
    result.status = CanStatus::invalid_argument;
    return result;
  }

  const auto frame_budget =
      per_frame *
      static_cast<time::MonotonicDuration::rep>(max_frames);
  const auto close_count =
      active_execution_.max_close_duration.count();
  if (frame_budget > max_rep - close_count) {
    result.status = CanStatus::invalid_argument;
    return result;
  }

  // A subscriber may request one deferred stop. Normal stop retains the
  // physical-channel lease, so its worst external work is one bounded close().
  result.status = CanStatus::ok;
  result.max_duration =
      time::MonotonicDuration{frame_budget + close_count};
  return result;
}

CanExecutionBudgetResult
CanBusRuntime::max_send_execution_duration() const noexcept {
  CanExecutionBudgetResult result{};
  if (state_ != CanBusState::running) {
    result.status = CanStatus::invalid_state;
    return result;
  }

  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();
  const auto send_count =
      active_execution_.max_try_send_duration.count();
  const auto close_count =
      active_execution_.max_close_duration.count();
  if (send_count <= 0 || close_count <= 0 ||
      send_count > maximum - close_count) {
    result.status = CanStatus::contract_violation;
    return result;
  }

  // Compliant send uses only try_send(). The larger bound also covers the
  // handled contract-violation path where driver reentrancy forces close().
  result.status = CanStatus::ok;
  result.max_duration =
      time::MonotonicDuration{send_count + close_count};
  return result;
}

bool CanBusRuntime::operation_busy() const noexcept {
  return dispatching_ || external_call_active_;
}

bool CanBusRuntime::add_callback_budget(
    const time::MonotonicDuration duration) noexcept {
  if (duration.count() <= 0) {
    return false;
  }

  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();
  if (callback_budget_.count() >
      maximum - duration.count()) {
    return false;
  }

  callback_budget_ += duration;
  return true;
}

std::size_t CanBusRuntime::dispatch(
    const ReceivedCanFrame& frame) noexcept {
  dispatching_ = true;
  std::size_t deliveries = 0U;

  for (auto& entry : subscriptions_) {
    if (!entry.used || entry.sink == nullptr ||
        !entry.filter.matches(frame.frame)) {
      continue;
    }

    entry.sink->on_can_frame(frame);
    ++deliveries;
  }

  dispatching_ = false;
  return deliveries;
}

CanStatus CanBusRuntime::validate_frame(
    const CanFrame& frame) const noexcept {
  if (!is_valid_can_frame(frame)) {
    return CanStatus::invalid_frame;
  }
  if ((frame.format == CanFrameFormat::fd &&
       !active_config_.fd_enabled) ||
      !capabilities_support_frame(active_capabilities_, frame)) {
    return CanStatus::unsupported;
  }
  return CanStatus::ok;
}

CanStatus CanBusRuntime::validate_received_frame(
    const ReceivedCanFrame& frame) noexcept {
  const auto frame_status = validate_frame(frame.frame);
  if (frame_status != CanStatus::ok) {
    return frame_status;
  }

  if (!time::is_valid_clock_reading(
          frame.timestamp,
          active_config_.timestamp_domain) ||
      frame.timestamp.uncertainty >
          active_execution_.max_rx_timestamp_uncertainty ||
      (has_last_rx_timestamp_ &&
       frame.timestamp.value < last_rx_timestamp_)) {
    return CanStatus::invalid_timestamp;
  }

  last_rx_timestamp_ = frame.timestamp.value;
  has_last_rx_timestamp_ = true;
  return CanStatus::ok;
}

void CanBusRuntime::latch_fault(
    const CanStatus status) noexcept {
  if (status == CanStatus::bus_off ||
      status == CanStatus::io_error ||
      status == CanStatus::not_open ||
      status == CanStatus::invalid_timestamp ||
      status == CanStatus::contract_violation) {
    state_ = CanBusState::faulted;
  }
}

void CanBusRuntime::latch_external_reentrancy_fault() noexcept {
  close_session_keep_lease();
  external_reentry_detected_ = false;
  stop_pending_ = false;
  state_ = CanBusState::faulted;
}

void CanBusRuntime::release_lease_only() noexcept {
  if (!owns_driver_lease_ || arbiter_ == nullptr ||
      !channel_id_.valid()) {
    return;
  }

  external_call_active_ = true;
  arbiter_->release(channel_id_, this);
  external_call_active_ = false;
  owns_driver_lease_ = false;
}

void CanBusRuntime::close_session_keep_lease() noexcept {
  if (!owns_open_session_) {
    return;
  }

  external_call_active_ = true;
  driver_.close();
  external_call_active_ = false;
  owns_open_session_ = false;
  last_rx_timestamp_ = time::MonotonicTime{0};
  has_last_rx_timestamp_ = false;
}

void CanBusRuntime::close_and_release() noexcept {
  close_session_keep_lease();
  if (external_reentry_detected_) {
    return;
  }
  release_lease_only();
}

void CanBusRuntime::transition_to_stopped() noexcept {
  external_reentry_detected_ = false;
  stop_pending_ = false;
  state_ =
      configuration_.state() == runtime::ConfigurationState::frozen
          ? CanBusState::stopped
          : CanBusState::configuring;
}

}  // namespace ecu::core::v2::transport
