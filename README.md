# Tessa electricity SE

A Tessera electricity dashboard tile for Home Assistant. Choose a Nord Pool, ENTSO-e, GE-Spot or EPEX Spot sensor, then view the current price and today's and tomorrow's prices in a touchable 15-minute graph.

## Features

- Sensor type is chosen by the tile: Nord Pool, ENTSO-e, GE-Spot or EPEX Spot. For the Nord Pool integration, select the sensor that carries `raw_today` and `raw_tomorrow` (for example, Nord Pool SE3 ▸ Aktuellt pris).
- Currency follows the selected entity's `unit_of_measurement`, including `SEK/kWh`. The optional cents display changes the numeric scale without tying the tile to a particular currency.
- 15-minute chart granularity whenever the sensor provides quarter-hour data. Today's and tomorrow's prices are shown together once tomorrow's list is available. The graph rolls forward with the local calendar day and does not retain a completed day.
- For each day independently, the cheapest 30% is green, the most expensive 30% is red, and the middle 40% uses a continuous green–amber–red gradient. Past bars are muted grey.
- The current price is prominent and colored by its place in that day's prices. Tap a bar to show that slot's price and time range in the headline.
- Configure a delivery price and a sales price formula. Both use `p`, the selected sensor price in its own currency per kWh. The delivery formula is applied to the purchase price; the sales formula is shown as the feed-in value.
- Choose bars or a stepped line. Larger tiles and the full-screen card show the price graph.

## Tariff formulas

The formula field supports `p`, decimal values, parentheses and `+ - * /`. Decimal comma is accepted.

| Setting | Example | Result |
|---|---|---|
| Delivery price | `p*1.25+0.08` | Apply a 25% uplift, then add 0.08 currency units/kWh |
| Sales price | `p+0.025` | Add 0.025 currency units/kWh to the market price |
| Fixed price | `0.42` | Use a fixed price in the sensor currency/kWh |

For a SEK/kWh sensor, `0.08` means 0.08 SEK/kWh. Keep both formulas empty to show the sensor price directly and omit sales pricing.

## Install

1. In Tessera's **Plugins** page, choose **Add with a link** and enter this repository URL.
2. Select the screen(s) where the plugin should be installed and let Tessera build the firmware.
3. In **Layout**, place one of the **Tessa electricity SE** tiles. Pick the matching integration tile, select its price sensor, and set the tariff formulas if needed.
4. Use a tile of at least 2×2 for an embedded graph. Tap a bar to inspect its price and time window; tap outside the bars to open the full-screen graph card.

The plugin uses Tessera's native plugin API and ESPHome external-component structure. It is designed to scale across Tessera-supported boards, including the Waveshare ESP32-S3-Touch-LCD-7B.

## Sensor data

The chosen sensor must expose daily price lists supported by its integration:

- Nord Pool: `raw_today[*].value`, `raw_tomorrow[*].value`
- ENTSO-e: `prices_today[*].price`, `prices_tomorrow[*].price`
- GE-Spot: `today_interval_prices[*].value`, `tomorrow_interval_prices[*].value`
- EPEX Spot: `data[*].price_per_kwh` with each item's `start_time`

If a source publishes hourly prices, the plugin keeps that hourly resolution. Quarter-hour granularity is available when the selected sensor supplies quarter-hour values.

## Attribution

This plugin adapts the MIT-licensed Tessera electricity-price plugin by Heronimonimo. See [LICENSE](LICENSE).

