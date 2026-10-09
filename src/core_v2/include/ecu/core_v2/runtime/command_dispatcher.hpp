#pragma once

#include "ecu/core_v2/runtime/command.hpp"
#include "ecu/core_v2/runtime/configuration_gate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

enum class CommandPolicyDecision : std::uint8_t {
  allow,
  deny,
  confirmation_required,
};

class ICommandPolicy {
 public:
  [[nodiscard]] virtual CommandPolicyDecision evaluate(
      const CommandView& command) const noexcept = 0;

 protected:
  ~ICommandPolicy() = default;
};

class AllowAllCommandPolicy final : public ICommandPolicy {
 public:
  [[nodiscard]] CommandPolicyDecision evaluate(
      const CommandView&) const noexcept override {
    return CommandPolicyDecision::allow;
  }
};

class ICommandHandler {
 public:
  [[nodiscard]] virtual CommandResult handle(
      const CommandView& command) noexcept = 0;

 protected:
  ~ICommandHandler() = default;
};

struct CommandExecutionContract {
  time::MonotonicDuration max_duration{0};
};

enum class CommandRegistrationStatus : std::uint8_t {
  registered,
  already_registered,
  type_conflict,
  invalid_argument,
  configuration_frozen,
  capacity_exhausted,
};

enum class CommandDispatchStatus : std::uint8_t {
  dispatched,
  invalid_command,
  configuration_not_frozen,
  no_handler,
  policy_denied,
  confirmation_required,
  busy,
};

struct CommandDispatchResult {
  CommandDispatchStatus status{
      CommandDispatchStatus::invalid_command};
  CommandResult result{};
};

struct CommandExecutionBudgetResult {
  bool valid{false};
  time::MonotonicDuration max_duration{0};
};

class CommandDispatcher final {
 public:
  static constexpr std::size_t kMaxHandlers = 64U;

  CommandDispatcher(
      const ICommandPolicy& policy,
      CommandExecutionContract policy_execution) noexcept;

  [[nodiscard]] CommandRegistrationStatus register_handler(
      CommandTypeId type,
      ICommandHandler& handler,
      CommandExecutionContract execution) noexcept;

  [[nodiscard]] bool freeze_configuration() noexcept;

  [[nodiscard]] CommandDispatchResult dispatch(
      const CommandView& command) noexcept;

  [[nodiscard]] std::size_t handler_count() const noexcept;
  [[nodiscard]] ConfigurationState configuration_state()
      const noexcept;

  [[nodiscard]] CommandExecutionBudgetResult
  max_dispatch_duration(CommandTypeId type) const noexcept;

 private:
  struct Entry {
    CommandTypeId type{0U};
    ICommandHandler* handler{nullptr};
    CommandExecutionContract execution{};
  };

  [[nodiscard]] Entry* find(CommandTypeId type) noexcept;
  [[nodiscard]] const Entry* find(CommandTypeId type) const noexcept;
  [[nodiscard]] Entry* find_free() noexcept;

  const ICommandPolicy& policy_;
  CommandExecutionContract policy_execution_{};
  ConfigurationGate configuration_{};
  std::array<Entry, kMaxHandlers> entries_{};
  bool dispatching_{false};
};

}  // namespace ecu::core::v2::runtime
