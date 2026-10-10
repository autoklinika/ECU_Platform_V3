// Linux tests of the system-wide lock and of the iproute2 link control.
// No CAN hardware is needed: the ip program is replaced by a recording script.

#include "ecu/platform/linux/runtime/flock_exclusive_lock.hpp"
#include "ecu/platform/linux/runtime/ip_can_link_control.hpp"
#include "ecu/platform/linux/runtime/netlink_can_link_query.hpp"
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
#include <vector>
#include <cstring>
#include <linux/can/netlink.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

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

void ip_command_lines_are_exact() {
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


// ---- rtnetlink parsing on synthetic kernel messages -----------------------

class LinkMessage {
 public:
  explicit LinkMessage(unsigned flags) {
    bytes_.resize(NLMSG_LENGTH(sizeof(ifinfomsg)));
    ifinfomsg info{};
    info.ifi_flags = flags;
    std::memcpy(bytes_.data() + NLMSG_LENGTH(0), &info, sizeof(info));
  }
  // Appends attribute at the current nesting level; returns its offset.
  std::size_t attr(unsigned short type, const void* data, std::size_t size) {
    align();
    const std::size_t at = bytes_.size();
    rtattr header{};
    header.rta_type = type;
    header.rta_len = static_cast<unsigned short>(RTA_LENGTH(size));
    bytes_.resize(at + RTA_LENGTH(size));
    std::memcpy(bytes_.data() + at, &header, sizeof(header));
    if (size) std::memcpy(bytes_.data() + at + RTA_LENGTH(0), data, size);
    return at;
  }
  std::size_t open(unsigned short type) { return attr(type, nullptr, 0); }
  void close(std::size_t at) {
    rtattr header{};
    std::memcpy(&header, bytes_.data() + at, sizeof(header));
    header.rta_len = static_cast<unsigned short>(bytes_.size() - at);
    std::memcpy(bytes_.data() + at, &header, sizeof(header));
  }
  std::vector<unsigned char> finish() {
    align();
    nlmsghdr header{};
    header.nlmsg_type = RTM_NEWLINK;
    header.nlmsg_len = static_cast<unsigned>(bytes_.size());
    std::memcpy(bytes_.data(), &header, sizeof(header));
    return bytes_;
  }

 private:
  void align() { bytes_.resize(NLMSG_ALIGN(bytes_.size())); }
  std::vector<unsigned char> bytes_;
};

std::vector<unsigned char> can_message(unsigned flags, unsigned bitrate,
                                       unsigned ctrl_flags, unsigned state,
                                       bool counters) {
  LinkMessage m{flags};
  m.attr(IFLA_IFNAME, "can0", 5);
  const auto info = m.open(IFLA_LINKINFO);
  m.attr(IFLA_INFO_KIND, "can", 4);
  const auto data = m.open(IFLA_INFO_DATA);
  can_bittiming timing{};
  timing.bitrate = bitrate;
  m.attr(IFLA_CAN_BITTIMING, &timing, sizeof(timing));
  can_ctrlmode mode{};
  mode.mask = 0xFFFFFFFFU;
  mode.flags = ctrl_flags;
  m.attr(IFLA_CAN_CTRLMODE, &mode, sizeof(mode));
  m.attr(IFLA_CAN_STATE, &state, sizeof(state));
  if (counters) {
    can_berr_counter c{};
    c.txerr = 17;
    c.rxerr = 130;
    m.attr(IFLA_CAN_BERR_COUNTER, &c, sizeof(c));
  }
  m.close(data);
  m.close(info);
  return m.finish();
}

void netlink_parser_reads_can_link_state() {
  const auto up = can_message(IFF_UP, 500000U, CAN_CTRLMODE_LISTENONLY,
                              CAN_STATE_ERROR_PASSIVE, true);
  const auto q = lx::parse_can_link_message(up.data(), up.size());
  CHECK(q.status == rt::LinkStatus::ok);
  CHECK(q.state.present && q.state.up);
  CHECK(q.state.bitrate == 500000U);
  CHECK(q.state.listen_only && !q.state.fd_enabled);
  CHECK(q.state.bus_state == rt::CanBusState::error_passive && !q.state.bus_off);
  CHECK(q.state.has_error_counters && q.state.tx_errors == 17U &&
        q.state.rx_errors == 130U);

  const auto off = can_message(IFF_UP, 250000U, CAN_CTRLMODE_FD,
                               CAN_STATE_BUS_OFF, false);
  const auto b = lx::parse_can_link_message(off.data(), off.size());
  CHECK(b.status == rt::LinkStatus::ok);
  CHECK(b.state.bus_off && b.state.fd_enabled && !b.state.listen_only);
  CHECK(!b.state.has_error_counters);

  const auto down = can_message(0U, 500000U, 0U, CAN_STATE_STOPPED, false);
  const auto d = lx::parse_can_link_message(down.data(), down.size());
  CHECK(d.status == rt::LinkStatus::ok && !d.state.up);
  CHECK(d.state.bus_state == rt::CanBusState::stopped);
}

void netlink_parser_rejects_non_can_and_malformed_messages() {
  LinkMessage eth{IFF_UP};
  const auto info = eth.open(IFLA_LINKINFO);
  eth.attr(IFLA_INFO_KIND, "veth", 5);
  eth.close(info);
  const auto veth = eth.finish();
  CHECK(lx::parse_can_link_message(veth.data(), veth.size()).status ==
        rt::LinkStatus::not_can);

  LinkMessage plain{IFF_UP};  // no IFLA_LINKINFO at all (e.g. loopback)
  const auto lo = plain.finish();
  CHECK(lx::parse_can_link_message(lo.data(), lo.size()).status ==
        rt::LinkStatus::not_can);

  auto good = can_message(IFF_UP, 500000U, 0U, CAN_STATE_ERROR_ACTIVE, true);
  CHECK(lx::parse_can_link_message(good.data(), 8).status ==
        rt::LinkStatus::io_error);  // truncated header
  CHECK(lx::parse_can_link_message(nullptr, 0).status ==
        rt::LinkStatus::io_error);
  // Corrupt the outer attribute length so it points past the message.
  const std::size_t first_attr = NLMSG_LENGTH(NLMSG_ALIGN(sizeof(ifinfomsg)));
  rtattr bad{};
  std::memcpy(&bad, good.data() + first_attr, sizeof(bad));
  bad.rta_len = 0xFFFF;
  std::memcpy(good.data() + first_attr, &bad, sizeof(bad));
  const auto corrupt = lx::parse_can_link_message(good.data(), good.size());
  CHECK(corrupt.status == rt::LinkStatus::not_can);  // nothing trusted
}

void netlink_query_against_the_real_kernel() {
  CHECK(lx::query_can_link("ecuv3nolink0").status == rt::LinkStatus::not_found);
  const auto lo = lx::query_can_link("lo");
  CHECK(lo.status == rt::LinkStatus::not_can ||
        lo.status == rt::LinkStatus::not_found);  // containers may lack lo
  CHECK(lx::query_can_link("").status == rt::LinkStatus::not_found);
}

}  // namespace

int main() {
  lock_is_exclusive_between_owners();
  lock_survives_nothing_but_its_process_even_sigkill();
  lock_reports_errors_instead_of_pretending();
  ip_command_lines_are_exact();
  ip_child_runs_without_environment_or_inherited_lock();
  failing_and_hanging_commands_are_reported_and_bounded();
  unsafe_interface_names_never_reach_the_command_line();
  missing_interface_query_reports_not_found();
  netlink_parser_reads_can_link_state();
  netlink_parser_rejects_non_can_and_malformed_messages();
  netlink_query_against_the_real_kernel();
  return ecu::test::finish("linux_runtime_platform_tests");
}
