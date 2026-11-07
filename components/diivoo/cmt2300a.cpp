#include "cmt2300a.h"

namespace esphome {
namespace diivoo {

// Registers (CMT2300A datasheet names).
static const uint8_t REG_MODE_CTL = 0x60;
static const uint8_t REG_FREQ_CHNL = 0x63;
static const uint8_t REG_INT1_CTL = 0x66;
static const uint8_t REG_FIFO_CTL = 0x69;
static const uint8_t REG_INT_CLR1 = 0x6A;
static const uint8_t REG_INT_CLR2 = 0x6B;
static const uint8_t REG_FIFO_CLR = 0x6C;
static const uint8_t REG_INT_FLAG = 0x6D;
static const uint8_t REG_FIFO_FLAG = 0x6E;
static const uint8_t REG_RSSI_CODE = 0x6F;
static const uint8_t REG_RSSI_DBM = 0x70;
static const uint8_t REG_SOFT_RESET = 0x7F;

static const uint8_t GO_STBY = 0x02;
static const uint8_t GO_RX = 0x08;
static const uint8_t GO_SLEEP = 0x10;
static const uint8_t GO_TX = 0x40;

static const uint8_t FIFO_CLR_TX = 0x01;
static const uint8_t FIFO_CLR_RX = 0x02;
static const uint8_t TX_DONE_FLAG = 0x08;  // read from INT_CLR1
static const uint8_t TX_DONE_CLR = 0x04;
static const uint8_t FIFO_CTL_RX_SPI = 0x00;
static const uint8_t FIFO_CTL_TX_SPI = 0x05;

// Bit-bang timing, in microseconds unless named otherwise.
static const uint8_t T_CLK_HIGH_US = 3;
static const uint8_t T_CLK_LOW_US = 3;
static const uint8_t T_CS_SETUP_US = 2;
static const uint8_t T_INTER_BLOCK_US = 10;
static const uint8_t T_FIFO_PAUSE_US = 20;
static const uint16_t T_TX_FIFO_CLEAR_US = 500;
static const uint32_t T_POST_TX_DELAY_MS = 10;
static const uint32_t T_POST_SLEEP_MS = 40;
static const uint32_t T_POST_STBY_MS = 1;
static const uint32_t TX_TIMEOUT_MS = 150;
static const uint32_t TX_POLL_DELAY_MS = 17;

struct RegisterWrite {
  uint8_t addr;
  uint8_t value;
};

// The original gateway's profile, order unchanged. The radio leaves it with
// the short preamble (0x39/0x3A = 40 units) and a 32-byte fixed packet
// (0x46 = 0x1F).
static const RegisterWrite INIT_SEQUENCE[] = {
    {0x60, 0x02}, {0x61, 0x51}, {0x62, 0x20}, {0x0D, 0x00}, {0x6A, 0x00}, {0x6B, 0x00}, {0x00, 0x00}, {0x01, 0x66},
    {0x02, 0xEC}, {0x03, 0x1C}, {0x04, 0x70}, {0x05, 0x80}, {0x06, 0x14}, {0x07, 0x08}, {0x08, 0x91}, {0x09, 0x02},
    {0x0A, 0x02}, {0x0B, 0xD0}, {0x0C, 0xAE}, {0x0D, 0xE0}, {0x0E, 0x55}, {0x0F, 0x00}, {0x10, 0x00}, {0x11, 0xF4},
    {0x12, 0x10}, {0x13, 0xE2}, {0x14, 0x42}, {0x15, 0x20}, {0x16, 0x10}, {0x17, 0x01}, {0x18, 0x42}, {0x19, 0x39},
    {0x1A, 0xE9}, {0x1B, 0x1A}, {0x1C, 0x42}, {0x1D, 0x23}, {0x1E, 0x37}, {0x1F, 0x1A}, {0x20, 0xA6}, {0x21, 0xC9},
    {0x22, 0x20}, {0x23, 0x20}, {0x24, 0xE2}, {0x25, 0x35}, {0x26, 0x19}, {0x27, 0x10}, {0x28, 0x9F}, {0x29, 0x4B},
    {0x2A, 0x29}, {0x2B, 0x29}, {0x2C, 0xC0}, {0x2D, 0x14}, {0x2E, 0x05}, {0x2F, 0x53}, {0x30, 0x10}, {0x31, 0x00},
    {0x32, 0xB4}, {0x33, 0x00}, {0x34, 0x00}, {0x35, 0x01}, {0x36, 0x00}, {0x37, 0x00}, {0x38, 0x22}, {0x39, 0x28},
    {0x3A, 0x00}, {0x3B, 0xAA}, {0x3C, 0x06}, {0x3D, 0x00}, {0x3E, 0x00}, {0x3F, 0x00}, {0x40, 0x00}, {0x41, 0x5E},
    {0x42, 0x10}, {0x43, 0xE9}, {0x44, 0xF3}, {0x45, 0x00}, {0x46, 0x1F}, {0x47, 0x00}, {0x48, 0x00}, {0x49, 0x00},
    {0x4A, 0x00}, {0x4B, 0x00}, {0x4C, 0x01}, {0x4D, 0xA8}, {0x4E, 0xA8}, {0x4F, 0x60}, {0x50, 0xFF}, {0x51, 0x02},
    {0x52, 0x00}, {0x53, 0x1F}, {0x54, 0x10}, {0x55, 0x50}, {0x56, 0x34}, {0x57, 0x19}, {0x58, 0x00}, {0x59, 0x07},
    {0x5A, 0x50}, {0x5B, 0x00}, {0x5C, 0x4B}, {0x5D, 0x09}, {0x5E, 0x3F}, {0x5F, 0x6A}, {0x64, 0x2C}, {0x63, 0x00},
    {0x09, 0x02}, {0x65, 0x20}, {0x66, 0x0A}, {0x67, 0x06}, {0x6A, 0x07}, {0x6B, 0x3F}, {0x6D, 0x00}, {0x68, 0x3F},
    {0x0D, 0x00},
};

// ------------------------------------------------------------ bit-bang SPI

void CMT2300A::sdio_output_() { this->sdio_->pin_mode(gpio::FLAG_OUTPUT); }

void CMT2300A::sdio_input_() { this->sdio_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP); }

void CMT2300A::send_byte_(uint8_t data) {
  for (int8_t i = 7; i >= 0; i--) {
    this->sdio_->digital_write((data >> i) & 0x01);
    this->sclk_->digital_write(true);
    delayMicroseconds(T_CLK_HIGH_US);
    this->sclk_->digital_write(false);
    delayMicroseconds(T_CLK_LOW_US);
  }
}

uint8_t CMT2300A::receive_byte_() {
  uint8_t data = 0;
  for (int8_t i = 7; i >= 0; i--) {
    this->sclk_->digital_write(true);
    delayMicroseconds(T_CLK_HIGH_US);
    if (this->sdio_->digital_read())
      data |= (1 << i);
    this->sclk_->digital_write(false);
    delayMicroseconds(T_CLK_LOW_US);
  }
  return data;
}

void CMT2300A::write_register_(uint8_t addr, uint8_t data) {
  this->csb_->digital_write(false);
  delayMicroseconds(T_CS_SETUP_US);
  this->send_byte_(addr & 0x7F);
  this->send_byte_(data);
  this->csb_->digital_write(true);
  delayMicroseconds(T_INTER_BLOCK_US);
}

uint8_t CMT2300A::read_register_(uint8_t addr) {
  this->csb_->digital_write(false);
  delayMicroseconds(T_CS_SETUP_US);
  this->send_byte_(addr | 0x80);
  this->sdio_input_();
  uint8_t result = this->receive_byte_();
  this->sdio_output_();
  this->sdio_->digital_write(true);
  this->csb_->digital_write(true);
  delayMicroseconds(T_INTER_BLOCK_US);
  return result;
}

void CMT2300A::read_fifo_(uint8_t *buffer, uint8_t len) {
  this->sclk_->digital_write(false);
  this->sdio_input_();
  for (uint8_t i = 0; i < len; i++) {
    this->fcsb_->digital_write(false);
    delayMicroseconds(T_CS_SETUP_US);
    buffer[i] = this->receive_byte_();
    this->fcsb_->digital_write(true);
    delayMicroseconds(T_FIFO_PAUSE_US);
  }
  this->sdio_output_();
  this->sdio_->digital_write(true);
}

void CMT2300A::write_fifo_(const uint8_t *buffer, uint8_t len) {
  this->sdio_output_();
  this->sclk_->digital_write(false);
  for (uint8_t i = 0; i < len; i++) {
    this->fcsb_->digital_write(false);
    delayMicroseconds(T_CS_SETUP_US);
    this->send_byte_(buffer[i]);
    this->fcsb_->digital_write(true);
    delayMicroseconds(T_FIFO_PAUSE_US);
  }
}

// ------------------------------------------------------------ radio helpers

void CMT2300A::go_standby_() {
  this->write_register_(REG_MODE_CTL, GO_STBY);
  delayMicroseconds(100);
}
void CMT2300A::go_rx_() { this->write_register_(REG_MODE_CTL, GO_RX); }
void CMT2300A::go_tx_() { this->write_register_(REG_MODE_CTL, GO_TX); }
void CMT2300A::go_sleep_() { this->write_register_(REG_MODE_CTL, GO_SLEEP); }

// Dummy reads kept from the original sequence.
void CMT2300A::prime_status_reads_() {
  (void) this->read_register_(REG_INT1_CTL);
  (void) this->read_register_(REG_INT_FLAG);
  (void) this->read_register_(REG_INT_CLR1);
}

void CMT2300A::prime_fifo_reads_() {
  (void) this->read_register_(REG_FIFO_CTL);
  (void) this->read_register_(REG_FIFO_FLAG);
}

void CMT2300A::clear_interrupts_(uint8_t clr1, uint8_t clr2) {
  this->write_register_(REG_INT_CLR1, clr1);
  this->write_register_(REG_INT_CLR2, clr2);
}

// The firmware's restoreRxAfterTx().
void CMT2300A::enter_rx_(uint8_t channel) {
  this->prime_status_reads_();
  this->clear_interrupts_(0x00, 0x00);
  this->go_standby_();
  this->write_register_(REG_FREQ_CHNL, channel);
  this->prime_status_reads_();
  this->clear_interrupts_(0x00, 0x00);
  this->prime_fifo_reads_();
  this->write_register_(REG_FIFO_CTL, FIFO_CTL_RX_SPI);
  this->write_register_(REG_FIFO_CLR, FIFO_CLR_RX);
  this->go_rx_();
  this->rx_channel_ = channel;
}

// Chip must be in standby.
void CMT2300A::apply_preamble_(Preamble preamble) {
  if (preamble == this->preamble_)
    return;
  if (preamble == Preamble::LONG) {
    this->write_register_(0x39, 0x2C);  // TX_PREAM_SIZE = 0x012C = 300
    this->write_register_(0x3A, 0x01);
  } else {
    this->write_register_(0x39, 0x28);  // TX_PREAM_SIZE = 0x0028 = 40
    this->write_register_(0x3A, 0x00);
  }
  this->write_register_(0x3B, 0xAA);  // PREAM_VALUE
  this->preamble_ = preamble;
}

// ------------------------------------------------------------ public API

void CMT2300A::init(uint8_t rx_channel) {
  this->csb_->digital_write(true);
  this->fcsb_->digital_write(true);
  this->sdio_output_();
  this->sdio_->digital_write(true);
  this->sclk_->digital_write(false);
  delay(100);

  this->write_register_(REG_SOFT_RESET, 0xFF);
  delay(30);
  for (const auto &reg : INIT_SEQUENCE)
    this->write_register_(reg.addr, reg.value);
  this->preamble_ = Preamble::SHORT;

  this->go_standby_();
  this->write_register_(REG_FREQ_CHNL, rx_channel);
  this->go_rx_();
  this->rx_channel_ = rx_channel;
}

void CMT2300A::tune(uint8_t rx_channel, Preamble preamble) {
  if (rx_channel == this->rx_channel_ && preamble == this->preamble_)
    return;
  this->go_standby_();
  this->apply_preamble_(preamble);
  this->enter_rx_(rx_channel);
}

bool CMT2300A::transmit(const uint8_t *frame, uint8_t len, uint8_t tx_channel, Preamble preamble,
                        uint8_t rx_channel) {
  this->go_standby_();
  this->apply_preamble_(preamble);
  this->write_register_(REG_FREQ_CHNL, tx_channel);

  this->prime_status_reads_();
  this->clear_interrupts_(0x00, 0x00);

  (void) this->read_register_(REG_FIFO_CTL);
  this->write_register_(REG_FIFO_CTL, FIFO_CTL_TX_SPI);
  (void) this->read_register_(REG_FIFO_FLAG);
  this->write_register_(REG_FIFO_CLR, FIFO_CLR_TX);
  delayMicroseconds(T_TX_FIFO_CLEAR_US);

  this->write_fifo_(frame, len);
  this->go_tx_();

  bool done = false;
  const uint32_t start = millis();
  while (millis() - start < TX_TIMEOUT_MS) {
    delay(TX_POLL_DELAY_MS);
    if (this->read_register_(REG_INT_CLR1) & TX_DONE_FLAG) {
      done = true;
      break;
    }
  }

  // Cleanup / calibration sequence as in the original.
  delay(T_POST_TX_DELAY_MS);
  this->prime_status_reads_();
  this->clear_interrupts_(TX_DONE_CLR, 0x00);
  this->go_sleep_();
  delay(T_POST_SLEEP_MS);
  this->go_standby_();
  delay(T_POST_STBY_MS);

  this->enter_rx_(rx_channel);
  return done;
}

void CMT2300A::read_packet(uint8_t *out, int8_t *rssi_dbm, uint8_t *int_flags) {
  const uint8_t flags = this->read_register_(REG_INT_FLAG);
  (void) this->read_register_(REG_RSSI_CODE);
  *rssi_dbm = static_cast<int8_t>(this->read_register_(REG_RSSI_DBM) - 128);
  *int_flags = flags;

  this->go_standby_();
  this->read_fifo_(out, PACKET_LENGTH);

  this->write_register_(REG_INT_CLR1, 0x00);
  this->write_register_(REG_INT_CLR2, flags);
  this->write_register_(REG_FIFO_CLR, FIFO_CLR_RX);
  this->go_rx_();
}

}  // namespace diivoo
}  // namespace esphome
