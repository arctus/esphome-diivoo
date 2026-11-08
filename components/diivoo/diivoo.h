#pragma once

// DIIVOO irrigation timers, controlled directly from the WG03 gateway's own
// ESP32 - no cloud, no add-on, no TCP bridge.
//
// Port of the radio protocol reverse-engineered by Technerd-SG in
// hassio-diivoo2mqtt (AGPL-3.0), reduced to what Home Assistant needs:
// pairing, opening and closing zones, and answering every request a valve
// makes so it stays content. Schedules are deliberately left out - a valve
// that asks for its plans is told it has none, so watering is driven by
// Home Assistant only.

#include <cmath>
#include <deque>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/valve/valve.h"
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif

#include "cmt2300a.h"

namespace esphome {
namespace diivoo {

static const uint8_t MAX_ZONES = 4;

class DiivooHub;
class DiivooValve;

// ---- Entities

/// One zone of a valve, exposed as a Home Assistant water valve.
class DiivooZoneValve : public valve::Valve, public Component {
 public:
  void set_target(DiivooValve *target, uint8_t zone) {
    this->target_ = target;
    this->zone_ = zone;
  }
  void setup() override;
  void dump_config() override;
  /// Called by the hub with the valve's reported state.
  void report(bool open, bool busy);

 protected:
  valve::ValveTraits get_traits() override;
  void control(const valve::ValveCall &call) override;

  DiivooValve *target_{nullptr};
  uint8_t zone_{1};
};

/// How long a zone runs when opened, in minutes. Also the safety limit: the
/// valve closes itself when it runs out, whatever happens to the gateway.
class DiivooDurationNumber : public number::Number, public Component {
 public:
  void set_target(DiivooValve *target, uint8_t zone) {
    this->target_ = target;
    this->zone_ = zone;
  }
  void set_initial_value(float value) { this->initial_value_ = value; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void dump_config() override;

 protected:
  void control(float value) override;

  DiivooValve *target_{nullptr};
  uint8_t zone_{1};
  float initial_value_{10};
  ESPPreferenceObject pref_;
};

class DiivooPairingSwitch : public switch_::Switch, public Parented<DiivooHub> {
 protected:
  void write_state(bool state) override;
};

enum class ValveButtonAction : uint8_t {
  REFRESH,
  UNPAIR,
};

class DiivooValveButton : public button::Button {
 public:
  void set_target(DiivooValve *target, ValveButtonAction action) {
    this->target_ = target;
    this->action_ = action;
  }

 protected:
  void press_action() override;

  DiivooValve *target_{nullptr};
  ValveButtonAction action_{ValveButtonAction::REFRESH};
};


/// What the gateway knows about one paired valve, persisted across reboots.
struct ValveRecord {
  uint32_t valve_id;
  uint16_t hardware_id;
  uint8_t address;
  uint8_t channel_code;
  uint8_t bound;
};

struct ZoneState {
  bool running{false};
  bool known{false};
  bool busy{false};
  uint16_t remaining{0};
  uint32_t reported_at{0};
  float last_remaining_published{NAN};
  DiivooZoneValve *valve{nullptr};
  DiivooDurationNumber *duration{nullptr};
  sensor::Sensor *remaining_sensor{nullptr};
  sensor::Sensor *water_sensor{nullptr};
};

/// One valve slot. The component creates `max_valves` of them, each with a
/// full set of entities for four zones; pairing fills the first empty slot,
/// and only the zones the paired model really has are shown to Home
/// Assistant (decided at boot, so the gateway restarts after a pairing).
class DiivooValve {
 public:
  DiivooValve(uint32_t pref_hash, const char *slot_name) : pref_hash_(pref_hash), slot_name_(slot_name) {}

  // ---- Codegen setters

  void set_zone_valve(uint8_t zone, DiivooZoneValve *v) { this->zone_(zone).valve = v; }
  void set_zone_duration(uint8_t zone, DiivooDurationNumber *n) { this->zone_(zone).duration = n; }
  void set_zone_remaining_sensor(uint8_t zone, sensor::Sensor *s) { this->zone_(zone).remaining_sensor = s; }
  void set_zone_water_sensor(uint8_t zone, sensor::Sensor *s) { this->zone_(zone).water_sensor = s; }
  void set_rssi_sensor(sensor::Sensor *s) { this->rssi_sensor_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { this->battery_sensor_ = s; }
  void set_battery_low_sensor(binary_sensor::BinarySensor *s) { this->battery_low_sensor_ = s; }
  void set_online_sensor(binary_sensor::BinarySensor *s) { this->online_sensor_ = s; }
  void set_valve_id_sensor(text_sensor::TextSensor *s) { this->valve_id_sensor_ = s; }
  void set_model_sensor(text_sensor::TextSensor *s) { this->model_sensor_ = s; }
  void set_refresh_button(button::Button *b) { this->refresh_button_ = b; }
  void set_unpair_button(button::Button *b) { this->unpair_button_ = b; }


  // ---- Entity API

  DiivooHub *hub() const { return this->hub_; }
  /// Zones of the paired model (0 when the slot is empty).
  uint8_t zone_count() const;
  bool is_bound() const { return this->record_.bound == 1; }
  bool zone_running(uint8_t zone) const;
  uint32_t zone_duration_seconds(uint8_t zone) const;
  void save_duration(uint8_t zone, float minutes);


 protected:
  friend class DiivooHub;

  ZoneState &zone_(uint8_t zone) { return this->zones_[(zone >= 1 && zone <= MAX_ZONES) ? zone - 1 : 0]; }
  uint8_t downlink_channel() const;
  uint8_t next_command_seq() { return this->command_seq_++; }
  uint8_t next_action_seq() { return this->action_seq_++; }
  /// Raw zone index from a frame -> 1-based zone, or 0 when out of range.
  uint8_t normalize_zone(uint8_t raw) const;

  void load();
  void save();
  void publish_identity();
  /// Hide the entities of an empty slot and of zones the model lacks.
  void apply_visibility();

  DiivooHub *hub_{nullptr};
  uint32_t pref_hash_;
  const char *slot_name_;
  ESPPreferenceObject pref_;
  ValveRecord record_{0, 0, 0, 0, 0};
  ZoneState zones_[MAX_ZONES];

  uint8_t command_seq_{0x02};
  uint8_t action_seq_{0x03};
  uint32_t last_seen_{0};
  uint32_t last_ping_{0};
  int8_t rssi_{0};
  bool online_published_{false};
  bool online_known_{false};

  sensor::Sensor *rssi_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  binary_sensor::BinarySensor *battery_low_sensor_{nullptr};
  binary_sensor::BinarySensor *online_sensor_{nullptr};
  text_sensor::TextSensor *valve_id_sensor_{nullptr};
  text_sensor::TextSensor *model_sensor_{nullptr};
  button::Button *refresh_button_{nullptr};
  button::Button *unpair_button_{nullptr};
};

class DiivooHub : public Component {
 public:
  // ---- Codegen setters

  void set_sclk_pin(GPIOPin *pin) { this->sclk_pin_ = pin; }
  void set_sdio_pin(GPIOPin *pin) { this->sdio_pin_ = pin; }
  void set_csb_pin(GPIOPin *pin) { this->csb_pin_ = pin; }
  void set_fcsb_pin(GPIOPin *pin) { this->fcsb_pin_ = pin; }
  void set_power_pin(GPIOPin *pin) { this->power_pin_ = pin; }
  void set_irq_pin(InternalGPIOPin *pin) { this->irq_pin_ = pin; }
  void set_hub_id(uint32_t id) { this->hub_id_ = id; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
  void set_pairing_timeout(uint32_t ms) { this->pairing_timeout_ms_ = ms; }
  void set_pairing_switch(DiivooPairingSwitch *s) { this->pairing_switch_ = s; }
#ifdef USE_LIGHT
  void set_pairing_light(light::LightState *light, const char *effect) {
    this->pairing_light_ = light;
    this->pairing_effect_ = effect;
  }
#endif
  void add_valve(DiivooValve *valve);


  // ---- Component

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }


  // ---- Entity API

  void set_pairing_mode(bool on);
  void toggle_pairing_mode() { this->set_pairing_mode(!this->pairing_mode_); }
  /// Queue an open/close for one zone. False when the valve is not paired.
  bool request_zone(DiivooValve *valve, uint8_t zone, bool open);
  void request_refresh(DiivooValve *valve);
  void unpair(DiivooValve *valve);


 protected:
  enum class JobType : uint8_t {
    SEND,
    ACTION,
    PING,
  };

  enum class JobPhase : uint8_t {
    IDLE,
    WAITING,
    GAP,
  };

  struct RadioJob {
    JobType type{JobType::SEND};
    DiivooValve *valve{nullptr};
    uint8_t frame[PACKET_LENGTH]{};
    uint8_t tx_channel{4};
    uint8_t rx_channel{0};
    Preamble preamble{Preamble::SHORT};
    uint32_t gap_after_ms{0};
    uint8_t zone{0};
    bool open{false};
    uint8_t seq{0};
  };

  struct SeenFrame {
    uint32_t hash;
    uint32_t at;
  };

  static void gpio_intr(DiivooHub *arg);

  // ---- Radio jobs

  void build_frame_(RadioJob &job, DiivooValve *valve, uint8_t seq, uint8_t cmd, const uint8_t *payload,
                    uint8_t len, uint32_t target_override = 0xFFFFFFFF);
  void queue_reply_(DiivooValve *valve, uint8_t seq, uint8_t cmd, const uint8_t *payload, uint8_t len,
                    uint8_t tx_channel, uint32_t gap_after_ms = 0);
  void service_jobs_();
  void start_job_();
  void finish_job_(bool success);
  void send_current_();


  // ---- Inbound frames

  void service_radio_();
  void handle_frame_(const uint8_t *frame, int8_t rssi);
  bool is_duplicate_(const uint8_t *frame);
  void match_current_job_(DiivooValve *valve, uint8_t seq, uint8_t cmd, const uint8_t *payload, uint8_t len);
  void handle_join_(DiivooValve *valve, uint32_t sender, uint8_t seq, const uint8_t *payload, uint8_t len);
  void handle_status_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len);
  void handle_event_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len);
  void handle_parameter_request_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len);
  void handle_action_ack_(DiivooValve *valve, uint8_t seq, const uint8_t *payload, uint8_t len);


  // ---- State

  void publish_zone_(DiivooValve *valve, uint8_t zone);
  void service_zones_();
  void service_watchdog_();
  void mark_seen_(DiivooValve *valve, int8_t rssi);
  uint8_t allocate_address_() const;
  uint8_t allocate_channel_code_(uint8_t proposed) const;
  DiivooValve *find_valve_(uint32_t valve_id) const;
  bool time_valid_() const;
  void encode_tuya_date_(uint8_t *out) const;
  uint8_t day_of_week_() const;
  /// Restart once the radio is idle, so HA sees the new entity set.
  void schedule_restart_(const char *reason);


  CMT2300A radio_;
  GPIOPin *sclk_pin_{nullptr};
  GPIOPin *sdio_pin_{nullptr};
  GPIOPin *csb_pin_{nullptr};
  GPIOPin *fcsb_pin_{nullptr};
  GPIOPin *power_pin_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  time::RealTimeClock *time_{nullptr};
  DiivooPairingSwitch *pairing_switch_{nullptr};
#ifdef USE_LIGHT
  light::LightState *pairing_light_{nullptr};
  const char *pairing_effect_{nullptr};
#endif

  uint32_t hub_id_{0};
  std::vector<DiivooValve *> valves_;
  volatile uint32_t pending_irqs_{0};

  bool pairing_mode_{false};
  uint32_t pairing_timeout_ms_{300000};
  uint32_t pairing_started_{0};
  uint32_t last_unknown_id_{0};
  uint32_t last_unknown_log_{0};

  std::deque<RadioJob> urgent_;
  std::deque<RadioJob> normal_;
  bool job_active_{false};
  RadioJob job_;
  JobPhase phase_{JobPhase::IDLE};
  uint32_t phase_until_{0};
  uint8_t attempt_{0};
  bool fallback_{false};
  bool job_matched_{false};
  uint32_t next_job_at_{0};

  SeenFrame seen_[8]{};
  uint8_t seen_next_{0};

  uint32_t boot_at_{0};
  bool boot_ping_done_{false};
  uint32_t last_zone_tick_{0};
  uint32_t last_watchdog_{0};
  uint32_t restart_at_{0};
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace diivoo
}  // namespace esphome
