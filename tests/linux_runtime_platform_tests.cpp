// Linux tests of the system-wide lock and of the iproute2 link control.
// No CAN hardware is needed: the ip program is replaced by a recording script.

#include "ecu/platform/linux/runtime/flock_exclusive_lock.hpp"
#include "ecu/platform/linux/runtime/ip_can_link_control.hpp"
#include "support/check.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rt = ecu::runtime;
namespace lx = ecu::platform::linux::runtime;

namespace {

std::string make_temp_dir() {
  char pattern[] = "/tmp/ecu-v3-test-XXXXXX";
  const char* dir = ::mkdtemp(pattern);
  return dir ? std::string{dir} : std::string{};
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void write_script(const std::string& path, const std::string& body) {
  {
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body;
  }
  ::chmod(path.c_str(), 0700);
}

void lock_is_exclusive_between_owners() {
  const auto dir = make_temp_dir();
  CHECK(!dir.empty());
  lx::FlockExclusiveLock first{dir + "/can0.lock"};
  lx::FlockExclusiveLock second{dir + "/can0.lock"};
  CHECK(first.try_acquire() == rt::LockStatus::acquired);
  CHECK(first.held());
  CHECK(second.try_acquire() == rt::LockStatus::busy);
  CHECK(!second.held());
  first.release();
  CHECK(second.try_acquire() == rt::LockStatus::acquired);

  // A different interface is an independent resource.
  lx::FlockExclusiveLock other{dir + "/can1.lock"};
  CHECK(other.try_acquire() == rt::LockStatus::acquired);
}

void lock_survives_nothing_but_its_process_even_sigkill() {
  const auto dir = make_temp_dir();
  const auto path = dir + "/can0.lock";
  int ready[2];
  CHECK(::pipe(ready) == 0);
  const pid_t child = ::fork();
  if (child == 0) {
    lx::FlockExclusiveLock held{path};
    const char ok = held.try_acquire() == rt::LockStatus::acquired ? '1' : '0';
    if (::write(ready[1], &ok, 1) != 1) ::_exit(2);
    for (;;) ::pause();
  }
  char ok = '0';
  CHECK(::read(ready[0], &ok, 1) == 1 && ok == '1');
  lx::FlockExclusiveLock parent{path};
  CHECK(parent.try_acquire() == rt::LockStatus::busy);
  ::kill(child, SIGKILL);  // crash without any cleanup code running
  int status = 0;
  ::waitpid(child, &status, 0);
  CHECK(parent.try_acquire() == rt::LockStatus::acquired);
  ::close(ready[0]);
  ::close(ready[1]);
}

void lock_reports_errors_instead_of_pretending() {
  lx::FlockExclusiveLock missing{"/nonexistent-ecu-dir/can0.lock"};
  CHECK(missing.try_acquire() == rt::LockStatus::error);
  lx::FlockExclusiveLock empty{""};
  CHECK(empty.try_acquire() == rt::LockStatus::error);
}

void ip_commands_match_the_v2_proven_command_lines() {
  const auto dir = make_temp_dir();
  const auto log = dir + "/ip.log";
  const auto ip = dir + "/ip";
  write_script(ip, "echo \"$*\" >> " + log + "\n");
  lx::IpCanLinkControl link{"can0", ip};
  CHECK(link.set_down() == rt::LinkStatus::ok);
  CHECK(link.configure({500000U, rt::CanLinkMode::listen_only}) ==
        rt::LinkStatus::ok);
  CHECK(link.configure({250000U, rt::CanLinkMode::normal}) ==
        rt::LinkStatus::ok);
  CHECK(link.set_up() == rt::LinkStatus::ok);
  CHECK(read_file(log) ==
        "link set can0 down\n"
        "link set can0 type can bitrate 500000 fd off listen-only on\n"
        "link set can0 type can bitrate 250000 fd off listen-only off\n"
        "link set can0 up\n");
}

void ip_child_runs_without_environment_or_inherited_lock() {
  const auto dir = make_temp_dir();
  const auto out = dir + "/env.txt";
  const auto ip = dir + "/ip";
  // The child records its environment and its open descriptors. A leaked
  // lock descriptor would keep the bench locked by a stray ip process.
  write_script(ip, "env > " + out + "\nls -l /proc/$$/fd >> " + out + "\n");
  ::setenv("ECU_V3_TEST_SECRET", "must-not-leak", 1);
  lx::FlockExclusiveLock lock{dir + "/can0.lock"};
  CHECK(lock.try_acquire() == rt::LockStatus::acquired);
  lx::IpCanLinkControl link{"can0", ip};
  CHECK(link.set_down() == rt::LinkStatus::ok);
  const auto text = read_file(out);
  CHECK(!text.empty());
  CHECK(text.find("ECU_V3_TEST_SECRET") == std::string::npos);
  CHECK(text.find("can0.lock") == std::string::npos);
  ::unsetenv("ECU_V3_TEST_SECRET");
}

void failing_and_hanging_commands_are_reported_and_bounded() {
  const auto dir = make_temp_dir();
  const auto failing = dir + "/ip-fail";
  write_script(failing, "exit 2\n");
  lx::IpCanLinkControl broken{"can0", failing};
  CHECK(broken.set_down() == rt::LinkStatus::command_failed);

  const auto hanging = dir + "/ip-hang";
  write_script(hanging, "exec sleep 30\n");
  lx::IpCanLinkControl stuck{"can0", hanging,
                             std::chrono::milliseconds{200}};
  const auto start = std::chrono::steady_clock::now();
  CHECK(stuck.set_up() == rt::LinkStatus::timeout);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{3});

  lx::IpCanLinkControl absent{"can0", dir + "/does-not-exist"};
  CHECK(absent.set_down() == rt::LinkStatus::command_failed);
}

void unsafe_interface_names_never_reach_the_command_line() {
  CHECK(lx::valid_interface_name("can0"));
  CHECK(lx::valid_interface_name("vcan_test.1"));
  CHECK(!lx::valid_interface_name(""));
  CHECK(!lx::valid_interface_name("-can0"));
  CHECK(!lx::valid_interface_name("can0;reboot"));
  CHECK(!lx::valid_interface_name("can0 up"));
  CHECK(!lx::valid_interface_name("abcdefghijklmnop"));  // > 15 chars

  const auto dir = make_temp_dir();
  const auto log = dir + "/ip.log";
  write_script(dir + "/ip", "echo called >> " + log + "\n");
  lx::IpCanLinkControl link{"can0;reboot", dir + "/ip"};
  CHECK(link.set_down() == rt::LinkStatus::command_failed);
  CHECK(read_file(log).empty());
}

void missing_interface_query_reports_not_found() {
  lx::IpCanLinkControl link{"ecuv3nolink0"};
  CHECK(link.query().status == rt::LinkStatus::not_found);
}

}  // namespace

int main() {
  lock_is_exclusive_between_owners();
  lock_survives_nothing_but_its_process_even_sigkill();
  lock_reports_errors_instead_of_pretending();
  ip_commands_match_the_v2_proven_command_lines();
  ip_child_runs_without_environment_or_inherited_lock();
  failing_and_hanging_commands_are_reported_and_bounded();
  unsafe_interface_names_never_reach_the_command_line();
  missing_interface_query_reports_not_found();
  return ecu::test::finish("linux_runtime_platform_tests");
}
