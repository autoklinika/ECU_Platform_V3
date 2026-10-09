// ecu_bench_runtime — Krok 1: the single owner of one CAN link.
//
// At this step the runtime only owns the link: it takes the system-wide lock,
// forces the link into the safe state (DOWN), optionally holds one verified
// configuration, watches for foreign changes and returns the link to the safe
// state on SIGTERM/SIGINT/SIGHUP. Sessions, measurements and the API arrive in
// later steps on top of this owner.

#include "ecu/platform/linux/runtime/flock_exclusive_lock.hpp"
#include "ecu/platform/linux/runtime/ip_can_link_control.hpp"
#include "ecu/runtime/can_owner.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

namespace {

namespace rt = ecu::runtime;
namespace lx = ecu::platform::linux::runtime;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 64;
constexpr int kExitSafeStateFailed = 70;
constexpr int kExitLockError = 71;
constexpr int kExitLockBusy = 75;

struct Options {
  std::string interface_name;
  std::string lock_dir{"/run/ecu-bench"};
  std::string ip{"/usr/sbin/ip"};
  std::optional<rt::CanLinkConfig> hold;
};

void usage() {
  std::fputs(
      "usage: ecu_bench_runtime --interface <ifname> [--lock-dir <dir>]\n"
      "                         [--ip <path>] [--hold <bitrate>[:listen|:normal]]\n"
      "\n"
      "  --hold  keep one verified link configuration (default mode: listen)\n",
      stderr);
}

std::optional<rt::CanLinkConfig> parse_hold(const std::string& text) {
  rt::CanLinkConfig config{};
  config.mode = rt::CanLinkMode::listen_only;
  std::string number = text;
  const auto colon = text.find(':');
  if (colon != std::string::npos) {
    number = text.substr(0, colon);
    const auto mode = text.substr(colon + 1);
    if (mode == "normal") {
      config.mode = rt::CanLinkMode::normal;
    } else if (mode != "listen") {
      return std::nullopt;
    }
  }
  if (number.empty() || number.size() > 7) return std::nullopt;
  for (const char ch : number)
    if (ch < '0' || ch > '9') return std::nullopt;
  config.bitrate = static_cast<std::uint32_t>(std::stoul(number));
  if (!rt::is_supported_bitrate(config.bitrate)) return std::nullopt;
  return config;
}

std::optional<Options> parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (i + 1 >= argc) return std::nullopt;
    const std::string value = argv[++i];
    if (key == "--interface") {
      options.interface_name = value;
    } else if (key == "--lock-dir") {
      options.lock_dir = value;
    } else if (key == "--ip") {
      options.ip = value;
    } else if (key == "--hold") {
      options.hold = parse_hold(value);
      if (!options.hold) return std::nullopt;
    } else {
      return std::nullopt;
    }
  }
  if (!lx::valid_interface_name(options.interface_name) ||
      options.lock_dir.empty() || options.lock_dir.front() != '/' ||
      options.ip.empty() || options.ip.front() != '/')
    return std::nullopt;
  return options;
}

// One JSON object per line; journald keeps it, tools can parse it.
void event(const char* name, const rt::CanOwner& owner,
           const char* detail = nullptr) {
  const auto seen = owner.last_observed();
  std::printf(
      "{\"event\":\"%s\",\"owner\":\"%s\",\"result\":\"%s\","
      "\"link\":{\"present\":%s,\"up\":%s,\"bitrate\":%u,"
      "\"listen_only\":%s,\"bus_off\":%s}%s%s%s}\n",
      name, rt::to_string(owner.state()), rt::to_string(owner.last_result()),
      seen.present ? "true" : "false", seen.up ? "true" : "false",
      static_cast<unsigned>(seen.bitrate),
      seen.listen_only ? "true" : "false", seen.bus_off ? "true" : "false",
      detail ? ",\"detail\":\"" : "", detail ? detail : "",
      detail ? "\"" : "");
  std::fflush(stdout);
}

bool link_matches(const rt::CanLinkState& s, const rt::CanLinkConfig& c) {
  return s.present && s.up && !s.bus_off && !s.fd_enabled &&
         s.bitrate == c.bitrate &&
         s.listen_only == (c.mode == rt::CanLinkMode::listen_only);
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse(argc, argv);
  if (!options) {
    usage();
    return kExitUsage;
  }

  // Block termination signals before anything else so a SIGTERM arriving
  // during start-up is handled by the same safe shutdown path.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGHUP);
  if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) return kExitLockError;

  lx::FlockExclusiveLock lock{options->lock_dir + "/" +
                              options->interface_name + ".lock"};
  lx::IpCanLinkControl link{options->interface_name, options->ip};
  rt::CanOwner owner{lock, link};

  const auto acquired = owner.acquire();
  if (acquired == rt::CanOwnerResult::lock_busy) {
    event("lock_busy", owner, "another runtime owns this link");
    return kExitLockBusy;
  }
  if (acquired == rt::CanOwnerResult::lock_error) {
    event("lock_error", owner, lock.path().c_str());
    return kExitLockError;
  }
  event("acquired", owner);

  if (options->hold && owner.state() == rt::CanOwnerState::safe) {
    (void)owner.configure(*options->hold);
    event("configured", owner);
  }

  // Supervise until a termination signal. Once per second verify that nobody
  // changed the link behind the owner's back; any deviation is corrected by
  // returning to the safe state (the owner never silently re-applies config).
  int exit_code = kExitOk;
  for (;;) {
    timespec wait{1, 0};
    const int received = sigtimedwait(&signals, nullptr, &wait);
    if (received == SIGTERM || received == SIGINT || received == SIGHUP) {
      event("signal", owner, received == SIGTERM ? "SIGTERM"
                             : received == SIGINT ? "SIGINT" : "SIGHUP");
      break;
    }
    if (received < 0 && errno != EAGAIN && errno != EINTR) {
      exit_code = kExitLockError;
      break;
    }
    const auto observed = link.query();
    const bool expected_up = owner.state() == rt::CanOwnerState::configured;
    const bool deviation =
        observed.status == rt::LinkStatus::ok &&
        (expected_up ? !link_matches(observed.state, owner.active_config())
                     : observed.state.up);
    if (deviation || owner.state() == rt::CanOwnerState::fault) {
      (void)owner.enter_safe_state();
      event("foreign_change_corrected", owner);
    }
  }

  owner.release();
  const auto final_state = owner.last_result();
  event("released", owner);
  if (final_state == rt::CanOwnerResult::safe_state_failed)
    return kExitSafeStateFailed;
  return exit_code;
}
