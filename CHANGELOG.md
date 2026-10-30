# Changelog

## 0.1.7

- Clear cached Nord Pool intervals when the selected market area, currency or local date changes.

## 0.1.6

- Fetch built-in Nord Pool interval prices for today and tomorrow through Home Assistant's Nord Pool action.
- Replace tariff number widgets with bounded free-text values and tie input precision to displayed decimals.

## 0.1.5

- Use free-text tax and cost fields with precision bounded by the decimal setting.
- Make the decimal setting control dashboard display precision, including cents mode.

## 0.1.4

- Use only tile option kinds accepted by Tessera's installed validator; sales display uses the selected sensor with its own additional cost.

## 0.1.3

- Keep the sales entity option within the fields accepted by Tessera's tile option validator.

## 0.1.2

- Allow choosing Nord Pool current-price sensors without a `raw_today` attribute.
- Replace typed tariff formulas with tax, additional-cost and optional sales-sensor settings.

## 0.1.1

- Set tile memory declarations to 16,000 bytes, within Tessera's manifest limit.

## 0.1.0

- Initial Tessa electricity SE plugin based on the MIT-licensed Tessera electricity-price plugin.
- Added sensor-native currency display and delivery/sales formulas.
- Added daily price ranking colors, muted past values, and touchable quarter-hour chart slots.

