#include <M5EPD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cctype>
#include <type_traits>
#include <limits>
#include <SD.h>

// Forward declarations for functions defined later but used early
int mapLegacySizeToPx(int legacy);
void setTextSizeCompat(int size);
void tryLoadSmoothFont();

namespace
{
// Defaults used if SD config is missing
constexpr char DEFAULT_WIFI_SSID[] = "SSID";
constexpr char DEFAULT_WIFI_PASSWORD[] = "SSID_PASSWORD";
constexpr char DEFAULT_OWM_API_KEY[] = "APK_KEY";
constexpr float DEFAULT_OWM_LATITUDE = 0.0F;
constexpr float DEFAULT_OWM_LONGITUDE = 0.0F;
constexpr char DEFAULT_OWM_UNITS[] = "imperial";
constexpr char DEFAULT_OWM_LANGUAGE[] = "en";
constexpr uint32_t DEFAULT_WEATHER_UPDATE_INTERVAL = 12UL * 60UL * 60UL * 1000UL; // 12 hours
constexpr uint32_t DEFAULT_INDOOR_UPDATE_INTERVAL = 10UL * 60UL * 1000UL; // 10 minutes

// Runtime-configurable settings (loaded from SD if present)
String CFG_WIFI_SSID = DEFAULT_WIFI_SSID;
String CFG_WIFI_PASSWORD = DEFAULT_WIFI_PASSWORD;
String CFG_OWM_API_KEY = DEFAULT_OWM_API_KEY;
float CFG_OWM_LATITUDE = DEFAULT_OWM_LATITUDE;
float CFG_OWM_LONGITUDE = DEFAULT_OWM_LONGITUDE;
String CFG_OWM_UNITS = DEFAULT_OWM_UNITS;
String CFG_OWM_LANGUAGE = DEFAULT_OWM_LANGUAGE;
uint32_t CFG_WEATHER_UPDATE_INTERVAL = DEFAULT_WEATHER_UPDATE_INTERVAL;
uint32_t CFG_INDOOR_UPDATE_INTERVAL = DEFAULT_INDOOR_UPDATE_INTERVAL;
constexpr uint16_t CANVAS_WIDTH = 960;
constexpr uint16_t CANVAS_HEIGHT = 540;
constexpr uint8_t DISPLAY_ROTATION = 0;
constexpr uint8_t COLOR_WHITE = 0;
constexpr uint8_t COLOR_BLACK = 15;
// Optional TrueType/OpenType font on SD for smoother text rendering.
constexpr char FONT_PATH_REGULAR[] = "/font/Roboto-Regular.ttf"; // place on SD card

struct DailyForecast
{
    time_t timestamp{};
    float minTemperature{NAN};
    float maxTemperature{NAN};
    String summary;
    String iconCode;
    int iconId{0};
};

struct WeatherSnapshot
{
    float outdoorTemperature{NAN};
    String outdoorDescription;
    DailyForecast days[3];
    time_t updatedAt{};
    String currentIconCode;
    int currentIconId{0};
};

struct DayAggregate
{
    float minTemperature{std::numeric_limits<float>::infinity()};
    float maxTemperature{-std::numeric_limits<float>::infinity()};
    String description;
    String iconCode;
    int iconId{0};
    time_t localTimestamp{};
    int yyyymmdd{0};
    bool hasData{false};
};

M5EPD_Canvas canvas(&M5.EPD);
bool canvasReady = false;
bool fontReady = false;
bool sdReady = false;
WeatherSnapshot latestWeather;
uint32_t lastWeatherUpdate = 0;
uint32_t lastIndoorUpdate = 0;
String lastErrorMessage;
// UI mode: 0 = main dashboard, 1..3 = detailed forecast for day index-1
uint8_t uiMode = 0;
bool wasTouching = false;
uint32_t lastTouchTime = 0;
bool pendingFullRefresh = false;

// Forward declare renderDisplay so renderUi can call it before definition
void renderDisplay(float indoorTemp, float indoorHumidity, bool indoorValid);

// Ensure SD is initialised before asset loads
bool ensureSdReady()
{
    if (sdReady)
    {
        return true;
    }
    if (SD.begin())
    {
        sdReady = true;
        return true;
    }
    return false;
}

String iconPathForOwmId(int id)
{
    if (id >= 200 && id < 300) return String("/icons/thunder.png");
    if (id >= 300 && id < 400) return String("/icons/drizzle.png");
    if (id >= 500 && id < 600) return String("/icons/rain.png");
    if (id >= 600 && id < 700) return String("/icons/snow.png");
    if (id >= 700 && id < 800) return String("/icons/fog.png");
    if (id == 800) return String("/icons/clear.png");
    if (id == 801) return String("/icons/partly_cloudy.png");
    if (id >= 802 && id <= 804) return String("/icons/clouds.png");
    return String("/icons/na.png");
}

// -------- Configuration loading (from SD /config/weather.json) --------
constexpr char CONFIG_PATH[] = "/config/weather.json";

void applyConfigDefaults()
{
    CFG_WIFI_SSID = DEFAULT_WIFI_SSID;
    CFG_WIFI_PASSWORD = DEFAULT_WIFI_PASSWORD;
    CFG_OWM_API_KEY = DEFAULT_OWM_API_KEY;
    CFG_OWM_LATITUDE = DEFAULT_OWM_LATITUDE;
    CFG_OWM_LONGITUDE = DEFAULT_OWM_LONGITUDE;
    CFG_OWM_UNITS = DEFAULT_OWM_UNITS;
    CFG_OWM_LANGUAGE = DEFAULT_OWM_LANGUAGE;
    CFG_WEATHER_UPDATE_INTERVAL = DEFAULT_WEATHER_UPDATE_INTERVAL;
    CFG_INDOOR_UPDATE_INTERVAL = DEFAULT_INDOOR_UPDATE_INTERVAL;
}

bool loadConfigFromSD()
{
    applyConfigDefaults();
    if (!ensureSdReady())
    {
        Serial.println("[Config] SD not ready; using defaults.");
        return false;
    }
    if (!SD.exists(CONFIG_PATH))
    {
        Serial.println("[Config] No /config/weather.json; using defaults.");
        return false;
    }
    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (!f)
    {
        Serial.println("[Config] Failed to open config; using defaults.");
        return false;
    }
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
        Serial.printf("[Config] JSON parse error: %s; using defaults.\n", err.c_str());
        return false;
    }
    // Map fields with fallbacks
    JsonObject wifi = doc["wifi"].as<JsonObject>();
    if (!wifi.isNull())
    {
        if (wifi["ssid"]) CFG_WIFI_SSID = String(wifi["ssid"].as<const char*>());
        if (wifi["password"]) CFG_WIFI_PASSWORD = String(wifi["password"].as<const char*>());
    }
    JsonObject owm = doc["openweathermap"].as<JsonObject>();
    if (!owm.isNull())
    {
        if (owm["apiKey"]) CFG_OWM_API_KEY = String(owm["apiKey"].as<const char*>());
        if (owm["lat"]) CFG_OWM_LATITUDE = owm["lat"].as<float>();
        if (owm["lon"]) CFG_OWM_LONGITUDE = owm["lon"].as<float>();
        if (owm["units"]) CFG_OWM_UNITS = String(owm["units"].as<const char*>());
        if (owm["lang"]) CFG_OWM_LANGUAGE = String(owm["lang"].as<const char*>());
    }
    JsonObject upd = doc["update"].as<JsonObject>();
    if (!upd.isNull())
    {
        if (upd["weatherHours"]) CFG_WEATHER_UPDATE_INTERVAL = (uint32_t)(upd["weatherHours"].as<uint32_t>() * 60UL * 60UL * 1000UL);
        if (upd["indoorMinutes"]) CFG_INDOOR_UPDATE_INTERVAL = (uint32_t)(upd["indoorMinutes"].as<uint32_t>() * 60UL * 1000UL);
    }
    Serial.println("[Config] Loaded configuration from SD.");
    return true;
}

bool drawWeatherIcon(int id, int x, int y, int maxW, int maxH)
{
    if (!ensureSdReady())
    {
        return false;
    }
    String path = iconPathForOwmId(id);
    if (!SD.exists(path))
    {
        Serial.printf("[Icon] Missing asset: %s\n", path.c_str());
        return false;
    }
    // Try PNG first
    if (path.endsWith(".png"))
    {
        return canvas.drawPngFile(SD, path.c_str(), x, y, maxW, maxH, 0, 0, 1.0, 127);
    }
    // Fallback BMP/JPG
    if (path.endsWith(".bmp"))
    {
        return canvas.drawBmpFile(SD, path.c_str(), x, y);
    }
    return canvas.drawJpgFile(SD, path.c_str(), x, y, maxW, maxH);
}

String owmIconPath(const String &code)
{
    return String("/icons/") + code + ".png";
}

String owmIconUrl(const String &code)
{
    return String("http://openweathermap.org/img/wn/") + code + "@2x.png";
}

bool ensureIconCached(const String &code)
{
    if (code.length() == 0)
    {
        return false;
    }
    if (!ensureSdReady())
    {
        return false;
    }
    SD.mkdir("/icons");
    const String path = owmIconPath(code);
    if (SD.exists(path))
    {
        return true;
    }
    Serial.printf("[Icon] Downloading %s -> %s\n", code.c_str(), path.c_str());

    HTTPClient http;
    const String url = owmIconUrl(code);
    http.setTimeout(7000);
    if (!http.begin(url))
    {
        Serial.println("[Icon] HTTP begin failed");
        return false;
    }
    const int codeHttp = http.GET();
    if (codeHttp != HTTP_CODE_OK)
    {
        Serial.printf("[Icon] HTTP %d for %s\n", codeHttp, url.c_str());
        http.end();
        return false;
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f)
    {
        Serial.println("[Icon] SD open failed");
        http.end();
        return false;
    }
    const size_t written = http.writeToStream(&f);
    f.close();
    http.end();
    Serial.printf("[Icon] Saved %u bytes to %s\n", (unsigned)written, path.c_str());
    return written > 0;
}

bool drawOwmIcon(const String &code, int x, int y, int maxW, int maxH)
{
    if (code.length() == 0)
    {
        return false;
    }
    if (!ensureSdReady())
    {
        return false;
    }
    const String path = owmIconPath(code);
    if (!SD.exists(path))
    {
        return false;
    }
    return canvas.drawPngFile(SD, path.c_str(), x, y, maxW, maxH, 0, 0, 1.0, 127);
}

bool connectToWifi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.printf("[WiFi] Already connected to %s\n", WiFi.SSID().c_str());
        return true;
    }

    Serial.println("[WiFi] Connecting to configured network...");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(CFG_WIFI_SSID.c_str(), CFG_WIFI_PASSWORD.c_str());

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - start > 30000UL)
        {
            Serial.println("[WiFi] Connection timed out; will retry later.");
            WiFi.disconnect(true);
            return false;
        }
        delay(500);
    }

    Serial.printf("[WiFi] Connected to %s\n", WiFi.SSID().c_str());
    return true;
}

void powerDownWifi()
{
    if (WiFi.getMode() == WIFI_MODE_NULL)
    {
        return;
    }

    Serial.println("[WiFi] Disabling radio to conserve power.");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
    WiFi.setSleep(true);
}

String buildApiUrl()
{
    String url = "https://api.openweathermap.org/data/2.5/onecall?lat=";
    url += String(CFG_OWM_LATITUDE, 6);
    url += "&lon=";
    url += String(CFG_OWM_LONGITUDE, 6);
    url += "&exclude=minutely,hourly,alerts&units=";
    url += CFG_OWM_UNITS;
    url += "&lang=";
    url += CFG_OWM_LANGUAGE;
    url += "&appid=";
    url += CFG_OWM_API_KEY;
    return url;
}

template <typename Sensor>
auto tryInitializeSensor(Sensor &sensor, int) -> decltype(sensor.Begin(), bool())
{
    sensor.Begin();
    return true;
}

template <typename Sensor>
auto tryInitializeSensor(Sensor &sensor, long) -> decltype(sensor.begin(), bool())
{
    sensor.begin();
    return true;
}

template <typename Sensor>
bool tryInitializeSensor(Sensor &, ...)
{
    return false;
}

void initIndoorSensor()
{
    auto &sensor = M5.SHT30;
    (void)tryInitializeSensor(sensor, 0);
}

String capitalizeWords(const String &text)
{
    String result = text;
    bool capitalizeNext = true;
    for (size_t i = 0; i < result.length(); ++i)
    {
        char &c = result[i];
        const unsigned char uc = static_cast<unsigned char>(c);
        if (isalpha(uc))
        {
            c = static_cast<char>(capitalizeNext ? toupper(uc) : tolower(uc));
            capitalizeNext = false;
        }
        else
        {
            capitalizeNext = true;
        }
    }
    return result;
}

String formatDayOfWeek(time_t timestamp)
{
    struct tm timeInfo;
    gmtime_r(&timestamp, &timeInfo);
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%a", &timeInfo);
    return String(buffer);
}

String formatTimestamp(time_t timestamp)
{
    struct tm timeInfo;
    gmtime_r(&timestamp, &timeInfo);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%d %b %H:%M", &timeInfo);
    return String(buffer);
}

template <typename Sensor>
auto tryReadWithGetTempData(Sensor &sensor, float &temperature, float &humidity, int)
    -> decltype(sensor.GetTempData(&temperature, &humidity), bool())
{
    return sensor.GetTempData(&temperature, &humidity);
}

template <typename Sensor>
bool tryReadWithGetTempData(Sensor &, float &, float &, ...)
{
    return false;
}

template <typename Sensor>
auto updateSensorReadingImpl(Sensor &sensor, std::true_type) -> bool
{
    sensor.UpdateData();
    return true;
}

template <typename Sensor>
auto updateSensorReadingImpl(Sensor &sensor, std::false_type) -> bool
{
    const auto status = sensor.UpdateData();
    return (status == true) || (status == 0);
}

template <typename Sensor>
auto updateSensorReading(Sensor &sensor, int) -> decltype(sensor.UpdateData(), bool())
{
    using ReturnType = decltype(sensor.UpdateData());
    return updateSensorReadingImpl(sensor, std::is_void<ReturnType>{});
}

template <typename Sensor>
bool updateSensorReading(Sensor &, ...)
{
    return true;
}

template <typename Sensor>
auto fetchTemperature(Sensor &sensor, int) -> decltype(sensor.GetTemperature(), float())
{
    return sensor.GetTemperature();
}

template <typename Sensor>
auto fetchTemperature(Sensor &sensor, long) -> decltype(sensor.getTemperature(), float())
{
    return sensor.getTemperature();
}

template <typename Sensor>
float fetchTemperature(Sensor &, ...)
{
    return NAN;
}

template <typename Sensor>
auto fetchHumidity(Sensor &sensor, int) -> decltype(sensor.GetRelHumidity(), float())
{
    return sensor.GetRelHumidity();
}

template <typename Sensor>
auto fetchHumidity(Sensor &sensor, long) -> decltype(sensor.GetHumidity(), float())
{
    return sensor.GetHumidity();
}

template <typename Sensor>
auto fetchHumidity(Sensor &sensor, double) -> decltype(sensor.getHumidity(), float())
{
    return sensor.getHumidity();
}

template <typename Sensor>
float fetchHumidity(Sensor &, ...)
{
    return NAN;
}

bool readIndoorClimate(float &temperature, float &humidity)
{
    auto &sensor = M5.SHT30;

    if (tryReadWithGetTempData(sensor, temperature, humidity, 0))
    {
        temperature = temperature * 9.0F / 5.0F + 32.0F;
        return true;
    }

    if (!updateSensorReading(sensor, 0))
    {
        return false;
    }

    const float tempC = fetchTemperature(sensor, 0);
    const float hum = fetchHumidity(sensor, 0);

    if (std::isnan(tempC) || std::isnan(hum))
    {
        return false;
    }

    temperature = tempC * 9.0F / 5.0F + 32.0F;
    humidity = hum;
    return true;
}

float readBatteryLevel()
{
    const float voltage = M5.getBatteryVoltage() / 1000.0F; // convert mV -> V
    float percentage = (voltage - 3.0F) / (4.2F - 3.0F);
    percentage = constrain(percentage, 0.0F, 1.0F);
    return percentage * 100.0F;
}

void drawBatteryIndicator(float level)
{
    constexpr int indicatorWidth = 120;
    constexpr int indicatorHeight = 36;
    const int x = CANVAS_WIDTH - indicatorWidth - 30;
    const int y = 20;

    canvas.drawRoundRect(x, y, indicatorWidth, indicatorHeight, 6, COLOR_BLACK);
    canvas.drawRect(x + indicatorWidth, y + indicatorHeight / 2 - 6, 6, 12, COLOR_BLACK);

    const int innerWidth = indicatorWidth - 14;
    const int innerHeight = indicatorHeight - 14;
    const int innerX = x + 7;
    const int innerY = y + 7;
    const int fillWidth = static_cast<int>((innerWidth) * (level / 100.0F));

    canvas.drawRect(innerX, innerY, innerWidth, innerHeight, COLOR_BLACK);
    if (fillWidth > 0)
    {
        canvas.fillRect(innerX, innerY, fillWidth, innerHeight, COLOR_BLACK);
    }

    canvas.setTextDatum(MC_DATUM);
    setTextSizeCompat(2);
    canvas.drawString(String(static_cast<int>(level + 0.5F)) + "%", x + indicatorWidth / 2, y + indicatorHeight / 2);
    canvas.setTextDatum(TL_DATUM);
}

void renderStatusMessage(const String &message)
{
    if (!canvasReady)
    {
        Serial.printf("[Display] Skipping status render (canvas unavailable): %s\n", message.c_str());
        return;
    }

    canvas.fillCanvas(COLOR_WHITE);
    canvas.setTextColor(COLOR_BLACK);
    canvas.setTextDatum(MC_DATUM);
    setTextSizeCompat(3);
    canvas.drawString(message, CANVAS_WIDTH / 2, CANVAS_HEIGHT / 2);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    canvas.setTextDatum(TL_DATUM);
}

int calculateDegreeRadius() noexcept
{
    const int textHeight = canvas.fontHeight();
    return std::max(2, textHeight / 10);
}

int calculateDegreeCenterY(int startY, int radius) noexcept
{
    return startY + radius + std::max(0, canvas.fontHeight() / 12);
}

void drawDegreesForText(const String &text, int16_t startX, int16_t startY)
{
    const int radius = calculateDegreeRadius();
    const int centerY = calculateDegreeCenterY(startY, radius);
    int searchPos = 0;
    while (true)
    {
        const int fIndex = text.indexOf('F', searchPos);
        if (fIndex == -1)
        {
            break;
        }
        if (fIndex == 0 || text.charAt(fIndex - 1) != ' ')
        {
            searchPos = fIndex + 1;
            continue;
        }

        const String prefix = text.substring(0, fIndex - 1);
        const int prefixWidth = canvas.textWidth(prefix);
        const int spaceWidth = canvas.textWidth(" ");
        const int fStartX = startX + prefixWidth + spaceWidth;
        const int availableSpace = std::max(1, spaceWidth - 1);
        const int offset = std::min(radius + 1, availableSpace);
        const int centerX = fStartX - offset;

        if (radius <= 2)
        {
            canvas.fillCircle(centerX, centerY, radius, COLOR_BLACK);
        }
        else
        {
            canvas.fillCircle(centerX, centerY, radius, COLOR_BLACK);
            canvas.fillCircle(centerX, centerY, radius - 1, COLOR_WHITE);
        }

        searchPos = fIndex + 1;
    }
}

void drawStringWithDegrees(const String &text, int16_t startX, int16_t startY)
{
    canvas.drawString(text, startX, startY);
}

void pushCanvasSmart()
{
    m5epd_update_mode_t mode = pendingFullRefresh ? UPDATE_MODE_GL16 : UPDATE_MODE_GC16;
    canvas.pushCanvas(0, 0, mode);
    pendingFullRefresh = false;
}

void renderForecastDetail(int dayIndex, float indoorTemp, float indoorHumidity, bool indoorValid)
{
    if (!canvasReady)
    {
        Serial.println("[Display] Skipping detail render because canvas is not ready.");
        return;
    }

    canvas.fillCanvas(COLOR_WHITE);
    canvas.setTextColor(COLOR_BLACK);
    canvas.setTextDatum(TL_DATUM);

    // Safety: bound index
    if (dayIndex < 0) dayIndex = 0;
    if (dayIndex > 2) dayIndex = 2;

    const DailyForecast &forecast = latestWeather.days[dayIndex];

    // Header
    setTextSizeCompat(4);
    const String title = String("Forecast: ") + formatDayOfWeek(forecast.timestamp);
    canvas.drawString(title, 30, 30);

    // Timestamp of last weather update
    setTextSizeCompat(2);
    const String updatedText = latestWeather.updatedAt != 0 ? formatTimestamp(latestWeather.updatedAt) : String("Pending");
    canvas.drawString(String("Updated: ") + updatedText, 30, 80);

    // Indoor quick status on the right
    setTextSizeCompat(2);
    const int indoorTextY = 80;
    if (indoorValid)
    {
        const String indoorLine = "Indoor: " + String(indoorTemp, 1) + " F  " + String(indoorHumidity, 1) + "% RH";
        const int indoorWidth = canvas.textWidth(indoorLine);
        const int indoorDrawX = CANVAS_WIDTH - 30 - indoorWidth;
        drawStringWithDegrees(indoorLine, indoorDrawX, indoorTextY);
    }
    else
    {
        const String indoorMessage = "Indoor sensor not available";
        const int indoorWidth = canvas.textWidth(indoorMessage);
        const int indoorDrawX = CANVAS_WIDTH - 30 - indoorWidth;
        canvas.drawString(indoorMessage, indoorDrawX, indoorTextY);
    }

    // Temperatures — use large value font and compute dynamic spacing to avoid overlap
    setTextSizeCompat(7); // slightly smaller than main big temp
    const int valueHeight = canvas.fontHeight();
    const int yHigh = 160;
    const int yLow = yHigh + valueHeight + 30; // spacing below high value
    String hiStr = std::isnan(forecast.maxTemperature) ? String("-- F") : String(forecast.maxTemperature, 1) + " F";
    String loStr = std::isnan(forecast.minTemperature) ? String("-- F") : String(forecast.minTemperature, 1) + " F";

    // Labels in smaller font
    setTextSizeCompat(3);
    canvas.drawString("High:", 30, yHigh);
    canvas.drawString("Low:", 30, yLow);

    // Values in large font
    setTextSizeCompat(7);
    drawStringWithDegrees(hiStr, 180, yHigh);
    drawStringWithDegrees(loStr, 180, yLow);

    // Weather icon on the right
    const int iconBoxX = CANVAS_WIDTH - 200;
    const int iconBoxY = 140;
    const int iconBoxW = 150;
    const int iconBoxH = 150;
    if (forecast.iconCode.length() > 0)
    {
        if (!drawOwmIcon(forecast.iconCode, iconBoxX, iconBoxY, iconBoxW, iconBoxH))
        {
            if (forecast.iconId > 0)
            {
                drawWeatherIcon(forecast.iconId, iconBoxX, iconBoxY, iconBoxW, iconBoxH);
            }
        }
    }

    // Summary, wrapped
    setTextSizeCompat(3);
    const String summary = forecast.summary.length() > 0 ? capitalizeWords(forecast.summary) : String("No summary available");
    const int summaryX = 30;
    int summaryY = 300;
    const int maxSummaryWidth = CANVAS_WIDTH - 60;
    String line;
    int index = 0;
    while (index < summary.length())
    {
        while (index < summary.length() && summary[index] == ' ')
        {
            ++index;
        }
        if (index >= summary.length())
        {
            break;
        }
        int nextSpace = summary.indexOf(' ', index);
        if (nextSpace == -1)
        {
            nextSpace = summary.length();
        }
        const String word = summary.substring(index, nextSpace);
        const String candidate = line.length() > 0 ? line + ' ' + word : word;
        if (line.length() == 0 || canvas.textWidth(candidate) <= maxSummaryWidth)
        {
            line = candidate;
        }
        else
        {
            canvas.drawString(line, summaryX, summaryY);
            line = word;
            summaryY += 28;
        }
        index = nextSpace + 1;
    }
    if (line.length() > 0)
    {
        canvas.drawString(line, summaryX, summaryY);
    }

    // Footer hint
    setTextSizeCompat(2);
    const String hint = "Tap to cycle days — tap again to return";
    const int hintWidth = canvas.textWidth(hint);
    canvas.setTextDatum(BC_DATUM);
    canvas.drawString(hint, CANVAS_WIDTH / 2, CANVAS_HEIGHT - 16);
    canvas.setTextDatum(TL_DATUM);

    pushCanvasSmart();
}

void renderUi(float indoorTemp, float indoorHumidity, bool indoorValid)
{
    if (uiMode == 0)
    {
        renderDisplay(indoorTemp, indoorHumidity, indoorValid);
    }
    else
    {
        const int dayIndex = static_cast<int>(uiMode) - 1;
        renderForecastDetail(dayIndex, indoorTemp, indoorHumidity, indoorValid);
    }
}

void refreshDisplayForUiChange()
{
    float indoorTemp = NAN;
    float indoorHumidity = NAN;
    const bool indoorValid = readIndoorClimate(indoorTemp, indoorHumidity);
    renderUi(indoorTemp, indoorHumidity, indoorValid);
}
void drawForecastCards()
{
    constexpr int baseY = 360;
    constexpr int cardWidth = 280;
    constexpr int cardHeight = 150;
    constexpr int spacing = 20;
    const int startX = 30;
    for (int i = 0; i < 3; ++i)
    {
        const int x = startX + i * (cardWidth + spacing);
        canvas.drawRoundRect(x, baseY, cardWidth, cardHeight, 12, COLOR_BLACK);

        const DailyForecast &forecast = latestWeather.days[i];
        if (forecast.timestamp == 0)
        {
            continue;
        }

        setTextSizeCompat(3);
        canvas.drawString(formatDayOfWeek(forecast.timestamp), x + 20, baseY + 16);

        setTextSizeCompat(3);
        String tempText;
        if (std::isnan(forecast.maxTemperature) || std::isnan(forecast.minTemperature))
        {
            tempText = "-- F / -- F";
        }
        else
        {
            tempText = String(forecast.maxTemperature, 1) + "F / " + String(forecast.minTemperature, 1) + " F";
        }
        drawStringWithDegrees(tempText, x + 20, baseY + 56);

        setTextSizeCompat(2);
        const String summary = forecast.summary.length() > 0 ? capitalizeWords(forecast.summary) : String("--");
        const int summaryX = x + 20;
        int summaryY = baseY + 96;
        const int maxSummaryWidth = cardWidth - 40;
        const int maxSummaryBottom = baseY + cardHeight - 16;

        String line;
        int index = 0;
        while (index < summary.length() && summaryY <= maxSummaryBottom)
        {
            while (index < summary.length() && summary[index] == ' ')
            {
                ++index;
            }
            if (index >= summary.length())
            {
                break;
            }
            int nextSpace = summary.indexOf(' ', index);
            if (nextSpace == -1)
            {
                nextSpace = summary.length();
            }
            const String word = summary.substring(index, nextSpace);
            const String candidate = line.length() > 0 ? line + ' ' + word : word;
            if (line.length() == 0 || canvas.textWidth(candidate) <= maxSummaryWidth)
            {
                line = candidate;
            }
            else
            {
                canvas.drawString(line, summaryX, summaryY);
                line = word;
                summaryY += 22;
            }
            index = nextSpace + 1;
        }
        if (line.length() > 0 && summaryY <= maxSummaryBottom)
        {
            canvas.drawString(line, summaryX, summaryY);
        }
    }
}

void renderDisplay(float indoorTemp, float indoorHumidity, bool indoorValid)
{
    if (!canvasReady)
    {
        Serial.println("[Display] Skipping full render because canvas is not ready.");
        return;
    }

    canvas.fillCanvas(COLOR_WHITE);
    canvas.setTextColor(COLOR_BLACK);
    canvas.setTextDatum(TL_DATUM);

    setTextSizeCompat(4);
    canvas.drawString("Home Weather Dashboard", 30, 30);

    setTextSizeCompat(2);
    canvas.drawString(String("WiFi: ") + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("Disconnected")), 30, 90);
    const String updatedText = latestWeather.updatedAt != 0 ? formatTimestamp(latestWeather.updatedAt) : String("Pending");
    canvas.drawString(String("Updated: ") + updatedText, 30, 130);

    drawBatteryIndicator(readBatteryLevel());

    setTextSizeCompat(8);
    if (std::isnan(latestWeather.outdoorTemperature))
    {
        drawStringWithDegrees(String("--.- F"), 30, 190);
    }
    else
    {
        drawStringWithDegrees(String(latestWeather.outdoorTemperature, 1) + " F", 30, 190);
    }

    setTextSizeCompat(3);
    const String description = latestWeather.outdoorDescription.length() > 0 ? capitalizeWords(latestWeather.outdoorDescription) : String("Waiting for data");
    canvas.drawString(description, 30, 260);

    setTextSizeCompat(3);
    const int indoorTextY = 90;
    if (indoorValid)
    {
        const String indoorLine = "Indoor: " + String(indoorTemp, 1) + " F  " + String(indoorHumidity, 1) + "% RH";
        const int indoorWidth = canvas.textWidth(indoorLine);
        const int indoorDrawX = CANVAS_WIDTH - 30 - indoorWidth;
        drawStringWithDegrees(indoorLine, indoorDrawX, indoorTextY);
    }
    else
    {
        const String indoorMessage = "Indoor sensor not available";
        const int indoorWidth = canvas.textWidth(indoorMessage);
        const int indoorDrawX = CANVAS_WIDTH - 30 - indoorWidth;
        canvas.drawString(indoorMessage, indoorDrawX, indoorTextY);
    }

    setTextSizeCompat(3);
    canvas.drawString("3-Day Forecast", 30, 330);

    drawForecastCards();
    
    pushCanvasSmart();
}

bool fetchWeather()
{
    Serial.println("[Weather] Requesting latest conditions from OpenWeather...");
    lastErrorMessage.clear();

    WiFiClientSecure client;
    client.setInsecure();

    // HTTP client for current + forecast requests
    HTTPClient http;
    http.setTimeout(12000);
    
    const String currentUrl = String("https://api.openweathermap.org/data/2.5/weather?lat=") +
                              String(CFG_OWM_LATITUDE, 6) + "&lon=" +
                              String(CFG_OWM_LONGITUDE, 6) + "&units=" +
                              CFG_OWM_UNITS + "&lang=" + CFG_OWM_LANGUAGE + "&appid=" + CFG_OWM_API_KEY;

    if (!http.begin(client, currentUrl))
    {
        lastErrorMessage = "Weather update failed: HTTP client init";
        Serial.println("[Weather] HTTP client failed to initialise (current).");
        return false;
    }

    int currentCode = http.GET();
    Serial.printf("[Weather] Current HTTP status code: %d\n", currentCode);
    if (currentCode <= 0)
    {
        Serial.printf("[Weather] Current HTTP error: %s (%d)\n", http.errorToString(currentCode).c_str(), currentCode);
        http.end();
        http.setTimeout(15000);
        if (!http.begin(client, currentUrl))
        {
            lastErrorMessage = "Weather update failed: HTTP client init (retry)";
            return false;
        }
        currentCode = http.GET();
        Serial.printf("[Weather] Current HTTP retry status: %d\n", currentCode);
    }
    String currentPayload;
    if (currentCode == HTTP_CODE_OK)
    {
        currentPayload = http.getString();
    }
    else
    {
        currentPayload = http.getString();
        if (currentPayload.length() > 0)
        {
            Serial.printf("[Weather] Current response body: %s\n", currentPayload.c_str());
        }
        lastErrorMessage = String("Weather update failed: HTTP ") + currentCode;
        http.end();
        return false;
    }
    http.end();

    DynamicJsonDocument currentDoc(8 * 1024);
    DeserializationError currentErr = deserializeJson(currentDoc, currentPayload);
    if (currentErr)
    {
        Serial.printf("[Weather] Current JSON parse error: %s\n", currentErr.c_str());
        lastErrorMessage = String("Weather update failed: JSON ") + currentErr.c_str();
        return false;
    }

    const int timezoneOffsetSeconds = currentDoc["timezone"].as<int>();
    latestWeather.outdoorTemperature = currentDoc["main"]["temp"].as<float>();
    latestWeather.outdoorDescription = currentDoc["weather"][0]["description"].as<String>();
    latestWeather.currentIconId = currentDoc["weather"][0]["id"].as<int>();
    latestWeather.currentIconCode = currentDoc["weather"][0]["icon"].as<String>();
    latestWeather.updatedAt = currentDoc["dt"].as<long>() + timezoneOffsetSeconds;

    http.setTimeout(12000);
    const String forecastUrl = String("https://api.openweathermap.org/data/2.5/forecast?lat=") +
                               String(CFG_OWM_LATITUDE, 6) + "&lon=" +
                               String(CFG_OWM_LONGITUDE, 6) + "&units=" +
                               CFG_OWM_UNITS + "&lang=" + CFG_OWM_LANGUAGE + "&appid=" + CFG_OWM_API_KEY;

    if (!http.begin(client, forecastUrl))
    {
        lastErrorMessage = "Weather update failed: HTTP client init";
        Serial.println("[Weather] HTTP client failed to initialise (forecast).");
        return false;
    }

    int forecastCode = http.GET();
    Serial.printf("[Weather] Forecast HTTP status code: %d\n", forecastCode);
    if (forecastCode <= 0)
    {
        Serial.printf("[Weather] Forecast HTTP error: %s (%d)\n", http.errorToString(forecastCode).c_str(), forecastCode);
        http.end();
        http.setTimeout(15000);
        if (!http.begin(client, forecastUrl))
        {
            lastErrorMessage = "Weather update failed: HTTP client init (forecast retry)";
            return false;
        }
        forecastCode = http.GET();
        Serial.printf("[Weather] Forecast HTTP retry status: %d\n", forecastCode);
    }
    String forecastPayload;
    if (forecastCode == HTTP_CODE_OK)
    {
        forecastPayload = http.getString();
    }
    else
    {
        forecastPayload = http.getString();
        if (forecastPayload.length() > 0)
        {
            Serial.printf("[Weather] Forecast response body: %s\n", forecastPayload.c_str());
        }
        lastErrorMessage = String("Weather update failed: HTTP ") + forecastCode;
        http.end();
        return false;
    }
    http.end();

    DynamicJsonDocument forecastDoc(32 * 1024);
    DeserializationError forecastErr = deserializeJson(forecastDoc, forecastPayload);
    if (forecastErr)
    {
        Serial.printf("[Weather] Forecast JSON parse error: %s\n", forecastErr.c_str());
        lastErrorMessage = String("Weather update failed: JSON ") + forecastErr.c_str();
        return false;
    }

    JsonArray list = forecastDoc["list"].as<JsonArray>();
    if (list.isNull() || list.size() == 0)
    {
        lastErrorMessage = "Weather update failed: empty forecast";
        return false;
    }

    const int forecastTimezoneOffset = forecastDoc["city"]["timezone"].as<int>();

    auto computeYmd = [](const struct tm &tmInfo) {
        return (tmInfo.tm_year + 1900) * 10000 + (tmInfo.tm_mon + 1) * 100 + tmInfo.tm_mday;
    };

    time_t firstLocalTs = static_cast<time_t>(list[0]["dt"].as<long>() + forecastTimezoneOffset);
    struct tm firstLocalTm;
    gmtime_r(&firstLocalTs, &firstLocalTm);
    const int firstYmd = computeYmd(firstLocalTm);

    DayAggregate aggregates[3];
    int dayCount = 0;

    for (JsonObject entry : list)
    {
        const long dtUtc = entry["dt"].as<long>();
        time_t localTs = static_cast<time_t>(dtUtc + forecastTimezoneOffset);
        struct tm localTm;
        gmtime_r(&localTs, &localTm);
        const int ymd = computeYmd(localTm);

        if (ymd == firstYmd)
        {
            continue;
        }

        int slot = -1;
        for (int i = 0; i < dayCount; ++i)
        {
            if (aggregates[i].yyyymmdd == ymd)
            {
                slot = i;
                break;
            }
        }

        if (slot == -1)
        {
            if (dayCount >= 3)
            {
                break;
            }
            slot = dayCount++;
            aggregates[slot].hasData = true;
            aggregates[slot].localTimestamp = localTs;
            aggregates[slot].yyyymmdd = ymd;
            aggregates[slot].description.clear();
            aggregates[slot].iconId = 0;
            aggregates[slot].minTemperature = std::numeric_limits<float>::infinity();
            aggregates[slot].maxTemperature = -std::numeric_limits<float>::infinity();
        }

        const float temp = entry["main"]["temp"].as<float>();
        const int iconId = entry["weather"][0]["id"].as<int>();
        const char *iconCodeC = entry["weather"][0]["icon"].as<const char *>();
        const char *desc = entry["weather"][0]["description"].as<const char *>();

        if (!std::isnan(temp))
        {
            if (temp < aggregates[slot].minTemperature)
            {
                aggregates[slot].minTemperature = temp;
            }
            if (temp > aggregates[slot].maxTemperature)
            {
                aggregates[slot].maxTemperature = temp;
            }
        }

        if (aggregates[slot].description.length() == 0 && desc != nullptr)
        {
            aggregates[slot].description = desc;
        }
        if (aggregates[slot].iconId == 0 && iconId != 0)
        {
            aggregates[slot].iconId = iconId;
        }
        if (aggregates[slot].iconCode.length() == 0 && iconCodeC != nullptr)
        {
            aggregates[slot].iconCode = String(iconCodeC);
        }
    }

    for (int i = 0; i < 3; ++i)
    {
        DailyForecast &forecast = latestWeather.days[i];
        forecast.timestamp = 0;
        forecast.minTemperature = NAN;
        forecast.maxTemperature = NAN;
        forecast.summary.clear();

        if (!aggregates[i].hasData)
        {
            continue;
        }

        if (aggregates[i].minTemperature != std::numeric_limits<float>::infinity())
        {
            forecast.minTemperature = aggregates[i].minTemperature;
        }
        if (aggregates[i].maxTemperature != -std::numeric_limits<float>::infinity())
        {
            forecast.maxTemperature = aggregates[i].maxTemperature;
        }
        forecast.summary = aggregates[i].description;
        forecast.timestamp = aggregates[i].localTimestamp;
        forecast.iconId = aggregates[i].iconId;
        forecast.iconCode = aggregates[i].iconCode;
    }

    // Cache OWM icons for current and upcoming days while Wi‑Fi is up
    ensureIconCached(latestWeather.currentIconCode);
    for (int i = 0; i < 3; ++i)
    {
        ensureIconCached(latestWeather.days[i].iconCode);
    }

    Serial.println("[Weather] Weather data parsed successfully.");
    return true;
}

void updateWeatherAndDisplay()
{
    Serial.println("[Update] Starting weather refresh cycle...");

    const bool wifiConnected = connectToWifi();
    if (!wifiConnected)
    {
        Serial.println("[Update] WiFi connection failed.");
        renderStatusMessage("WiFi connection failed");
        powerDownWifi();
        return;
    }

    Serial.println("[Update] WiFi connected; fetching weather.");
    if (!fetchWeather())
    {
        Serial.println("[Update] Weather download or parse failed.");
        const String message = lastErrorMessage.length() > 0 ? lastErrorMessage : String("Weather update failed");
        renderStatusMessage(message);
        powerDownWifi();
        return;
    }

    float indoorTemp = NAN;
    float indoorHumidity = NAN;
    const bool indoorValid = readIndoorClimate(indoorTemp, indoorHumidity);

    Serial.println("[Update] Rendering display.");
    renderUi(indoorTemp, indoorHumidity, indoorValid);
    lastWeatherUpdate = millis();
    // Keep indoor timer aligned so we don't immediately trigger an indoor-only refresh.
    lastIndoorUpdate = lastWeatherUpdate;
    Serial.println("[Update] Update cycle complete.");
    powerDownWifi();
}

// Read only the indoor sensor and refresh the display without using WiFi.
void updateIndoorAndDisplay()
{
    Serial.println("[Indoor] Starting indoor-only refresh cycle...");

    float indoorTemp = NAN;
    float indoorHumidity = NAN;
    const bool indoorValid = readIndoorClimate(indoorTemp, indoorHumidity);

    Serial.println("[Indoor] Rendering display with latest weather snapshot.");
    renderUi(indoorTemp, indoorHumidity, indoorValid);
    lastIndoorUpdate = millis();
    Serial.println("[Indoor] Indoor-only update complete.");
}
} // namespace

// Forward declaration so we can call it from setup()
void tryLoadSmoothFont();

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("[Setup] Booting Home Weather Dashboard");

    M5.begin();
    M5.EPD.SetRotation(DISPLAY_ROTATION);
    M5.TP.SetRotation(DISPLAY_ROTATION);
    M5.RTC.begin();

    M5.EPD.Clear(true);

    initIndoorSensor();

    canvasReady = canvas.createCanvas(CANVAS_WIDTH, CANVAS_HEIGHT);
    if (!canvasReady)
    {
        Serial.println("[Setup] Failed to allocate EPD canvas. Display output disabled.");
    }
    else
    {
        canvas.setTextColor(COLOR_BLACK);
        canvas.setTextDatum(TL_DATUM);
        renderStatusMessage("Booting...");
    }

    // Attempt to load a smoother TTF/OTF font from SD card.
    tryLoadSmoothFont();

    // Load runtime configuration from SD (overrides defaults if present)
    loadConfigFromSD();

    updateWeatherAndDisplay();
}

void loop()
{
    const uint32_t now = millis();

    if ((now - lastWeatherUpdate) > CFG_WEATHER_UPDATE_INTERVAL)
    {
        updateWeatherAndDisplay();
    }
    else if ((now - lastIndoorUpdate) > CFG_INDOOR_UPDATE_INTERVAL)
    {
        updateIndoorAndDisplay();
    }

    // Touch handling: use GT911 API (available->update->getFingerNum/readFinger(0))
    M5.update();
    bool touching = false;
    tp_finger_t finger = {0, 0, 0, 0};
    if (M5.TP.available())
    {
        M5.TP.update();
        const uint8_t n = M5.TP.getFingerNum();
        touching = (n > 0);
        if (n > 0)
        {
            finger = M5.TP.readFinger(0);
        }
    }

    if (touching && !wasTouching && (now - lastTouchTime) > 400UL)
    {
        lastTouchTime = now;
        // Cycle UI mode: 0 -> 1 -> 2 -> 3 -> 0
        uiMode = (uiMode + 1) % 4;
        Serial.printf("[Touch] Tap @(%d,%d). Mode -> %u\n", (int)finger.x, (int)finger.y, uiMode);
        // Force a full refresh on the next render to avoid any ghosting between screen modes
        pendingFullRefresh = true;
        refreshDisplayForUiChange();
    }

    wasTouching = touching;
    delay(50);
}
int mapLegacySizeToPx(int legacy)
{
    switch (legacy)
    {
    case 2: return 26;  // small labels
    case 3: return 36;  // medium text
    case 4: return 48;  // headers
    case 8: return 84;  // large temperature (slightly smaller)
    default:
        return legacy * 12; // reasonable fallback scaling
    }
}

void setTextSizeCompat(int size)
{
    if (fontReady)
    {
        canvas.setTextSize(mapLegacySizeToPx(size));
    }
    else
    {
        canvas.setTextSize(size);
    }
}

void tryLoadSmoothFont()
{
    if (!canvasReady)
    {
        return;
    }

    // Initialize SD and try to load a TTF/OTF font if present.
    if (!sdReady && !SD.begin())
    {
        Serial.println("[Font] SD card not available; using default bitmap font.");
        return;
    }
    sdReady = true;

    Serial.printf("[Font] Looking for font: %s\n", FONT_PATH_REGULAR);
    if (!SD.exists(FONT_PATH_REGULAR))
    {
        Serial.println("[Font] Font file not found on SD; using default font.");
        return;
    }

    // M5EPD supports loading TrueType/OpenType fonts from FS.
    // This renders much smoother than the scaled bitmap font.
    canvas.loadFont(FONT_PATH_REGULAR, SD);
    // Pre-create renderers for the sizes we use.
    // The cache size (256) balances memory and speed for repeated glyphs.
    canvas.createRender(mapLegacySizeToPx(2), 256);
    canvas.createRender(mapLegacySizeToPx(3), 256);
    canvas.createRender(mapLegacySizeToPx(4), 256);
    canvas.createRender(mapLegacySizeToPx(8), 256);
    fontReady = true;
    Serial.println("[Font] Smooth font loaded successfully.");
}
