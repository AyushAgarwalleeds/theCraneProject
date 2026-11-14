#pragma once

#include <Arduino.h>

class SerialLine {
public:
  template<class Handler>
  void poll(Handler handle) {
    for (int n = 0; n < 96 && Serial.available(); ++n) {
      const int c = Serial.read();
      if (c == '\r' || c == '\n') {
        if (!overflow_ && length_) {
          buffer_[length_] = 0;
          handle(String(buffer_));
        }
        length_ = 0;
        overflow_ = false;
      } else if (length_ < sizeof(buffer_) - 1) {
        buffer_[length_++] = char(c);
      } else {
        overflow_ = true;
      }
    }
  }
private:
  char buffer_[96]{};
  size_t length_ = 0;
  bool overflow_ = false;
};
