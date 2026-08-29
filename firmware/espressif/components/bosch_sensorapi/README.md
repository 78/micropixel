# Bosch SensorAPI snapshot

This component contains the minimal upstream sources used by the ESP-Mosaico
BMI270 and BMM150 drivers. The files remain unmodified and retain their
BSD-3-Clause notices.

- BMI270 SensorAPI commit: `41129fcfe39c583ee5462d79195741945d51c1fe`
- BMM150 SensorAPI commit: `0dce0617873cda1f6d51f6b7b961fdc2641e0c7c`

MicroPixel supplies the ESP-IDF I2C callbacks and owns serialization through
its shared board I2C executor; no upstream example or bus abstraction is used.
