// Host stand-in: the panel is replaced by host_frame(), so the bus does nothing.
#pragma once
struct HostSPI {
  void begin(int, int, int, int) {}
};
extern HostSPI SPI;
