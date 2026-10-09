#include "ecu/platform/linux/runtime/flock_exclusive_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace ecu::platform::linux::runtime {

using ecu::runtime::LockStatus;

FlockExclusiveLock::FlockExclusiveLock(std::string path)
    : path_(std::move(path)) {}

FlockExclusiveLock::~FlockExclusiveLock() { release(); }

LockStatus FlockExclusiveLock::try_acquire() noexcept {
  if (fd_ >= 0) return LockStatus::acquired;
  if (path_.empty()) return LockStatus::error;
  const int fd = ::open(path_.c_str(),
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return LockStatus::error;
  int rc = 0;
  do {
    rc = ::flock(fd, LOCK_EX | LOCK_NB);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    const bool busy = errno == EWOULDBLOCK;
    ::close(fd);
    return busy ? LockStatus::busy : LockStatus::error;
  }
  fd_ = fd;
  return LockStatus::acquired;
}

void FlockExclusiveLock::release() noexcept {
  if (fd_ < 0) return;
  (void)::flock(fd_, LOCK_UN);
  ::close(fd_);
  fd_ = -1;
}

}  // namespace ecu::platform::linux::runtime
