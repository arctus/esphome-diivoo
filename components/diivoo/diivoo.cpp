#include "diivoo.h"

#include "esphome/core/log.h"

namespace esphome {
namespace diivoo {

static const char *const TAG = "diivoo";

// Frame layout: 0x51, target id (LE32), sender id (LE32), seq, cmd, payload
// length, payload, zero padding to 32 bytes. The CRC is the radio's job.
static const uint8_t FRAME_HEADER = 0x51;
static const uint8_t FRAME_OVERHEAD = 12;
static const uint8_t MAX_PAYLOAD = PACKET_LENGTH - FRAME_OVERHEAD;

// Commands, valve -> hub.
static const uint8_t CMD_JOIN = 0x01;
static const uint8_t CMD_STATUS = 0x02;
static const uint8_t CMD_EVENT = 0x04;
static const uint8_t CMD_PARAMETER_REQUEST = 0x05;
static const uint8_t CMD_SCHEDULE_REQUEST = 0x06;
static const uint8_t CMD_SIMPLE_ACK = 0xA0;
static const uint8_t CMD_ACTION_ACK = 0xA1;
// Commands, hub -> valve.
static const uint8_t CMD_PING = 0x20;
static const uint8_t CMD_ACTION = 0x21;
static const uint8_t CMD_JOIN_RESPONSE = 0x81;
static const uint8_t CMD_ACK = 0x82;
static const uint8_t CMD_EVENT_ACK = 0x84;
static const uint8_t CMD_PARAMETERS = 0x85;
static const uint8_t CMD_SCHEDULES = 0x86;

static const uint8_t IDLE_RX_CHANNEL = 0;
static const uint8_t PAIRING_CHANNEL = 4;
static const uint8_t CONTEXT_BYTE = 0x02;

// Action: two long-preamble tries, then the "legacy spam" fallback of up to
// twelve short ones - the same ladder the add-on climbs.
static const uint8_t ACTION_ATTEMPTS = 2;
static const uint32_t ACTION_WAIT_MS = 350;
static const uint32_t ACTION_RETRY_GAP_MS = 120;
static const uint8_t FALLBACK_ATTEMPTS = 12;
static const uint32_t FALLBACK_WAIT_MS = 180;
static const uint32_t FALLBACK_RETRY_GAP_MS = 35;
static const uint8_t PING_ATTEMPTS = 3;
static const uint32_t PING_LISTEN_MS = 1800;
static const uint32_t JOIN_RESPONSE_GAP_MS = 70;

static const uint32_t DUPLICATE_WINDOW_MS = 5000;
static const uint32_t ZONE_TICK_MS = 5000;
static const uint32_t WATCHDOG_TICK_MS = 60000;
static const uint32_t FIRST_WATCHDOG_MS = 20000;
static const uint32_t PING_AFTER_SILENCE_MS = 5UL * 60 * 60 * 1000;
static const uint32_t PING_MIN_INTERVAL_MS = 10UL * 60 * 1000;
static const uint32_t OFFLINE_AFTER_MS = 12UL * 60 * 60 * 1000;
static const uint32_t FINISHED_GRACE_S = 60;
static const uint16_t MAX_DURATION_S = 0xFFFF;

static uint32_t read_le32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static uint16_t read_le16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

static bool is_running(uint8_t state_byte) { return (state_byte & 0x03) != 0; }

static uint8_t zones_for_hardware(uint16_t hardware_id) {
  switch (hardware_id) {
    case 0x0D1F:
      return 1;
    case 0x0E20:
      return 2;
    case 0x0F25:
      return 3;
    case 0x1026:
      return 4;
    default:
      return 0;
  }
}

static const char *model_for_hardware(uint16_t hardware_id) {
  switch (hardware_id) {
    case 0x0D1F:
      return "WT-07W";
    case 0x0E20:
      return "WT-09W";
    case 0x0F25:
      return "WT-11W";
    case 0x1026:
      return "WT-13W";
    default:
      return "Unknown";
  }
}

static uint32_t hash_frame(const uint8_t *frame) {
  uint32_t hash = 2166136261UL;
  for (uint8_t i = 0; i < PACKET_LENGTH; i++) {
    hash *= 16777619UL;
    hash ^= frame[i];
  }
  return hash;
}

// ======================================================================
// Entities
// ======================================================================

void DiivooZoneValve::setup() {
  this->position = valve::VALVE_CLOSED;
  this->current_operation = valve::VALVE_OPERATION_IDLE;
  this->publish_state(false);
}

void DiivooZoneValve::dump_config() { LOG_VALVE("", "DIIVOO zone valve", this); }

valve::ValveTraits DiivooZoneValve::get_traits() {
  valve::ValveTraits traits;
  traits.set_is_assumed_state(false);
  traits.set_supports_position(false);
  traits.set_supports_toggle(true);
  traits.set_supports_stop(false);
  return traits;
}

void DiivooZoneValve::control(const valve::ValveCall &call) {
  bool open;
  if (call.get_toggle().has_value()) {
    open = !this->is_fully_open();
  } else if (call.get_position().has_value()) {
    open = *call.get_position() > 0.5f;
  } else {
    return;
  }
  if (this->target_ == nullptr || !this->target_->hub()->request_zone(this->target_, this->zone_, open)) {
    ESP_LOGW(TAG, "Zone %u: valve is not paired, ignoring %s", this->zone_, open ? "open" : "close");
    this->publish_state(false);
    return;
  }
  this->current_operation = open ? valve::VALVE_OPERATION_OPENING : valve::VALVE_OPERATION_CLOSING;
  this->publish_state(false);
}

void DiivooZoneValve::report(bool open, bool busy) {
  this->position = open ? valve::VALVE_OPEN : valve::VALVE_CLOSED;
  if (!busy)
    this->current_operation = valve::VALVE_OPERATION_IDLE;
  this->publish_state(false);
}

void DiivooDurationNumber::setup() {
  float value = this->initial_value_;
  this->pref_ = this->make_entity_preference<float>();
  float stored;
  if (this->pref_.load(&stored) && std::isfinite(stored))
    value = stored;
  this->publish_state(value);
}

void DiivooDurationNumber::dump_config() { LOG_NUMBER("", "DIIVOO zone duration", this); }

void DiivooDurationNumber::control(float value) {
  this->pref_.save(&value);
  this->publish_state(value);
}

void DiivooPairingSwitch::write_state(bool state) { this->parent_->set_pairing_mode(state); }

void DiivooValveButton::press_action() {
  if (this->target_ == nullptr)
    return;
  if (this->action_ == ValveButtonAction::UNPAIR) {
    this->target_->hub()->unpair(this->target_);
  } else {
    this->target_->hub()->request_refresh(this->target_);
  }
}

// ======================================================================
// Valve slot
// ======================================================================

bool DiivooValve::zone_running(uint8_t zone) const {
  if (zone < 1 || zone > MAX_ZONES)
    return false;
  return this->zones_[zone - 1].running;
}

uint32_t DiivooValve::zone_duration_seconds(uint8_t zone) const {
  if (zone < 1 || zone > MAX_ZONES)
    return 600;
  const DiivooDurationNumber *number = this->zones_[zone - 1].duration;
  if (number == nullptr || !number->has_state() || !std::isfinite(number->state) || number->state <= 0)
    return 600;
  const uint32_t seconds = static_cast<uint32_t>(number->state * 60.0f);
  return seconds > MAX_DURATION_S ? MAX_DURATION_S : seconds;
}

uint8_t DiivooValve::downlink_channel() const {
  if (this->record_.channel_code == 0)
    return PAIRING_CHANNEL;
  return this->record_.channel_code - 1;
}

uint8_t DiivooValve::normalize_zone(uint8_t raw) const {
  if (raw >= 1 && raw <= this->zone_count_)
    return raw;
  if (raw == 0 && this->zone_count_ == 1)
    return 1;
  return 0;
}

void DiivooValve::load() {
  this->pref_ = global_preferences->make_preference<ValveRecord>(this->pref_hash_, true);
  ValveRecord stored;
  if (this->pref_.load(&stored) && stored.bound == 1)
    this->record_ = stored;
}

void DiivooValve::save() { this->pref_.save(&this->record_); }

void DiivooValve::publish_identity() {
  if (this->valve_id_sensor_ != nullptr) {
    this->valve_id_sensor_->publish_state(this->is_bound() ? str_sprintf("%u", (unsigned) this->record_.valve_id)
                                                           : std::string("Not paired"));
  }
  if (this->model_sensor_ != nullptr) {
    this->model_sensor_->publish_state(this->is_bound() ? model_for_hardware(this->record_.hardware_id) : "");
  }
}

// ======================================================================
// Hub: setup and loop
// ======================================================================

void DiivooHub::add_valve(DiivooValve *valve) {
  valve->hub_ = this;
  this->valves_.push_back(valve);
}

void IRAM_ATTR DiivooHub::gpio_intr(DiivooHub *arg) { arg->pending_irqs_ = arg->pending_irqs_ + 1; }

void DiivooHub::setup() {
  if (this->hub_id_ == 0) {
    uint8_t mac[6];
    get_mac_address_raw(mac);
    this->hub_id_ = read_le32(mac + 2);
    if (this->hub_id_ == 0)
      this->hub_id_ = 0x00D11F00;
  }

  for (auto *valve : this->valves_) {
    valve->load();
    valve->publish_identity();
  }

  this->sclk_pin_->setup();
  this->sdio_pin_->setup();
  this->csb_pin_->setup();
  this->fcsb_pin_->setup();
  if (this->power_pin_ != nullptr) {
    this->power_pin_->setup();
    this->power_pin_->digital_write(true);  // radio supply on (inverted in YAML)
  }
  this->irq_pin_->setup();

  this->radio_.set_pins(this->sclk_pin_, this->sdio_pin_, this->csb_pin_, this->fcsb_pin_);
  this->radio_.init(IDLE_RX_CHANNEL);
  this->irq_pin_->attach_interrupt(DiivooHub::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);

  if (this->pairing_switch_ != nullptr)
    this->pairing_switch_->publish_state(false);

  this->boot_at_ = millis();
  this->last_watchdog_ = this->boot_at_;
}

void DiivooHub::dump_config() {
  ESP_LOGCONFIG(TAG, "DIIVOO gateway:");
  ESP_LOGCONFIG(TAG, "  Hub ID: %u (0x%08X)", (unsigned) this->hub_id_, (unsigned) this->hub_id_);
  LOG_PIN("  SCLK pin: ", this->sclk_pin_);
  LOG_PIN("  SDIO pin: ", this->sdio_pin_);
  LOG_PIN("  CSB pin: ", this->csb_pin_);
  LOG_PIN("  FCSB pin: ", this->fcsb_pin_);
  LOG_PIN("  Power pin: ", this->power_pin_);
  LOG_PIN("  IRQ pin: ", this->irq_pin_);
  for (auto *valve : this->valves_) {
    if (valve->is_bound()) {
      ESP_LOGCONFIG(TAG, "  Slot '%s': valve %u, %s, %u zone(s), address %u, channel %u", valve->slot_name_,
                    (unsigned) valve->record_.valve_id, model_for_hardware(valve->record_.hardware_id),
                    valve->zone_count_, valve->record_.address, valve->downlink_channel());
    } else {
      ESP_LOGCONFIG(TAG, "  Slot '%s': not paired (%u zone(s))", valve->slot_name_, valve->zone_count_);
    }
  }
}

void DiivooHub::loop() {
  this->service_radio_();
  this->service_jobs_();

  const uint32_t now = millis();
  if (this->pairing_mode_ && now - this->pairing_started_ > this->pairing_timeout_ms_) {
    ESP_LOGI(TAG, "Pairing mode timed out");
    this->set_pairing_mode(false);
  }
  if (now - this->last_zone_tick_ >= ZONE_TICK_MS) {
    this->last_zone_tick_ = now;
    this->service_zones_();
  }
  const uint32_t watchdog_every = this->boot_ping_done_ ? WATCHDOG_TICK_MS : FIRST_WATCHDOG_MS;
  if (now - this->last_watchdog_ >= watchdog_every) {
    this->last_watchdog_ = now;
    this->boot_ping_done_ = true;
    this->service_watchdog_();
  }
}

// ======================================================================
// Hub: entity API
// ======================================================================

void DiivooHub::set_pairing_mode(bool on) {
  this->pairing_mode_ = on;
  this->pairing_started_ = millis();
  if (on) {
    ESP_LOGI(TAG, "Pairing mode ON for %u s - power-cycle the valve (take a battery out and back in)",
             (unsigned) (this->pairing_timeout_ms_ / 1000));
  } else {
    ESP_LOGI(TAG, "Pairing mode OFF");
  }
  if (this->pairing_switch_ != nullptr)
    this->pairing_switch_->publish_state(on);
}

bool DiivooHub::request_zone(DiivooValve *valve, uint8_t zone, bool open) {
  if (!valve->is_bound() || zone < 1 || zone > valve->zone_count_)
    return false;

  // A newer command for the same zone replaces one still waiting in the queue.
  for (auto it = this->normal_.begin(); it != this->normal_.end();) {
    if (it->type == JobType::ACTION && it->valve == valve && it->zone == zone) {
      it = this->normal_.erase(it);
    } else {
      ++it;
    }
  }

  RadioJob job;
  job.type = JobType::ACTION;
  job.valve = valve;
  job.zone = zone;
  job.open = open;
  job.seq = valve->next_action_seq();
  job.tx_channel = valve->downlink_channel();
  job.rx_channel = valve->downlink_channel();
  job.preamble = Preamble::LONG;
  uint8_t payload[5];
  uint8_t len;
  if (open) {
    const uint32_t seconds = valve->zone_duration_seconds(zone);
    payload[0] = zone;
    payload[1] = 0x02;
    payload[2] = 0x01;
    payload[3] = seconds & 0xFF;
    payload[4] = (seconds >> 8) & 0xFF;
    len = 5;
    ESP_LOGI(TAG, "Valve %u zone %u: open for %u s", (unsigned) valve->record_.valve_id, zone, (unsigned) seconds);
  } else {
    payload[0] = zone;
    payload[1] = 0x02;
    payload[2] = 0x00;
    len = 3;
    ESP_LOGI(TAG, "Valve %u zone %u: close", (unsigned) valve->record_.valve_id, zone);
  }
  this->build_frame_(job, valve, job.seq, CMD_ACTION, payload, len);
  valve->zone_(zone).busy = true;
  this->normal_.push_back(job);
  return true;
}

void DiivooHub::request_refresh(DiivooValve *valve) {
  if (!valve->is_bound())
    return;
  for (const auto &queued : this->normal_) {
    if (queued.type == JobType::PING && queued.valve == valve)
      return;
  }
  if (this->job_active_ && this->job_.type == JobType::PING && this->job_.valve == valve)
    return;

  RadioJob job;
  job.type = JobType::PING;
  job.valve = valve;
  job.seq = valve->next_command_seq();
  job.tx_channel = valve->downlink_channel();
  job.rx_channel = IDLE_RX_CHANNEL;
  job.preamble = Preamble::LONG;
  const uint8_t payload[2] = {job.seq, 0x01};
  this->build_frame_(job, valve, job.seq, CMD_PING, payload, sizeof(payload));
  valve->last_ping_ = millis();
  this->normal_.push_back(job);
  ESP_LOGD(TAG, "Valve %u: wake/refresh ping queued", (unsigned) valve->record_.valve_id);
}

void DiivooHub::unpair(DiivooValve *valve) {
  ESP_LOGI(TAG, "Slot '%s': forgetting valve %u", valve->slot_name_, (unsigned) valve->record_.valve_id);
  valve->record_ = ValveRecord{0, 0, 0, 0, 0};
  valve->save();
  valve->last_seen_ = 0;
  valve->publish_identity();
  for (uint8_t zone = 1; zone <= valve->zone_count_; zone++) {
    ZoneState &state = valve->zone_(zone);
    state.running = false;
    state.known = false;
    state.busy = false;
    state.remaining = 0;
    this->publish_zone_(valve, zone);
  }
  if (valve->online_sensor_ != nullptr)
    valve->online_sensor_->publish_state(false);
  valve->online_published_ = false;
  valve->online_known_ = true;
}

// ======================================================================
// Hub: radio jobs
// ======================================================================

void DiivooHub::build_frame_(RadioJob &job, DiivooValve *valve, uint8_t seq, uint8_t cmd, const uint8_t *payload,
                             uint8_t len, uint32_t target_override) {
  if (len > MAX_PAYLOAD)
    len = MAX_PAYLOAD;
  const uint32_t target = target_override != 0xFFFFFFFF ? target_override : valve->record_.valve_id;
  uint8_t *f = job.frame;
  memset(f, 0, PACKET_LENGTH);
  f[0] = FRAME_HEADER;
  f[1] = target & 0xFF;
  f[2] = (target >> 8) & 0xFF;
  f[3] = (target >> 16) & 0xFF;
  f[4] = (target >> 24) & 0xFF;
  f[5] = this->hub_id_ & 0xFF;
  f[6] = (this->hub_id_ >> 8) & 0xFF;
  f[7] = (this->hub_id_ >> 16) & 0xFF;
  f[8] = (this->hub_id_ >> 24) & 0xFF;
  f[9] = seq;
  f[10] = cmd;
  f[11] = len;
  memcpy(f + FRAME_OVERHEAD, payload, len);
}

void DiivooHub::queue_reply_(DiivooValve *valve, uint8_t seq, uint8_t cmd, const uint8_t *payload, uint8_t len,
                             uint8_t tx_channel, uint32_t gap_after_ms) {
  RadioJob job;
  job.type = JobType::SEND;
  job.valve = valve;
  job.seq = seq;
  job.tx_channel = tx_channel;
  job.rx_channel = IDLE_RX_CHANNEL;
  job.preamble = Preamble::SHORT;
  job.gap_after_ms = gap_after_ms;
  this->build_frame_(job, valve, seq, cmd, payload, len);
  this->urgent_.push_back(job);
}

void DiivooHub::service_jobs_() {
  const uint32_t now = millis();
  if (!this->job_active_) {
    if (now < this->next_job_at_ && this->next_job_at_ - now < 1000)
      return;
    if (!this->urgent_.empty() || !this->normal_.empty())
      this->start_job_();
    return;
  }

  if (this->phase_ == JobPhase::GAP) {
    if (now >= this->phase_until_)
      this->send_current_();
    return;
  }
  if (this->phase_ != JobPhase::WAITING)
    return;

  if (this->job_matched_) {
    this->finish_job_(true);
    return;
  }
  if (now < this->phase_until_)
    return;

  if (this->job_.type == JobType::ACTION) {
    if (!this->fallback_ && this->attempt_ < ACTION_ATTEMPTS) {
      ESP_LOGD(TAG, "Valve %u: no confirmation, retrying", (unsigned) this->job_.valve->record_.valve_id);
      this->phase_ = JobPhase::GAP;
      this->phase_until_ = now + ACTION_RETRY_GAP_MS;
    } else if (!this->fallback_) {
      ESP_LOGD(TAG, "Valve %u: switching to short-preamble retries", (unsigned) this->job_.valve->record_.valve_id);
      this->fallback_ = true;
      this->attempt_ = 0;
      this->send_current_();
    } else if (this->attempt_ < FALLBACK_ATTEMPTS) {
      this->phase_ = JobPhase::GAP;
      this->phase_until_ = now + FALLBACK_RETRY_GAP_MS;
    } else {
      this->finish_job_(false);
    }
  } else if (this->job_.type == JobType::PING) {
    if (this->attempt_ < PING_ATTEMPTS) {
      this->job_.frame[9] = this->job_.valve->next_command_seq();
      this->send_current_();
    } else {
      this->finish_job_(false);
    }
  } else {
    this->finish_job_(true);
  }
}

void DiivooHub::start_job_() {
  if (!this->urgent_.empty()) {
    this->job_ = this->urgent_.front();
    this->urgent_.pop_front();
  } else {
    this->job_ = this->normal_.front();
    this->normal_.pop_front();
  }
  this->job_active_ = true;
  this->attempt_ = 0;
  this->fallback_ = false;
  this->job_matched_ = false;
  this->high_freq_.start();
  this->send_current_();
}

void DiivooHub::send_current_() {
  this->attempt_++;
  RadioJob &job = this->job_;
  ESP_LOGV(TAG, "TX cmd=0x%02X seq=0x%02X ch=%u->%u %s: %s", job.frame[10], job.frame[9], job.tx_channel,
           job.rx_channel, this->fallback_ || job.preamble == Preamble::SHORT ? "short" : "long",
           format_hex_pretty(job.frame, PACKET_LENGTH).c_str());

  switch (job.type) {
    case JobType::SEND:
      if (!this->radio_.transmit(job.frame, PACKET_LENGTH, job.tx_channel, job.preamble, job.rx_channel))
        ESP_LOGW(TAG, "TX of cmd 0x%02X did not report done", job.frame[10]);
      this->finish_job_(true);
      return;
    case JobType::ACTION: {
      const Preamble preamble = this->fallback_ ? Preamble::SHORT : Preamble::LONG;
      this->radio_.transmit(job.frame, PACKET_LENGTH, job.tx_channel, preamble, job.rx_channel);
      this->phase_ = JobPhase::WAITING;
      this->phase_until_ = millis() + (this->fallback_ ? FALLBACK_WAIT_MS : ACTION_WAIT_MS);
      return;
    }
    case JobType::PING:
      this->radio_.transmit(job.frame, PACKET_LENGTH, job.tx_channel, Preamble::LONG, job.rx_channel);
      this->phase_ = JobPhase::WAITING;
      this->phase_until_ = millis() + PING_LISTEN_MS;
      return;
  }
}

void DiivooHub::finish_job_(bool success) {
  RadioJob &job = this->job_;
  if (job.type == JobType::ACTION) {
    this->radio_.tune(IDLE_RX_CHANNEL, Preamble::SHORT);
    ZoneState &zone = job.valve->zone_(job.zone);
    zone.busy = false;
    if (!success) {
      ESP_LOGW(TAG, "Valve %u zone %u: no confirmation for %s after all retries",
               (unsigned) job.valve->record_.valve_id, job.zone, job.open ? "open" : "close");
    }
    this->publish_zone_(job.valve, job.zone);
  } else if (job.type == JobType::PING && !success) {
    ESP_LOGW(TAG, "Valve %u did not answer the wake/refresh ping", (unsigned) job.valve->record_.valve_id);
  }

  this->job_active_ = false;
  this->phase_ = JobPhase::IDLE;
  this->next_job_at_ = millis() + job.gap_after_ms;
  if (this->urgent_.empty() && this->normal_.empty())
    this->high_freq_.stop();
}

// ======================================================================
// Hub: inbound frames
// ======================================================================

void DiivooHub::service_radio_() {
  while (true) {
    bool pending;
    {
      InterruptLock lock;
      pending = this->pending_irqs_ > 0;
      if (pending)
        this->pending_irqs_ = this->pending_irqs_ - 1;
    }
    if (!pending)
      return;

    uint8_t frame[PACKET_LENGTH];
    int8_t rssi;
    uint8_t flags;
    this->radio_.read_packet(frame, &rssi, &flags);
    ESP_LOGV(TAG, "RX flags=0x%02X rssi=%d: %s", flags, rssi, format_hex_pretty(frame, PACKET_LENGTH).c_str());
    this->handle_frame_(frame, rssi);
  }
}

bool DiivooHub::is_duplicate_(const uint8_t *frame) {
  const uint32_t now = millis();
  const uint32_t hash = hash_frame(frame);
  for (const auto &seen : this->seen_) {
    if (seen.hash == hash && seen.at != 0 && now - seen.at < DUPLICATE_WINDOW_MS)
      return true;
  }
  this->seen_[this->seen_next_] = SeenFrame{hash, now == 0 ? 1 : now};
  this->seen_next_ = (this->seen_next_ + 1) % (sizeof(this->seen_) / sizeof(this->seen_[0]));
  return false;
}

void DiivooHub::handle_frame_(const uint8_t *frame, int8_t rssi) {
  const uint32_t sender = read_le32(frame + 5);
  const uint8_t seq = frame[9];
  const uint8_t cmd = frame[10];
  uint8_t len = frame[11];
  if (len > MAX_PAYLOAD)
    len = MAX_PAYLOAD;
  const uint8_t *payload = frame + FRAME_OVERHEAD;

  if (sender == 0 || sender == 0xFFFFFFFF || sender == this->hub_id_)
    return;

  DiivooValve *valve = this->find_valve_(sender);
  if (valve == nullptr) {
    if (cmd == CMD_JOIN && this->pairing_mode_) {
      for (auto *slot : this->valves_) {
        if (!slot->is_bound()) {
          valve = slot;
          break;
        }
      }
      if (valve == nullptr) {
        ESP_LOGW(TAG, "Valve %u wants to pair but every slot is taken - unpair one first", (unsigned) sender);
        return;
      }
    } else {
      const uint32_t now = millis();
      if (sender != this->last_unknown_id_ || now - this->last_unknown_log_ > 60000) {
        this->last_unknown_id_ = sender;
        this->last_unknown_log_ = now;
        ESP_LOGI(TAG, "Heard unpaired valve %u (cmd 0x%02X, %d dBm)%s", (unsigned) sender, cmd, rssi,
                 cmd == CMD_JOIN ? " - turn pairing mode on to adopt it" : "");
      }
      return;
    }
  }

  ESP_LOGD(TAG, "Valve %u: cmd 0x%02X seq 0x%02X len %u (%d dBm)", (unsigned) sender, cmd, seq, len, rssi);
  this->mark_seen_(valve, rssi);
  this->match_current_job_(valve, seq, cmd, payload, len);
  if (this->is_duplicate_(frame))
    return;

  switch (cmd) {
    case CMD_JOIN:
      this->handle_join_(valve, sender, seq, payload, len);
      break;
    case CMD_STATUS:
      this->handle_status_(valve, seq, payload, len);
      break;
    case CMD_EVENT:
      this->handle_event_(valve, seq, payload, len);
      break;
    case CMD_PARAMETER_REQUEST:
      this->handle_parameter_request_(valve, seq, payload, len);
      break;
    case CMD_SCHEDULE_REQUEST: {
      // No schedules: Home Assistant does the scheduling.
      const uint8_t empty[1] = {0x00};
      this->queue_reply_(valve, seq, CMD_SCHEDULES, empty, sizeof(empty), valve->downlink_channel());
      break;
    }
    case CMD_SIMPLE_ACK:
      break;
    case CMD_ACTION_ACK:
      this->handle_action_ack_(valve, seq, payload, len);
      break;
    default:
      ESP_LOGD(TAG, "Valve %u: ignoring cmd 0x%02X", (unsigned) sender, cmd);
      break;
  }
}

void DiivooHub::match_current_job_(DiivooValve *valve, uint8_t seq, uint8_t cmd, const uint8_t *payload,
                                   uint8_t len) {
  if (!this->job_active_ || this->phase_ != JobPhase::WAITING || this->job_.valve != valve)
    return;

  if (this->job_.type == JobType::PING) {
    this->job_matched_ = true;
    return;
  }
  if (this->job_.type != JobType::ACTION)
    return;

  if (cmd == CMD_ACTION_ACK && seq == this->job_.seq && len >= 13 && is_running(payload[1]) == this->job_.open) {
    this->job_matched_ = true;
  } else if (cmd == CMD_STATUS && len >= 15 && valve->normalize_zone(payload[2]) == this->job_.zone &&
             is_running(payload[3]) == this->job_.open) {
    this->job_matched_ = true;
  }
}

void DiivooHub::handle_join_(DiivooValve *valve, uint32_t sender, uint8_t seq, const uint8_t *payload, uint8_t len) {
  if (len < 7) {
    ESP_LOGW(TAG, "Valve %u: join request too short", (unsigned) sender);
    return;
  }
  const uint8_t proposed = payload[0];
  const uint16_t hardware_id = uint16_t(payload[2]) | (uint16_t(payload[3]) << 8);
  ESP_LOGI(TAG, "Valve %u: join | model %s (0x%04X) fw %u | proposes channel code %u", (unsigned) sender,
           model_for_hardware(hardware_id), hardware_id, payload[6], proposed);

  uint8_t subtype;
  uint8_t status;
  uint8_t channels[3];
  uint8_t channel_count = 0;
  auto add_channel = [&](int channel) {
    if (channel < 0)
      return;
    for (uint8_t i = 0; i < channel_count; i++) {
      if (channels[i] == channel)
        return;
    }
    channels[channel_count++] = static_cast<uint8_t>(channel);
  };

  if (!valve->is_bound()) {
    const uint8_t zones = zones_for_hardware(hardware_id);
    if (zones != 0 && zones != valve->zone_count_) {
      ESP_LOGW(TAG, "Slot '%s' is declared with %u zone(s) but this %s has %u", valve->slot_name_,
               valve->zone_count_, model_for_hardware(hardware_id), zones);
    }
    valve->record_.valve_id = sender;
    valve->record_.hardware_id = hardware_id;
    valve->record_.address = this->allocate_address_();
    valve->record_.channel_code = this->allocate_channel_code_(proposed);
    valve->record_.bound = 1;
    valve->save();
    valve->publish_identity();
    ESP_LOGI(TAG, "Slot '%s': paired valve %u | address %u, channel code %u (channel %u)", valve->slot_name_,
             (unsigned) sender, valve->record_.address, valve->record_.channel_code, valve->downlink_channel());
    subtype = 0x0A;
    status = 0x01;
    add_channel(PAIRING_CHANNEL);
    add_channel(valve->downlink_channel());
    this->set_pairing_mode(false);
  } else {
    if (valve->record_.hardware_id != hardware_id) {
      valve->record_.hardware_id = hardware_id;
      valve->save();
      valve->publish_identity();
    }
    if (proposed != valve->record_.channel_code) {
      ESP_LOGI(TAG, "Valve %u: proposes channel code %u, expected %u - restoring", (unsigned) sender, proposed,
               valve->record_.channel_code);
    }
    subtype = 0x00;
    status = 0x02;
    add_channel(PAIRING_CHANNEL);
    add_channel(proposed > 0 ? proposed - 1 : -1);
    add_channel(valve->downlink_channel());
  }

  if (!this->time_valid_())
    ESP_LOGW(TAG, "No valid time yet - the join response carries a zero date");

  uint8_t response[11];
  response[0] = subtype;
  response[1] = valve->record_.address;
  response[2] = valve->record_.channel_code;
  response[3] = 0xE0;
  response[4] = 0x01;
  this->encode_tuya_date_(response + 5);
  response[9] = this->day_of_week_();
  response[10] = status;

  for (uint8_t i = 0; i < channel_count; i++) {
    const uint32_t gap = (i + 1 < channel_count) ? JOIN_RESPONSE_GAP_MS : 0;
    this->queue_reply_(valve, seq, CMD_JOIN_RESPONSE, response, sizeof(response), channels[i], gap);
  }
}

void DiivooHub::handle_status_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len) {
  if (len < 15) {
    ESP_LOGD(TAG, "Valve %u: status report too short", (unsigned) valve->record_.valve_id);
    return;
  }
  const uint8_t meta = payload[1];
  const uint8_t battery = (meta >> 3) & 0x03;
  const uint8_t event = meta & 0x07;
  const uint8_t zone = valve->normalize_zone(payload[2]);
  const bool running = is_running(payload[3]);
  const uint16_t remaining = read_le16(payload + 10);
  const uint16_t runtime = read_le16(payload + 13);

  ESP_LOGD(TAG, "Valve %u zone %u: %s (state 0x%02X) remaining %u s of %u s, battery %u", (unsigned) valve->record_.valve_id,
           zone, running ? "running" : "off", payload[3], remaining, runtime, battery);

  if (zone != 0) {
    ZoneState &state = valve->zone_(zone);
    state.running = running;
    state.known = true;
    state.remaining = running ? remaining : 0;
    state.reported_at = millis();
    this->publish_zone_(valve, zone);
  }

  if (battery == 1 || battery == 2) {
    if (valve->battery_sensor_ != nullptr)
      valve->battery_sensor_->publish_state(battery == 1 ? 100.0f : 20.0f);
    if (valve->battery_low_sensor_ != nullptr)
      valve->battery_low_sensor_->publish_state(battery == 2);
  }

  if (event == 6 && this->time_valid_()) {
    // Acknowledge with the current time.
    ESPTime t = this->time_->now();
    const uint8_t minute = t.minute;
    const uint8_t hour = t.hour;
    const uint8_t day = t.day_of_month;
    const uint8_t month = t.month;
    const uint8_t year = static_cast<uint8_t>(t.year - 2020);
    uint8_t reply[7];
    reply[0] = 0x00;
    reply[1] = CONTEXT_BYTE;
    reply[2] = (t.second & 0x3F) | ((minute & 0x03) << 6);
    reply[3] = ((minute >> 2) & 0x0F) | ((hour & 0x0F) << 4);
    reply[4] = ((hour >> 4) & 0x01) | ((day & 0x1F) << 1) | ((month & 0x03) << 6);
    reply[5] = ((month >> 2) & 0x03) | ((year & 0x3F) << 2);
    reply[6] = (this->day_of_week_() & 0x07) | (0x03 << 3);
    ESP_LOGD(TAG, "Valve %u: time sync", (unsigned) valve->record_.valve_id);
    this->queue_reply_(valve, seq, CMD_ACK, reply, sizeof(reply), valve->downlink_channel());
  } else {
    const uint8_t reply[2] = {0x00, CONTEXT_BYTE};
    this->queue_reply_(valve, seq, CMD_ACK, reply, sizeof(reply), valve->downlink_channel());
  }
}

void DiivooHub::handle_event_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len) {
  if (len >= 14) {
    const uint8_t zone = valve->normalize_zone(payload[1]);
    const uint8_t event = payload[2];
    const uint32_t water_ml = read_le32(payload + 8);
    const uint16_t elapsed = read_le16(payload + 12);
    ESP_LOGD(TAG, "Valve %u zone %u: event 0x%02X, ran %u s, %u ml", (unsigned) valve->record_.valve_id, zone, event,
             elapsed, (unsigned) water_ml);
    if (zone != 0) {
      ZoneState &state = valve->zone_(zone);
      if (state.water_sensor != nullptr)
        state.water_sensor->publish_state(water_ml / 1000.0f);
      if (event == 0x01) {
        state.running = false;
        state.known = true;
        state.remaining = 0;
        state.reported_at = millis();
        this->publish_zone_(valve, zone);
      }
    }
  }
  const uint8_t reply[1] = {0x00};
  this->queue_reply_(valve, seq, CMD_EVENT_ACK, reply, sizeof(reply), valve->downlink_channel());
}

void DiivooHub::handle_parameter_request_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len) {
  uint8_t zone = len > 1 ? valve->normalize_zone(payload[1]) : 1;
  if (zone == 0)
    zone = 1;
  const uint32_t duration = valve->zone_duration_seconds(zone);
  const uint16_t interval_on = 10;
  const uint16_t interval_off = 30;
  // Duration, mist interval on/off, no rain delay.
  const uint8_t reply[15] = {
      0x00,
      uint8_t(duration & 0xFF),
      uint8_t((duration >> 8) & 0xFF),
      uint8_t(interval_on & 0xFF),
      uint8_t(interval_on >> 8),
      uint8_t(interval_off & 0xFF),
      uint8_t(interval_off >> 8),
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
  };
  ESP_LOGD(TAG, "Valve %u zone %u: parameters requested, default duration %u s", (unsigned) valve->record_.valve_id,
           zone, (unsigned) duration);
  this->queue_reply_(valve, seq, CMD_PARAMETERS, reply, sizeof(reply), valve->downlink_channel());
}

void DiivooHub::handle_action_ack_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len) {
  if (len >= 13 && this->job_active_ && this->job_.type == JobType::ACTION && this->job_.valve == valve &&
      this->job_.seq == seq) {
    ZoneState &state = valve->zone_(this->job_.zone);
    state.running = is_running(payload[1]);
    state.known = true;
    state.remaining = state.running ? read_le16(payload + 8) : 0;
    state.reported_at = millis();
    ESP_LOGD(TAG, "Valve %u zone %u: action confirmed, %s, remaining %u s", (unsigned) valve->record_.valve_id,
             this->job_.zone, state.running ? "running" : "off", state.remaining);
  }
  // The valve expects a final ACK after every action ACK.
  const uint8_t reply[2] = {0x00, CONTEXT_BYTE};
  this->queue_reply_(valve, valve->next_command_seq(), CMD_ACK, reply, sizeof(reply), valve->downlink_channel());
}

// ======================================================================
// Hub: state
// ======================================================================

void DiivooHub::publish_zone_(DiivooValve *valve, uint8_t zone) {
  ZoneState &state = valve->zone_(zone);
  if (state.valve != nullptr)
    state.valve->report(state.running, state.busy);
  if (state.remaining_sensor != nullptr) {
    float remaining = 0;
    if (state.running && state.known) {
      const uint32_t elapsed = (millis() - state.reported_at) / 1000;
      remaining = elapsed >= state.remaining ? 0 : state.remaining - elapsed;
    }
    if (remaining != state.last_remaining_published) {
      state.last_remaining_published = remaining;
      state.remaining_sensor->publish_state(remaining);
    }
  }
}

void DiivooHub::service_zones_() {
  const uint32_t now = millis();
  for (auto *valve : this->valves_) {
    for (uint8_t zone = 1; zone <= valve->zone_count_; zone++) {
      ZoneState &state = valve->zone_(zone);
      if (state.running && state.known && !state.busy) {
        const uint32_t elapsed = (now - state.reported_at) / 1000;
        if (state.remaining > 0 && elapsed > state.remaining + FINISHED_GRACE_S) {
          // The valve times itself out; if its stop report was lost, catch up.
          ESP_LOGI(TAG, "Valve %u zone %u: run time elapsed without a stop report, assuming closed",
                   (unsigned) valve->record_.valve_id, zone);
          state.running = false;
          state.remaining = 0;
        }
      }
      this->publish_zone_(valve, zone);
    }

    const bool online = valve->is_bound() && valve->last_seen_ != 0 && now - valve->last_seen_ < OFFLINE_AFTER_MS;
    if (valve->online_sensor_ != nullptr && (!valve->online_known_ || online != valve->online_published_)) {
      valve->online_sensor_->publish_state(online);
      valve->online_published_ = online;
      valve->online_known_ = true;
    }
  }
}

void DiivooHub::service_watchdog_() {
  const uint32_t now = millis();
  for (auto *valve : this->valves_) {
    if (!valve->is_bound())
      continue;
    const bool silent = valve->last_seen_ == 0 || now - valve->last_seen_ > PING_AFTER_SILENCE_MS;
    const bool ping_due = valve->last_ping_ == 0 || now - valve->last_ping_ > PING_MIN_INTERVAL_MS;
    if (silent && ping_due)
      this->request_refresh(valve);
  }
}

void DiivooHub::mark_seen_(DiivooValve *valve, int8_t rssi) {
  valve->last_seen_ = millis();
  if (valve->last_seen_ == 0)
    valve->last_seen_ = 1;
  valve->rssi_ = rssi;
  if (valve->rssi_sensor_ != nullptr)
    valve->rssi_sensor_->publish_state(rssi);
  if (valve->online_sensor_ != nullptr && (!valve->online_known_ || !valve->online_published_)) {
    valve->online_sensor_->publish_state(true);
    valve->online_published_ = true;
    valve->online_known_ = true;
  }
}

uint8_t DiivooHub::allocate_address_() const {
  for (uint8_t address = 1; address < 255; address++) {
    bool used = false;
    for (auto *valve : this->valves_) {
      if (valve->is_bound() && valve->record_.address == address)
        used = true;
    }
    if (!used)
      return address;
  }
  return 1;
}

uint8_t DiivooHub::allocate_channel_code_(uint8_t proposed) const {
  auto used = [this](uint8_t code) {
    for (auto *valve : this->valves_) {
      if (valve->is_bound() && valve->record_.channel_code == code)
        return true;
    }
    return false;
  };
  for (uint8_t code = 0x08; code <= 0x10; code++) {
    if (!used(code))
      return code;
  }
  if (proposed > 0 && !used(proposed))
    return proposed;
  return 0x06;
}

DiivooValve *DiivooHub::find_valve_(uint32_t valve_id) const {
  for (auto *valve : this->valves_) {
    if (valve->is_bound() && valve->record_.valve_id == valve_id)
      return valve;
  }
  return nullptr;
}

bool DiivooHub::time_valid_() const { return this->time_ != nullptr && this->time_->now().is_valid(); }

void DiivooHub::encode_tuya_date_(uint8_t *out) const {
  uint32_t value = 0;
  if (this->time_valid_()) {
    ESPTime t = this->time_->now();
    value = ((uint32_t(t.year - 2020) & 0x3F) << 26) | ((uint32_t(t.month) & 0x0F) << 22) |
            ((uint32_t(t.day_of_month) & 0x1F) << 17) | ((uint32_t(t.hour) & 0x1F) << 12) |
            ((uint32_t(t.minute) & 0x3F) << 6) | (uint32_t(t.second) & 0x3F);
  }
  out[0] = value & 0xFF;
  out[1] = (value >> 8) & 0xFF;
  out[2] = (value >> 16) & 0xFF;
  out[3] = (value >> 24) & 0xFF;
}

// 0 = Sunday, like JavaScript's getDay(); ESPTime counts Sunday as 1.
uint8_t DiivooHub::day_of_week_() const {
  if (!this->time_valid_())
    return 0;
  const uint8_t dow = this->time_->now().day_of_week;
  return dow >= 1 && dow <= 7 ? dow - 1 : 0;
}

}  // namespace diivoo
}  // namespace esphome
