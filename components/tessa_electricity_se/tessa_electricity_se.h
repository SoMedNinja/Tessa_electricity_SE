#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"
#include <string>

namespace esphome::tessa_electricity_se {

struct Shared;
struct BarSource;

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tiles, its card and its top bar item in
// setup().
class ElectricityPrice : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_nordpool_config_entry(const std::string &value) { nordpool_config_entry_ = value; }
  void on_message(JsonObjectConst message) override;

 private:
  tessera::BarItem bar();

  Shared *shared_ = nullptr;  // what a card takes over from the tile that opened it
  BarSource *bar_ = nullptr;  // what the top bar shows: a tile's prices
  std::string nordpool_config_entry_;
};

}  // namespace esphome::tessa_electricity_se

