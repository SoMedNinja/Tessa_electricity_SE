# Tessa electricity SE

A Tessera electricity tile for Home Assistant. Choose a price sensor and configure purchase and optional sales costs in the tile inspector.

## Features

- Separate Nord Pool, ENTSO-e, GE-Spot and EPEX Spot tile types. Nord Pool entity selection includes current-price sensors without a `raw_today` attribute.
- Currency follows the sensor's `unit_of_measurement`, including `SEK/kWh`. Optional cents display scales the number without changing its currency.
- When the sensor supplies price lists, the graph shows today and tomorrow at the sensor's resolution, including 15-minute data. Each day is ranked independently: cheapest 30% green, middle 40% graded green–amber–red, priciest 30% red. Past bars are grey.
- Tap a graph interval to show its price and time range in the headline.
- Delivery and sales prices use separate numeric settings. No formulas need to be typed. Tessera's supported tile option kinds allow one selected sensor per tile, so sales pricing uses that sensor plus the configured sales additional cost.

## Configure costs

In the tile inspector, choose the **Entity** for delivery, then set:

- **Tax (%)**: percentage uplift. Enter `25` for 25%.
- **Delivery additional cost**: added after tax, in the selected sensor's currency per kWh.
- **Show sales price**: optionally show the selected sensor's price adjusted for sales.
- **Sales additional cost**: added to that sensor's price, in its currency per kWh.

The calculations are:

- Delivery: `delivery sensor × (1 + tax / 100) + delivery additional cost`
- Sales: `selected sensor + sales additional cost`

For a SEK/kWh sensor, an additional cost of `0.08` means 0.08 SEK/kWh. Costs may be negative.

## Install

1. In Tessera's **Plugins** page, add this repository by link.
2. Place a **Tessa electricity SE** tile in **Layout** and select the matching sensor in **Entity**.
3. Set tax and additional costs in the tile inspector. Enable **Show sales price** if you want the adjusted price shown.
4. Use a tile at least 2×2 for the graph. Tap a bar to inspect its interval; tap elsewhere to open the full-screen graph card.

The plugin uses Tessera's native plugin API and ESPHome external-component structure, for Tessera-supported boards including the Waveshare ESP32-S3-Touch-LCD-7B.

## Sensor data

The graph reads these list attributes:

- Nord Pool: `raw_today[*].value`, `raw_tomorrow[*].value`
- ENTSO-e: `prices_today[*].price`, `prices_tomorrow[*].price`
- GE-Spot: `today_interval_prices[*].value`, `tomorrow_interval_prices[*].value`
- EPEX Spot: `data[*].price_per_kwh` with each item's `start_time`

If a selected entity exposes only its current value and no supported price lists, the tile can show its current price but cannot draw the interval graph. Hourly lists remain hourly; quarter-hour lists retain 15-minute resolution.

## Attribution

This plugin adapts the MIT-licensed Tessera electricity-price plugin by Heronimonimo. See [LICENSE](LICENSE).

