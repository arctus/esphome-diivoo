#pragma once

// CMT2300A sub-GHz transceiver on the DIIVOO WG03 gateway, driven over its
// 3-wire SPI (shared SDIO line, separate CSB for registers and FCSB for the
// FIFO), bit-banged exactly like the reverse-engineered tcp_gateway firmware
// (Technerd-SG/hassio-diivoo2mqtt, AGPL-3.0). The register profile, the
// dummy status reads and every delay are copied from it on purpose: they are
// the sequence that is known to reach the valves, not a clean-room design.

#include <cstdint>

#include "esphome/core/hal.h"

namespace esphome {
namespace diivoo {

static const uint8_t PACKET_LENGTH = 32;

enum class Preamble : uint8_t {
  SHORT = 0,  // 40 units: ordinary replies
  LONG = 1,   // 300 units: long enough to land in a sleeping valve's wake slot
};

class CMT2300A {
 public:
  void set_pins(GPIOPin *sclk, GPIOPin *sdio, GPIOPin *csb, GPIOPin *fcsb) {
    this->sclk_ = sclk;
    this->sdio_ = sdio;
    this->csb_ = csb;
    this->fcsb_ = fcsb;
  }

  /// Soft reset, load the register profile and start listening on rx_channel.
  void init(uint8_t rx_channel);

  /// Standby, switch preamble and listen on rx_channel (the firmware's TUNE).
  void tune(uint8_t rx_channel, Preamble preamble);

  /// Send one frame on tx_channel, then return to RX on rx_channel. Blocks
  /// for roughly 60 to 210 ms, like the original firmware. Returns whether
  /// the chip reported TX_DONE.
  bool transmit(const uint8_t *frame, uint8_t len, uint8_t tx_channel, Preamble preamble, uint8_t rx_channel);

  /// Read the packet that raised the interrupt and re-arm RX on the current
  /// channel. Always returns PACKET_LENGTH bytes.
  void read_packet(uint8_t *out, int8_t *rssi_dbm, uint8_t *int_flags);

  Preamble preamble() const { return this->preamble_; }
  uint8_t rx_channel() const { return this->rx_channel_; }

 protected:
  void send_byte_(uint8_t data);
  uint8_t receive_byte_();
  void write_register_(uint8_t addr, uint8_t data);
  uint8_t read_register_(uint8_t addr);
  void write_fifo_(const uint8_t *buffer, uint8_t len);
  void read_fifo_(uint8_t *buffer, uint8_t len);
  void sdio_output_();
  void sdio_input_();

  void go_standby_();
  void go_rx_();
  void go_tx_();
  void go_sleep_();
  void prime_status_reads_();
  void prime_fifo_reads_();
  void clear_interrupts_(uint8_t clr1, uint8_t clr2);
  void enter_rx_(uint8_t channel);
  void apply_preamble_(Preamble preamble);

  GPIOPin *sclk_{nullptr};
  GPIOPin *sdio_{nullptr};
  GPIOPin *csb_{nullptr};
  GPIOPin *fcsb_{nullptr};
  Preamble preamble_{Preamble::SHORT};
  uint8_t rx_channel_{0};
};

}  // namespace diivoo
}  // namespace esphome
