# m5PaperWeather

An M5Paper landscape dashboard that shows indoor temperature and humidity alongside a three-day weather forecast pulled from [OpenWeatherMap](https://openweathermap.org/).

## Features

- Landscape layout optimized for the 960×540 E-Ink display.
- Current outdoor conditions with descriptive text.
- Indoor temperature and relative humidity sourced from the onboard SHT30 sensor.
- Three-day forecast summary cards using OpenWeatherMap's One Call API.
- Battery gauge indicating the current charge level.
- Power-friendly refresh cadence: twice-daily forecast (Wi‑Fi) and 10‑minute indoor-only updates.

## Work in progress
   - Need to update the graphics and try new fonts.
   - Looking to have the 3 day forecast change when tap on the screen.
   - Possible using SD card for background images for the borders and icons.

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

- Adjust refresh cadence in `src/main.cpp`:
  - `WEATHER_UPDATE_INTERVAL` for forecast fetches (default 12 hours)
  - `INDOOR_UPDATE_INTERVAL` for indoor sensor refreshes (default 10 minutes)
- Modify the drawing functions to tweak fonts, layout, or add more telemetry.
- The battery percentage is derived from the measured voltage; tune the min/max thresholds in `readBatteryLevel()` if you prefer a different calibration.

## Smoother fonts (SD card)

You can enable anti‑aliased TTF/OTF fonts for smoother text rendering.

Steps
- Copy a TrueType/OpenType font to the microSD card at: `/font/Roboto-Regular.ttf`
- Insert the microSD card and reboot the device.
- On boot, the app will automatically load the font from SD. If the file is missing, it falls back to the built‑in bitmap font.

Change the font or path
- Edit the path in `src/main.cpp` to match your font: `FONT_PATH_REGULAR`.
- Example default: `/font/Roboto-Regular.ttf`.

Sizes and tuning
- The app precreates render sizes for the font and maps legacy sizes to pixels:
  - `2 → 26 px`, `3 → 36 px`, `4 → 48 px`, `8 → 84 px`
- To slightly change the large current‑temperature font, edit the mapping for `8` in `mapLegacySizeToPx(...)` inside `src/main.cpp`.
- Alternatively, change the call site used for the big temperature: `src/main.cpp:516` (`setTextSizeCompat(8)`).

Troubleshooting
- If you see messages like `Freetype: Size X not found` or `Render is not available` in the serial log:
  - Ensure the font file exists on the SD card at the configured path.
  - Power cycle after changing fonts.
  - If you changed text sizes, also update the size mapping and ensure those sizes are precreated in `tryLoadSmoothFont()`.

## API usage

The code calls the OpenWeatherMap One Call endpoint over HTTPS. Make sure your OpenWeatherMap account is provisioned for this API and that the API key you hardcode has sufficient quota.
