#pragma once

#include "SocketUtils.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define RIVET_POLLER_KQUEUE 1
#include <sys/event.h>
#include <sys/time.h>
#elif defined(__linux__)
#define RIVET_POLLER_EPOLL 1
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#error "Unsupported platform for EventPoller (requires kqueue or epoll)"
#endif

namespace rivet::net {

/// Event bitmask flags returned by EventPoller.
enum PollFlags : uint32_t {
  kPollNone  = 0,
  kPollRead  = 1u << 0,
  kPollWrite = 1u << 1,
  kPollError = 1u << 2,
  kPollHup   = 1u << 3,
  kPollWake  = 1u << 4,
};

struct PollEvent {
  uint64_t tag{0};
  int      fd{-1};
  uint32_t flags{kPollNone};
};

/// Zero-allocation OS event demultiplexer (`kqueue` + `EVFILT_USER` on macOS/BSD,
/// `epoll` + `eventfd` on Linux) with coalesced cross-thread wakeup and cached
/// monotonic timestamp per wakeup.
class EventPoller {
  static constexpr uintptr_t kWakeIdent = 0x52495654ULL; //< "RIVT"
  static constexpr uint64_t  kWakeTag   = ~uint64_t{0};

  int poll_fd_{-1};
#if defined(RIVET_POLLER_EPOLL)
  int wake_fd_{-1};
#endif
  std::atomic<bool> wake_pending_{false};
  uint64_t          cached_now_ns_{0};

  [[nodiscard]] static uint64_t read_clock_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

public:
  EventPoller() noexcept
      : cached_now_ns_(read_clock_ns()) {
#if defined(RIVET_POLLER_KQUEUE)
    poll_fd_ = ::kqueue();
    if (poll_fd_ >= 0) {
      (void)set_cloexec(poll_fd_);
      struct kevent kev{};
      EV_SET(&kev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
      (void)::kevent(poll_fd_, &kev, 1, nullptr, 0, nullptr);
    }
#elif defined(RIVET_POLLER_EPOLL)
    poll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (poll_fd_ >= 0 && wake_fd_ >= 0) {
      struct epoll_event ev{};
      ev.events   = EPOLLIN;
      ev.data.u64 = kWakeTag;
      (void)::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
    }
#endif
  }

  ~EventPoller() noexcept { close(); }

  EventPoller(const EventPoller &)            = delete;
  EventPoller &operator=(const EventPoller &) = delete;

  void close() noexcept {
#if defined(RIVET_POLLER_EPOLL)
    if (wake_fd_ >= 0) {
      ::close(wake_fd_);
      wake_fd_ = -1;
    }
#endif
    if (poll_fd_ >= 0) {
      ::close(poll_fd_);
      poll_fd_ = -1;
    }
  }

  [[nodiscard]] bool is_valid() const noexcept {
#if defined(RIVET_POLLER_EPOLL)
    return poll_fd_ >= 0 && wake_fd_ >= 0;
#else
    return poll_fd_ >= 0;
#endif
  }

  [[nodiscard]] uint64_t now_ns() const noexcept { return cached_now_ns_; }

  uint64_t refresh_now_ns() noexcept {
    cached_now_ns_ = read_clock_ns();
    return cached_now_ns_;
  }

  bool add_or_mod(int fd, uint32_t events, uint64_t tag) noexcept {
    if (poll_fd_ < 0 || fd < 0) {
      return false;
    }
#if defined(RIVET_POLLER_KQUEUE)
    struct kevent changes[2]{};
    int           n = 0;
    if ((events & kPollRead) != 0) {
      EV_SET(&changes[n++],
          static_cast<uintptr_t>(fd),
          EVFILT_READ,
          EV_ADD | EV_ENABLE,
          0,
          0,
          reinterpret_cast<void *>(static_cast<uintptr_t>(tag)));
    } else {
      struct kevent del{};
      EV_SET(&del, static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
      (void)::kevent(poll_fd_, &del, 1, nullptr, 0, nullptr);
    }
    if ((events & kPollWrite) != 0) {
      EV_SET(&changes[n++],
          static_cast<uintptr_t>(fd),
          EVFILT_WRITE,
          EV_ADD | EV_ENABLE,
          0,
          0,
          reinterpret_cast<void *>(static_cast<uintptr_t>(tag)));
    } else {
      struct kevent del{};
      EV_SET(&del, static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
      (void)::kevent(poll_fd_, &del, 1, nullptr, 0, nullptr);
    }
    if (n == 0) {
      return true;
    }
    return ::kevent(poll_fd_, changes, n, nullptr, 0, nullptr) >= 0;
#elif defined(RIVET_POLLER_EPOLL)
    struct epoll_event ev{};
    if ((events & kPollRead) != 0) {
      ev.events |= EPOLLIN | EPOLLRDHUP;
    }
    if ((events & kPollWrite) != 0) {
      ev.events |= EPOLLOUT;
    }
    ev.data.u64 = tag;
    if (::epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
      return true;
    }
    if (errno == ENOENT) {
      return ::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }
    return false;
#endif
  }

  void remove(int fd) noexcept {
    if (poll_fd_ < 0 || fd < 0) {
      return;
    }
#if defined(RIVET_POLLER_KQUEUE)
    struct kevent del[2]{};
    EV_SET(&del[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&del[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void)::kevent(poll_fd_, &del[0], 1, nullptr, 0, nullptr);
    (void)::kevent(poll_fd_, &del[1], 1, nullptr, 0, nullptr);
#elif defined(RIVET_POLLER_EPOLL)
    (void)::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
  }

  /// Thread-safe coalesced wakeup: at most 1 syscall per sleep/wake cycle.
  void wake() noexcept {
    if (poll_fd_ < 0) {
      return;
    }
    if (wake_pending_.exchange(true, std::memory_order_acq_rel)) {
      return; //< Wake already armed!
    }
#if defined(RIVET_POLLER_KQUEUE)
    struct kevent kev{};
    EV_SET(&kev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)::kevent(poll_fd_, &kev, 1, nullptr, 0, nullptr);
#elif defined(RIVET_POLLER_EPOLL)
    if (wake_fd_ >= 0) {
      const uint64_t one = 1;
      (void)::write(wake_fd_, &one, sizeof(one));
    }
#endif
  }

  /// Waits up to `timeout_ms` (`< 0` = infinite, `0` = non-blocking poll) and fills `out`.
  [[nodiscard]] size_t wait(std::span<PollEvent> out, int timeout_ms) noexcept {
    if (poll_fd_ < 0 || out.empty()) {
      cached_now_ns_ = read_clock_ns();
      return 0;
    }

    static constexpr size_t kMaxBatch = 128;
    const size_t            cap       = (out.size() < kMaxBatch) ? out.size() : kMaxBatch;

#if defined(RIVET_POLLER_KQUEUE)
    std::array<struct kevent, kMaxBatch> raw{};
    struct timespec                      ts{};
    const struct timespec               *tsp = nullptr;
    if (timeout_ms >= 0) {
      ts.tv_sec  = timeout_ms / 1000;
      ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L;
      tsp        = &ts;
    }

    const int n    = ::kevent(poll_fd_, nullptr, 0, raw.data(), static_cast<int>(cap), tsp);
    cached_now_ns_ = read_clock_ns();
    if (n <= 0) {
      return 0;
    }

    size_t count = 0;
    for (int i = 0; i < n; ++i) {
      const auto &ev = raw[static_cast<size_t>(i)];
      if (ev.filter == EVFILT_USER && ev.ident == kWakeIdent) {
        wake_pending_.store(false, std::memory_order_release);
        out[count++] = PollEvent{kWakeTag, -1, kPollWake};
        continue;
      }

      uint32_t flags = kPollNone;
      if (ev.filter == EVFILT_READ) {
        flags |= kPollRead;
      } else if (ev.filter == EVFILT_WRITE) {
        flags |= kPollWrite;
      }
      if ((ev.flags & EV_EOF) != 0) {
        flags |= kPollHup;
      }
      if ((ev.flags & EV_ERROR) != 0) {
        flags |= kPollError;
      }
      out[count++] = PollEvent{
          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev.udata)),
          static_cast<int>(ev.ident),
          flags,
      };
    }
    return count;

#elif defined(RIVET_POLLER_EPOLL)
    std::array<struct epoll_event, kMaxBatch> raw{};
    const int n    = ::epoll_wait(poll_fd_, raw.data(), static_cast<int>(cap), timeout_ms);
    cached_now_ns_ = read_clock_ns();
    if (n <= 0) {
      return 0;
    }

    size_t count = 0;
    for (int i = 0; i < n; ++i) {
      const auto &ev = raw[static_cast<size_t>(i)];
      if (ev.data.u64 == kWakeTag) {
        uint64_t val = 0;
        while (::read(wake_fd_, &val, sizeof(val)) > 0) {
        }
        wake_pending_.store(false, std::memory_order_release);
        out[count++] = PollEvent{kWakeTag, -1, kPollWake};
        continue;
      }

      uint32_t flags = kPollNone;
      if ((ev.events & EPOLLIN) != 0) {
        flags |= kPollRead;
      }
      if ((ev.events & EPOLLOUT) != 0) {
        flags |= kPollWrite;
      }
      if ((ev.events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
        flags |= kPollHup;
      }
      if ((ev.events & EPOLLERR) != 0) {
        flags |= kPollError;
      }
      out[count++] = PollEvent{ev.data.u64, -1, flags};
    }
    return count;
#endif
  }
};

} //< namespace rivet::net
