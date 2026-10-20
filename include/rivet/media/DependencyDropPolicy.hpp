#pragma once

#include "AccessUnitView.hpp"
#include <cstdint>

namespace rivet::media {

/// The action the queue should take for a specific Access Unit
enum class DropDecision : uint8_t {
  /// Keep the frame and queue it for sending
  Keep,

  /// Drop this frame because the network is congested, but keep the connection alive
  Drop,

  /// The network is completely frozen/dead. Disconnect the subscriber
  Disconnect
};

/// A smart backpressure policy that drops video frames based on their encoding importance rather
/// than dropping random packets. This ensures the video degrades gradually when the client's
/// connection is slow.
class DependencyDropPolicy {
  static constexpr uint8_t DISPOSABLE_DROP_THRESHOLD = 60; //< Drop B-frames if queue > 60% full
  static constexpr uint8_t REFERENCE_DROP_THRESHOLD  = 90; //< Drop P-frames if queue > 90% full
  static constexpr uint8_t CRITICAL_DROP_THRESHOLD   = 95; //< Drop I-frames if queue > 95% full

public:
  DependencyDropPolicy() noexcept = default;

  /// Evaluates whether a packet/frame should be kept or dropped based on priority and queue fill
  /// percentage
  [[nodiscard]] DropDecision evaluate(DependencyPriority prio,
      uint8_t                                            queue_fill_pct) const noexcept {
    if (queue_fill_pct >= 100) {
      return DropDecision::Disconnect;
    }

    if (prio == DependencyPriority::Disposable && queue_fill_pct >= DISPOSABLE_DROP_THRESHOLD) {
      return DropDecision::Drop;
    }

    if (prio == DependencyPriority::Reference && queue_fill_pct >= REFERENCE_DROP_THRESHOLD) {
      return DropDecision::Drop;
    }

    if (prio == DependencyPriority::Critical && queue_fill_pct >= CRITICAL_DROP_THRESHOLD) {
      return DropDecision::Drop;
    }

    return DropDecision::Keep;
  }

  /// Evaluates whether an Access Unit should be kept or dropped based on how full the client's
  /// queue is
  ///\param au The completely assembled Video frame
  ///\param queue_fill_pct How full the client's socket queue is (0 to 100)
  ///\return DropDecision
  [[nodiscard]] DropDecision evaluate(const AccessUnitView &au,
      uint8_t                                               queue_fill_pct) const noexcept {
    return evaluate(au.get_priority(), queue_fill_pct);
  }
};

} //< namespace rivet::media