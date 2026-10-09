#pragma once

#include <cstdint>
#include <limits>

namespace ecu::core::v2::runtime {

struct CancellationToken {
  std::uint64_t generation{0U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return generation != 0U;
  }
};

enum class CancellationBeginStatus : std::uint8_t {
  started,
  busy,
  generation_exhausted,
};

struct CancellationBeginResult {
  CancellationBeginStatus status{
      CancellationBeginStatus::generation_exhausted};
  CancellationToken token{};
};

enum class CancellationRequestStatus : std::uint8_t {
  requested,
  already_requested,
  invalid_token,
  stale_token,
  no_active_operation,
};

enum class CancellationCompleteStatus : std::uint8_t {
  completed,
  invalid_token,
  stale_token,
  no_active_operation,
};

class CancellationSource final {
 public:
  // Single-executor contract. A token identifies exactly one operation
  // generation. A stale token can never cancel a later operation.
  [[nodiscard]] CancellationBeginResult begin() noexcept {
    if (active_) {
      return {CancellationBeginStatus::busy, token()};
    }

    const auto maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    if (generation_ == maximum) {
      return {
          CancellationBeginStatus::generation_exhausted,
          {}};
    }

    ++generation_;
    active_ = true;
    requested_ = false;
    return {CancellationBeginStatus::started, token()};
  }

  [[nodiscard]] CancellationRequestStatus request(
      const CancellationToken operation) noexcept {
    if (!operation.valid()) {
      return CancellationRequestStatus::invalid_token;
    }
    if (!active_) {
      return CancellationRequestStatus::no_active_operation;
    }
    if (operation.generation != generation_) {
      return CancellationRequestStatus::stale_token;
    }
    if (requested_) {
      return CancellationRequestStatus::already_requested;
    }

    requested_ = true;
    return CancellationRequestStatus::requested;
  }

  [[nodiscard]] bool requested(
      const CancellationToken operation) const noexcept {
    return active_ &&
           operation.valid() &&
           operation.generation == generation_ &&
           requested_;
  }

  [[nodiscard]] CancellationCompleteStatus complete(
      const CancellationToken operation) noexcept {
    if (!operation.valid()) {
      return CancellationCompleteStatus::invalid_token;
    }
    if (!active_) {
      return CancellationCompleteStatus::no_active_operation;
    }
    if (operation.generation != generation_) {
      return CancellationCompleteStatus::stale_token;
    }

    active_ = false;
    requested_ = false;
    return CancellationCompleteStatus::completed;
  }

  [[nodiscard]] bool active() const noexcept {
    return active_;
  }

  [[nodiscard]] CancellationToken token() const noexcept {
    return active_ ? CancellationToken{generation_}
                   : CancellationToken{};
  }

 private:
  std::uint64_t generation_{0U};
  bool active_{false};
  bool requested_{false};
};

}  // namespace ecu::core::v2::runtime
