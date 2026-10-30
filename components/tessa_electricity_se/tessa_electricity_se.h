#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::tessa_electricity_se {

struct Shared;
struct BarSource;

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tiles, its card and its top bar item in
// setup().
class ElectricityPrice : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 private:
  tessera::BarItem bar();

  Shared *shared_ = nullptr;  // what a card takes over from the tile that opened it
  BarSource *bar_ = nullptr;  // what the top bar shows: a tile's prices
};

}  // namespace esphome::tessa_electricity_se

