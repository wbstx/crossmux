#pragma once

#include <cstdint>

// No trusted clock, so exports omit the "Added on" stamp.
class HalClock {
 public:
  bool isAvailable() const { return false; }
  bool getDateTime(uint16_t&, uint8_t&, uint8_t&, uint8_t&, uint8_t&) const { return false; }
};

inline HalClock halClock;
