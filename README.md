# Tessa electricity SE

A Tessera electricity tile for Home Assistant. Choose a price sensor and configure purchase and optional sales costs in the tile inspector.

## Features

- Separate Nord Pool, ENTSO-e, GE-Spot and EPEX Spot tile types. Nord Pool entity selection includes current-price sensors without a `raw_today` attribute.
- Currency follows the sensor's `unit_of_measurement`, including `SEK/kWh`. Optional cents display scales the number without changing its currency.
- The graph shows today and tomorrow at the source's available resolution, including 15-minute data when available. Nord Pool's built-in current-price sensors use its `get_prices_for_date` action for the interval lists. Each day is ranked independently: cheapest 30% green, middle 40% graded green–amber–red, priciest 30% red. Past bars are grey.
- Tap a graph interval to show its price and time range in the headline.
- Delivery and sales prices use separate numeric settings. No formulas need to be typed. Tessera's supported tile option kinds allow one selected sensor per tile, so sales pricing uses that sensor plus the configured sales additional cost.

## Configure costs

In the tile inspector, choose the **Entity** for delivery, then set:

- **Tax (%)**: percentage uplift. Enter `25` for 25%. These are free-text fields with up to two whole digits and up to the selected decimal precision.
- **Delivery additional cost**: added after tax, in the selected sensor's currency per kWh. Negative values are allowed.
- **Show sales price**: optionally show the selected sensor's price adjusted for sales.
- **Sales additional cost**: added to that sensor's price, in its currency per kWh.

The calculations are:

- Delivery: `delivery sensor × (1 + tax / 100) + delivery additional cost`
- Sales: `selected sensor + sales additional cost`

For a SEK/kWh sensor, an additional cost of `0.08` means 0.08 SEK/kWh. Costs may be negative.

## Install

1. In Tessera's **Plugins** page, add this repository by link.
2. Place a **Tessa electricity SE** tile in **Layout** and select the matching sensor in **Entity**.
3. In the plugin's screen settings, enter the Nord Pool config entry ID to enable timelines from the built-in current-price sensors. In Home Assistant, open **Developer Tools → Actions**, choose **Nord Pool: Get prices for date**, and inspect the YAML to find `config_entry`.
4. Set the decimal count, tax and additional costs in the tile inspector. The decimal count controls both dashboard price precision and allowed tariff-input precision. Enable **Show sales price** if you want the adjusted price shown.
5. Use a tile at least 2×2 for the graph. Tap a bar to inspect its interval; tap elsewhere to open the full-screen graph card.

The plugin uses Tessera's native plugin API and ESPHome external-component structure, for Tessera-supported boards including the Waveshare ESP32-S3-Touch-LCD-7B.

## Sensor data

The graph reads these list attributes, or for the built-in Nord Pool integration obtains interval lists through Home Assistant's action:

- Nord Pool: `raw_today[*].value`, `raw_tomorrow[*].value`
- ENTSO-e: `prices_today[*].price`, `prices_tomorrow[*].price`
- GE-Spot: `today_interval_prices[*].value`, `tomorrow_interval_prices[*].value`
- EPEX Spot: `data[*].price_per_kwh` with each item's `start_time`

The built-in Nord Pool action needs the screen's config entry ID, market area and currency are taken from the selected current-price sensor, and it returns prices in currency per MWh. The plugin converts them to currency per kWh. Tomorrow's chart appears once Nord Pool publishes those prices. Other entities that expose only a current value and no supported list still cannot provide a timeline. Hourly lists remain hourly; quarter-hour lists retain 15-minute resolution.

## Attribution

This plugin adapts the MIT-licensed Tessera electricity-price plugin by Heronimonimo. See [LICENSE](LICENSE).

