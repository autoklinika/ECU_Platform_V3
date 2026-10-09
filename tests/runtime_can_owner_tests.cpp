// Portable tests of CanOwner against an in-memory link and lock.

#include "ecu/runtime/can_owner.hpp"
#include "support/check.hpp"

#include <string>
#include <vector>

namespace rt = ecu::runtime;

namespace {

class FakeLock final : public rt::IExclusiveLock {
 public:
  rt::LockStatus next{rt::LockStatus::acquired};
  bool is_held{false};
  int releases{0};
  rt::LockStatus try_acquire() noexcept override {
    if (next == rt::LockStatus::acquired) is_held = true;
    return next;
  }
  void release() noexcept override {
    is_held = false;
    ++releases;
  }
  bool held() const noexcept override { return is_held; }
};

// Models a SocketCAN link: configuration is only accepted while DOWN, like
// the kernel. Can be told to fail individual steps or to lie on read-back.
class FakeLink final : public rt::ICanLinkControl {
 public:
  rt::CanLinkState state{true, false, false, false, false, 0U};
  std::vector<std::string> calls;
  rt::LinkStatus fail_down{rt::LinkStatus::ok};
  rt::LinkStatus fail_configure{rt::LinkStatus::ok};
  rt::LinkStatus fail_up{rt::LinkStatus::ok};
  bool down_has_no_effect{false};
  bool report_wrong_bitrate{false};
  bool missing{false};

  rt::LinkStatus set_down() noexcept override {
    calls.emplace_back("down");
    if (fail_down != rt::LinkStatus::ok) return fail_down;
    if (!down_has_no_effect) state.up = false;
    return rt::LinkStatus::ok;
  }
  rt::LinkStatus configure(const rt::CanLinkConfig& c) noexcept override {
    calls.emplace_back("configure:" + std::to_string(c.bitrate) +
                       (c.mode == rt::CanLinkMode::listen_only ? ":listen"
                                                               : ":normal"));
    if (fail_configure != rt::LinkStatus::ok) return fail_configure;
    if (state.up) return rt::LinkStatus::command_failed;  // kernel: EBUSY
    state.bitrate = c.bitrate;
    state.listen_only = c.mode == rt::CanLinkMode::listen_only;
    return rt::LinkStatus::ok;
  }
  rt::LinkStatus set_up() noexcept override {
    calls.emplace_back("up");
    if (fail_up != rt::LinkStatus::ok) return fail_up;
    state.up = true;
    return rt::LinkStatus::ok;
  }
  rt::CanLinkQuery query() noexcept override {
    calls.emplace_back("query");
    if (missing) return {rt::LinkStatus::not_found, {}};
    auto reported = state;
    if (report_wrong_bitrate) reported.bitrate = 125000U;
    return {rt::LinkStatus::ok, reported};
  }

  bool touched() const {
    for (const auto& c : calls)
      if (c != "query") return true;
    return false;
  }
};

const rt::CanLinkConfig k500Listen{500000U, rt::CanLinkMode::listen_only};
const rt::CanLinkConfig k250Normal{250000U, rt::CanLinkMode::normal};

void link_is_never_touched_without_the_lock() {
  FakeLock lock;
  FakeLink link;
  lock.next = rt::LockStatus::busy;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::lock_busy);
  CHECK(owner.state() == rt::CanOwnerState::released);
  CHECK(link.calls.empty());
  CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::not_owner);
  CHECK(owner.enter_safe_state() == rt::CanOwnerResult::not_owner);
  CHECK(link.calls.empty());

  lock.next = rt::LockStatus::error;
  CHECK(owner.acquire() == rt::CanOwnerResult::lock_error);
  CHECK(link.calls.empty());
}

void acquire_forces_a_link_left_up_by_a_crashed_owner_down() {
  FakeLock lock;
  FakeLink link;
  link.state.up = true;  // V2 failure mode: adapter killed mid-operation
  link.state.bitrate = 250000U;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::ok);
  CHECK(owner.state() == rt::CanOwnerState::safe);
  CHECK(!link.state.up);
  CHECK(link.calls.size() >= 2U && link.calls[0] == "down");
}

void configure_goes_through_verified_down_and_reads_back() {
  FakeLock lock;
  FakeLink link;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::ok);
  link.calls.clear();
  CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::ok);
  CHECK(owner.state() == rt::CanOwnerState::configured);
  CHECK(owner.active_config() == k500Listen);
  const std::vector<std::string> expected{
      "down", "query", "configure:500000:listen", "up", "query"};
  CHECK(link.calls == expected);
  CHECK(link.state.up && link.state.listen_only &&
        link.state.bitrate == 500000U);

  // Re-applying the same config only verifies it; no link flapping.
  link.calls.clear();
  CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::ok);
  CHECK(link.calls == std::vector<std::string>{"query"});

  // Changing mode (listen -> normal) always passes through DOWN.
  link.calls.clear();
  CHECK(owner.configure(k250Normal) == rt::CanOwnerResult::ok);
  CHECK(!link.calls.empty() && link.calls.front() == "down");
  CHECK(!link.state.listen_only && link.state.bitrate == 250000U);
}

void read_back_mismatch_ends_in_safe_state() {
  FakeLock lock;
  FakeLink link;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::ok);
  link.report_wrong_bitrate = true;
  CHECK(owner.configure(k500Listen) ==
        rt::CanOwnerResult::verification_failed);
  CHECK(owner.state() == rt::CanOwnerState::safe);
  CHECK(!link.state.up);
  CHECK(owner.last_observed().bitrate == 125000U);  // evidence is kept
}

void failed_step_ends_in_safe_state() {
  FakeLock lock;
  FakeLink link;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::ok);
  link.fail_up = rt::LinkStatus::timeout;
  CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::command_failed);
  CHECK(owner.state() == rt::CanOwnerState::safe);
  CHECK(!link.state.up);

  link.fail_up = rt::LinkStatus::ok;
  link.fail_configure = rt::LinkStatus::command_failed;
  CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::command_failed);
  CHECK(owner.state() == rt::CanOwnerState::safe);
}

void unverifiable_safe_state_is_a_fault_that_blocks_configuration() {
  FakeLock lock;
  FakeLink link;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::ok);
  CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::ok);

  link.down_has_no_effect = true;  // link stays UP whatever we do
  CHECK(owner.enter_safe_state() == rt::CanOwnerResult::safe_state_failed);
  CHECK(owner.state() == rt::CanOwnerState::fault);
  CHECK(owner.configure(k250Normal) == rt::CanOwnerResult::safe_state_failed);
  CHECK(owner.state() == rt::CanOwnerState::fault);

  link.down_has_no_effect = false;  // recovery is explicit and verified
  CHECK(owner.enter_safe_state() == rt::CanOwnerResult::ok);
  CHECK(owner.state() == rt::CanOwnerState::safe);
  CHECK(owner.configure(k250Normal) == rt::CanOwnerResult::ok);
}

void unsupported_bitrate_is_rejected_before_touching_the_link() {
  FakeLock lock;
  FakeLink link;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::ok);
  link.calls.clear();
  CHECK(owner.configure({333333U, rt::CanLinkMode::normal}) ==
        rt::CanOwnerResult::invalid_config);
  CHECK(link.calls.empty());
}

void missing_interface_is_reported_and_silent() {
  FakeLock lock;
  FakeLink link;
  link.missing = true;
  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::link_unavailable);
  CHECK(owner.owns_link());
  CHECK(owner.state() == rt::CanOwnerState::safe);
  CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::link_unavailable);
  CHECK(owner.state() == rt::CanOwnerState::safe);
}

void release_and_destructor_return_link_to_safe_state() {
  FakeLock lock;
  FakeLink link;
  {
    rt::CanOwner owner{lock, link};
    CHECK(owner.acquire() == rt::CanOwnerResult::ok);
    CHECK(owner.configure(k500Listen) == rt::CanOwnerResult::ok);
    CHECK(link.state.up && lock.is_held);
  }
  CHECK(!link.state.up);
  CHECK(!lock.is_held);
  CHECK(lock.releases == 1);

  rt::CanOwner owner{lock, link};
  CHECK(owner.acquire() == rt::CanOwnerResult::ok);
  owner.release();
  owner.release();  // idempotent
  CHECK(lock.releases == 2);
  CHECK(owner.state() == rt::CanOwnerState::released);
}

}  // namespace

int main() {
  link_is_never_touched_without_the_lock();
  acquire_forces_a_link_left_up_by_a_crashed_owner_down();
  configure_goes_through_verified_down_and_reads_back();
  read_back_mismatch_ends_in_safe_state();
  failed_step_ends_in_safe_state();
  unverifiable_safe_state_is_a_fault_that_blocks_configuration();
  unsupported_bitrate_is_rejected_before_touching_the_link();
  missing_interface_is_reported_and_silent();
  release_and_destructor_return_link_to_safe_state();
  return ecu::test::finish("runtime_can_owner_tests");
}
