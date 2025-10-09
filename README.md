# m5PaperWeather

An M5Paper landscape dashboard that shows indoor temperature and humidity alongside a three-day weather forecast pulled from [OpenWeatherMap](https://openweathermap.org/). This is the device used for this build. (https://shop.m5stack.com/products/m5paper-esp32-development-kit-v1-1-960x540-4-7-eink-display-235-ppi) Not sure if it will run on m5PaperS3

## Features

- Landscape layout optimized for the 960×540 E-Ink display.
- Current outdoor conditions with descriptive text.
- Indoor temperature and relative humidity sourced from the onboard SHT30 sensor.
- Three-day forecast summary cards using OpenWeatherMap's One Call API.
- Battery gauge indicating the current charge level.
- Power-friendly refresh cadence: twice-daily forecast (Wi‑Fi) and 10‑minute indoor-only updates.
- Tap navigation: cycle views Main → Day 1 → Day 2 → Day 3 → Main with a detailed daily page (high/low and summary).

## Touch Navigation

- Tap anywhere on the screen to cycle views:
  - Main dashboard → Day 1 detail → Day 2 detail → Day 3 detail → back to Main.
- Detail pages show the selected day’s high/low and a wrapped summary, plus indoor temp/RH in the top‑right.
- Debounce is ~400 ms to avoid double taps. You can change this in `src/m5paperWeather.cpp` inside the `loop()` logic.
- To reduce ghosting when switching views, the app performs a one‑time stronger refresh. You can adjust the mode in `pushCanvasSmart()`.


## Getting started

1. Install [PlatformIO](https://platformio.org/) (the project uses the Arduino framework for the ESP32-based M5Paper).
2. Open this folder in VS Code with the PlatformIO extension or run the PlatformIO CLI.
3. Create a config file on the microSD card at `/config/weather.json`:

   ```json
   {
     "wifi": {
       "ssid": "YourSSID",
       "password": "YourPassword"
     },
     "openweathermap": {
       "apiKey": "YOUR_API_KEY",
       "lat": 0.0,
       "lon": 0.0,
       "units": "imperial",
       "lang": "en"
     },
     "update": {
       "weatherHours": 12,
       "indoorMinutes": 10
     }
   }
   ```

   If the file is missing, the app falls back to built‑in defaults.
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

- Adjust refresh cadence in `src/m5paperWeather.cpp`:
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
- Edit the path in `src/m5paperWeather.cpp` to match your font: `FONT_PATH_REGULAR`.
- Example default: `/font/Roboto-Regular.ttf`.

Sizes and tuning
- The app precreates render sizes for the font and maps legacy sizes to pixels:
  - `2 → 26 px`, `3 → 36 px`, `4 → 48 px`, `8 → 84 px`
- To slightly change the large current‑temperature font, edit the mapping for `8` in `mapLegacySizeToPx(...)` inside `src/m5paperWeather.cpp`.
- Alternatively, change the call site used for the big temperature within `renderDisplay(...)` where `setTextSizeCompat(8)` is called.

Troubleshooting
- If you see messages like `Freetype: Size X not found` or `Render is not available` in the serial log:
  - Ensure the font file exists on the SD card at the configured path.
  - Power cycle after changing fonts.
  - If you changed text sizes, also update the size mapping and ensure those sizes are precreated in `tryLoadSmoothFont()`.

## Customisation (advanced)

- Touch behavior: Adjust tap debounce and cycling in `loop()` (`uiMode`, `lastTouchTime`).
- View refresh: Change the one‑shot refresh mode in `pushCanvasSmart()` (e.g., `UPDATE_MODE_GL16`, `GLD16`, `DU`).
- Detail layout: Tweak fonts/positions in `renderForecastDetail(...)`.

## Weather icons (SD card)

You can display grayscale weather icons on the detailed day screens.

Steps
- Create an `icons` folder on the microSD card and copy PNGs (recommended), JPGs or BMPs:
  - `/icons/clear.png` — clear sky (800)
  - `/icons/partly_cloudy.png` — few clouds (801)
  - `/icons/clouds.png` — clouds (802–804)
  - `/icons/rain.png` — rain (500–531)
  - `/icons/drizzle.png` — drizzle (300–321)
  - `/icons/thunder.png` — thunderstorm (200–232)
  - `/icons/snow.png` — snow (600–622)
  - `/icons/fog.png` — atmosphere/mist (700–781)
  - Optional: `/icons/na.png` — fallback icon
- Recommended size: around 120–160 px square. The app fits icons into a ~150×150 px box on the right.

How it works
- The app maps OpenWeatherMap condition `id` to the above filenames.
- Icons are loaded from SD on render. If a file is missing, it logs a message and continues.
- Files can be PNG with transparency; the app uses an alpha threshold to render on E‑Ink.

Auto-download (optional)
- During weather fetch, the app will also cache OpenWeatherMap's official icon PNGs by `icon` code (e.g., `10d`) into `/icons`.
- If a matching file like `/icons/10d.png` does not exist, it downloads from `https://openweathermap.org/img/wn/10d@2x.png` and saves it.
- Detail views prefer these cached icons; if unavailable, they fall back to the custom filenames above if present.

Change mapping
- Adjust `iconPathForOwmId(...)` in `src/m5paperWeather.cpp` to point to your filenames.
- If you prefer embedded icons in flash, we can add PROGMEM bitmaps and a compile‑time switch.

## API usage

The code calls the OpenWeatherMap One Call endpoint over HTTPS. Make sure your OpenWeatherMap account is provisioned for this API and that the API key you hardcode has sufficient quota.
