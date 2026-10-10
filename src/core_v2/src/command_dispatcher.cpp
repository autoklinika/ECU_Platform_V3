#include "ecu/core_v2/runtime/command_dispatcher.hpp"

#include <limits>

namespace ecu::core::v2::runtime {

CommandDispatcher::CommandDispatcher(
    const ICommandPolicy& policy,
    const CommandExecutionContract policy_execution) noexcept
    : policy_(policy),
      policy_execution_(policy_execution) {}

CommandRegistrationStatus CommandDispatcher::register_handler(
    const CommandTypeId type,
    ICommandHandler& handler,
    const CommandExecutionContract execution) noexcept {
  if (!configuration_.accepts_registration()) {
    return CommandRegistrationStatus::configuration_frozen;
  }
  if (type == 0U ||
      execution.max_duration.count() <= 0 ||
      policy_execution_.max_duration.count() <= 0) {
    return CommandRegistrationStatus::invalid_argument;
  }

  if (auto* existing = find(type); existing != nullptr) {
    return existing->handler == &handler
               ? CommandRegistrationStatus::already_registered
               : CommandRegistrationStatus::type_conflict;
  }

  auto* slot = find_free();
  if (slot == nullptr) {
    return CommandRegistrationStatus::capacity_exhausted;
  }

  slot->type = type;
  slot->handler = &handler;
  slot->execution = execution;
  return CommandRegistrationStatus::registered;
}

bool CommandDispatcher::freeze_configuration() noexcept {
  if (policy_execution_.max_duration.count() <= 0 ||
      handler_count() == 0U) {
    return false;
  }
  return configuration_.freeze();
}

CommandDispatchResult CommandDispatcher::dispatch(
    const CommandView& command) noexcept {
  CommandDispatchResult result{};
  if (!is_valid_command_view(command)) {
    return result;
  }
  if (configuration_.state() != ConfigurationState::frozen) {
    result.status =
        CommandDispatchStatus::configuration_not_frozen;
    return result;
  }
  if (dispatching_) {
    result.status = CommandDispatchStatus::busy;
    result.result = {
        CommandOutcome::rejected,
        0U};
    return result;
  }

  auto* entry = find(command.header.type);
  if (entry == nullptr || entry->handler == nullptr) {
    result.status = CommandDispatchStatus::no_handler;
    return result;
  }

  dispatching_ = true;
  const auto decision = policy_.evaluate(command);
  if (decision == CommandPolicyDecision::deny) {
    dispatching_ = false;
    result.status = CommandDispatchStatus::policy_denied;
    result.result = {
        CommandOutcome::rejected,
        0U};
    return result;
  }
  if (decision ==
      CommandPolicyDecision::confirmation_required) {
    dispatching_ = false;
    result.status =
        CommandDispatchStatus::confirmation_required;
    result.result = {
        CommandOutcome::rejected,
        0U};
    return result;
  }

  result.result = entry->handler->handle(command);
  dispatching_ = false;
  result.status = CommandDispatchStatus::dispatched;
  return result;
}

std::size_t CommandDispatcher::handler_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& entry : entries_) {
    if (entry.handler != nullptr) {
      ++count;
    }
  }
  return count;
}

ConfigurationState
CommandDispatcher::configuration_state() const noexcept {
  return configuration_.state();
}

CommandExecutionBudgetResult
CommandDispatcher::max_dispatch_duration(
    const CommandTypeId type) const noexcept {
  CommandExecutionBudgetResult result{};
  const auto* entry = find(type);
  if (entry == nullptr ||
      entry->handler == nullptr ||
      policy_execution_.max_duration.count() <= 0 ||
      entry->execution.max_duration.count() <= 0) {
    return result;
  }

  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();
  const auto policy =
      policy_execution_.max_duration.count();
  const auto handler =
      entry->execution.max_duration.count();
  if (policy > maximum - handler) {
    return result;
  }

  result.valid = true;
  result.max_duration =
      time::MonotonicDuration{policy + handler};
  return result;
}

CommandDispatcher::Entry* CommandDispatcher::find(
    const CommandTypeId type) noexcept {
  for (auto& entry : entries_) {
    if (entry.handler != nullptr && entry.type == type) {
      return &entry;
    }
  }
  return nullptr;
}

const CommandDispatcher::Entry* CommandDispatcher::find(
    const CommandTypeId type) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.handler != nullptr && entry.type == type) {
      return &entry;
    }
  }
  return nullptr;
}

CommandDispatcher::Entry* CommandDispatcher::find_free()
    noexcept {
  for (auto& entry : entries_) {
    if (entry.handler == nullptr) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace ecu::core::v2::runtime
