#include "ecu/platform/linux/runtime/ip_can_link_control.hpp"

#include "ecu/platform/linux/v2/socketcan_link_info.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace ecu::platform::linux::runtime {
namespace {

using ecu::runtime::CanLinkConfig;
using ecu::runtime::CanLinkMode;
using ecu::runtime::CanLinkQuery;
using ecu::runtime::LinkStatus;

[[nodiscard]] CanLinkQuery netlink_query(const std::string& name) noexcept {
  namespace v2 = ecu::platform::linux::v2;
  const auto result = v2::query_socketcan_link(name.c_str(), 100);
  CanLinkQuery out{};
  switch (result.status) {
    case v2::SocketCanLinkQueryStatus::ok: out.status = LinkStatus::ok; break;
    case v2::SocketCanLinkQueryStatus::not_found:
      out.status = LinkStatus::not_found;
      return out;
    case v2::SocketCanLinkQueryStatus::not_can:
      out.status = LinkStatus::command_failed;
      return out;
    case v2::SocketCanLinkQueryStatus::timeout:
      out.status = LinkStatus::timeout;
      return out;
    case v2::SocketCanLinkQueryStatus::io_error:
      out.status = LinkStatus::io_error;
      return out;
  }
  out.state.present = true;
  out.state.up = result.info.up;
  out.state.bus_off = result.info.bus_off;
  out.state.fd_enabled = result.info.fd_enabled;
  out.state.listen_only = result.info.listen_only_enabled;
  out.state.bitrate = result.info.nominal_bitrate;
  return out;
}

}  // namespace

bool valid_interface_name(const std::string& name) noexcept {
  if (name.empty() || name.size() > 15U) return false;
  for (const char ch : name) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                    ch == '.';
    if (!ok) return false;
  }
  return name.front() != '-';
}

LinkStatus run_bounded(const std::string& program,
                       const std::vector<std::string>& args,
                       const std::chrono::milliseconds timeout) noexcept {
  try {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2U);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    char* envp[] = {nullptr};

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
      return LinkStatus::io_error;
    (void)posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                           "/dev/null", O_RDONLY, 0);
    (void)posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                           "/dev/null", O_WRONLY, 0);
    (void)posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                           "/dev/null", O_WRONLY, 0);
    // The runtime blocks SIGTERM/SIGINT for sigwait(); the child must not
    // inherit that mask or ignored dispositions.
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return LinkStatus::io_error;
    }
    sigset_t empty;
    sigset_t all;
    sigemptyset(&empty);
    sigfillset(&all);
    (void)posix_spawnattr_setsigmask(&attributes, &empty);
    (void)posix_spawnattr_setsigdefault(&attributes, &all);
    (void)posix_spawnattr_setflags(
        &attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, program.c_str(), &actions,
                                    &attributes, argv.data(), envp);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return LinkStatus::command_failed;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int status = 0;
      const pid_t done = ::waitpid(pid, &status, WNOHANG);
      if (done == pid) {
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                   ? LinkStatus::ok
                   : LinkStatus::command_failed;
      }
      if (done < 0 && errno != EINTR) return LinkStatus::io_error;
      if (std::chrono::steady_clock::now() >= deadline) {
        (void)::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return LinkStatus::timeout;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
  } catch (...) {
    return LinkStatus::io_error;
  }
}

IpCanLinkControl::IpCanLinkControl(std::string interface_name,
                                   std::string ip_program,
                                   const std::chrono::milliseconds timeout)
    : interface_(std::move(interface_name)),
      ip_(std::move(ip_program)),
      timeout_(timeout) {}

LinkStatus IpCanLinkControl::run(
    const std::vector<std::string>& args) noexcept {
  if (!valid_interface_name(interface_)) return LinkStatus::command_failed;
  return run_bounded(ip_, args, timeout_);
}

LinkStatus IpCanLinkControl::set_down() noexcept {
  try {
    return run({"link", "set", interface_, "down"});
  } catch (...) {
    return LinkStatus::io_error;
  }
}

LinkStatus IpCanLinkControl::configure(const CanLinkConfig& config) noexcept {
  if (!ecu::runtime::is_supported_bitrate(config.bitrate))
    return LinkStatus::command_failed;
  try {
    return run({"link", "set", interface_, "type", "can", "bitrate",
                std::to_string(config.bitrate), "fd", "off", "listen-only",
                config.mode == CanLinkMode::listen_only ? "on" : "off"});
  } catch (...) {
    return LinkStatus::io_error;
  }
}

LinkStatus IpCanLinkControl::set_up() noexcept {
  try {
    return run({"link", "set", interface_, "up"});
  } catch (...) {
    return LinkStatus::io_error;
  }
}

CanLinkQuery IpCanLinkControl::query() noexcept {
  try {
    if (query_) return query_();
    if (!valid_interface_name(interface_)) return {LinkStatus::not_found, {}};
    return netlink_query(interface_);
  } catch (...) {
    return {LinkStatus::io_error, {}};
  }
}

}  // namespace ecu::platform::linux::runtime
