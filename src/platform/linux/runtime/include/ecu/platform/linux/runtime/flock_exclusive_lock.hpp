#pragma once

#include "ecu/runtime/can_link.hpp"

#include <string>

namespace ecu::platform::linux::runtime {

// System-wide ownership via flock(2) on a lock file.
//
// The kernel drops the lock when the owning process exits for any reason,
// including SIGKILL, so a crashed runtime never blocks the bench. The lock
// file itself carries no state and is never deleted.
class FlockExclusiveLock final : public ecu::runtime::IExclusiveLock {
 public:
  explicit FlockExclusiveLock(std::string path);
  ~FlockExclusiveLock() override;
  FlockExclusiveLock(const FlockExclusiveLock&) = delete;
  FlockExclusiveLock& operator=(const FlockExclusiveLock&) = delete;

  [[nodiscard]] ecu::runtime::LockStatus try_acquire() noexcept override;
  void release() noexcept override;
  [[nodiscard]] bool held() const noexcept override { return fd_ >= 0; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
  int fd_{-1};
};

}  // namespace ecu::platform::linux::runtime
