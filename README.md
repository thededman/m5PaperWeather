# m5PaperWeather

An M5Paper landscape dashboard that shows indoor temperature and humidity alongside a three-day weather forecast pulled from [OpenWeatherMap](https://openweathermap.org/).

## Features

- Landscape layout optimized for the 960×540 E-Ink display.
- Current outdoor conditions with descriptive text.
- Indoor temperature and relative humidity sourced from the onboard SHT30 sensor.
- Three-day forecast summary cards using OpenWeatherMap's One Call API.
- Battery gauge indicating the current charge level.
- Automatic refresh every 30 minutes with quick status feedback on Wi-Fi or API failures.

## Getting started

1. Install [PlatformIO](https://platformio.org/) (the project uses the Arduino framework for the ESP32-based M5Paper).
2. Open this folder in VS Code with the PlatformIO extension or run the PlatformIO CLI.
3. Edit `src/main.cpp` and replace the placeholder values for:
   - `WIFI_SSID` and `WIFI_PASSWORD`
   - `OPENWEATHERMAP_API_KEY`
   - `OPENWEATHERMAP_LATITUDE` and `OPENWEATHERMAP_LONGITUDE` to match your location
   - Optionally update `OPENWEATHERMAP_LANGUAGE` or units (`metric` by default)
4. Build and upload the firmware:

   ```bash
   pio run --target upload
   ```

5. Monitor the serial output if desired:

   ```bash
   pio device monitor
   ```

The device will try to connect to the configured Wi-Fi network, pull the latest forecast, and render the dashboard. If a connection or API call fails, a status message is shown and another attempt will occur automatically.

## Customisation tips

- Adjust `WEATHER_UPDATE_INTERVAL` in `src/main.cpp` to change how often the display refreshes (default 30 minutes).
- Modify the drawing functions to tweak fonts, layout, or add more telemetry.
- The battery percentage is derived from the measured voltage; tune the min/max thresholds in `readBatteryLevel()` if you prefer a different calibration.

## API usage

The code calls the OpenWeatherMap One Call endpoint over HTTPS. Make sure your OpenWeatherMap account is provisioned for this API and that the API key you hardcode has sufficient quota.
