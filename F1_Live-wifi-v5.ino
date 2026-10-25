// =====================================================================
//  F1 LIVE — табло на ESP32 + TFT ST7789 (240x320)
//  Все данные берутся из интернета, перепрошивать ничего не нужно.
//
//  Источники:
//    • Время .................. NTP (pool.ntp.org)
//    • Часовой пояс ........... по IP (ip-api.com)
//    • Календарь, очки,
//      результаты, круги ...... api.jolpi.ca (открытое API Formula 1)
//    • Цвета команд ........... api.openf1.org
//    • Схемы трасс ............ github.com/bacinger/f1-circuits
//
//  Wi-Fi настраивается с телефона: при первом запуске (или если сеть
//  недоступна) табло само создаёт точку доступа "F1-Timer".
//
//  Кнопка (внешняя на BTN_EXT, кнопка BOOT на плате работает так же):
//    одно нажатие ...... следующий этап / страница таблицы
//    двойное нажатие ... следующий режим
//    удержание 0.6–5 с . обновить данные сейчас
//    BOOT дольше 5 с ... забыть Wi-Fi и перезагрузиться
//
//  Внешняя кнопка: одна ножка -> BTN_EXT, ножка по диагонали -> GND
//
//  Режимы: COUNTDOWN -> SCHEDULE -> TRACK -> STANDINGS
//
//  Библиотеки: Adafruit ST7735 and ST7789, Adafruit GFX,
//              ArduinoJson (v7), WiFiManager (tzapu)
//  Подключение: SCL->18, SDA->23, RES->17, DC->16, CS->5, VDD/BL->3V3
// =====================================================================

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_CS   5
#define TFT_DC   16
#define TFT_RST  17
#define BTN      0        // кнопка BOOT на плате
#define BTN_EXT  21       // внешняя кнопка (можно поменять на любой свободный пин)

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

#define F1_RED  0xE8E4
#define GREY    0x8410
#define DIM     0x4208
#define DARK    0x2104
#define GREEN   0x07E8

const char* AP_NAME = "F1-Timer";
const unsigned long REFRESH_MS      = 30UL * 60UL * 1000UL;   // обычно — раз в 30 минут
const unsigned long REFRESH_LIVE_MS =  5UL * 60UL * 1000UL;   // во время уик-энда — раз в 5 минут

// =====================================================================
//  Данные из интернета
// =====================================================================
#define MAX_RACES   30
#define MAX_SESS    5
#define MAX_DRIVERS 30
#define MAX_LOCS    64
#define MAX_COLORS  32
#define MAX_TRACK   260

struct Session { const char* label; time_t start; int durMin; };

struct Race {
  int round;
  char name[24];        // "UNITED STATES"
  char circuitId[20];
  char circuit[56];     // "CIRCUIT OF THE AMERICAS, AUSTIN"
  float lat, lon;
  Session s[MAX_SESS];
  int nSess;
};

struct Driver { char name[14]; char code[4]; char team[14]; int points; uint16_t color; };
struct Loc    { char id[10]; float lat, lon; };            // индекс схем трасс
struct TeamColor { char code[4]; char team[14]; uint16_t color; };

Race races[MAX_RACES];       int raceCount = 0;
Driver drivers[MAX_DRIVERS]; int driverCount = 0;
Loc locs[MAX_LOCS];          int locCount = 0;
TeamColor colors[MAX_COLORS]; int colorCount = 0;

char season[8] = "";
int  standingsRound = 0;
int  lastRound = 0;                // последний этап с опубликованными результатами
char lastWinner[16] = "";
char lastRaceName[24] = "";
char tzLabel[16] = "MSK";

// Схема трассы (загружается при просмотре, хранится последняя)
char    trackFor[20] = "";         // для какого circuitId загружена
bool    trackOk = false;
int16_t trackPts[MAX_TRACK * 2];
int     trackN = 0;
char    trackLength[20] = "";
char    trackLaps[6] = "";
char    trackWinner[16] = "";
int     trackPrevYear = 0;

bool haveData = false;
unsigned long lastFetch = 0;

// Распознавание жестов: одно / двойное нажатие / удержание
struct Button {
  int pin;
  bool canResetWiFi;          // только BOOT умеет сбрасывать Wi-Fi
  bool pressed;
  unsigned long pressStart;
  unsigned long lastRelease;
  int clicks;
};

Button btnBoot = {BTN,     true,  false, 0, 0, 0};
Button btnExt  = {BTN_EXT, false, false, 0, 0, 0};

const unsigned long DEBOUNCE_MS     = 40;    // защита от дребезга контактов
const unsigned long HOLD_MS         = 600;   // дольше — это удержание
const unsigned long DOUBLE_GAP_MS   = 300;   // пауза, за которую ждём второе нажатие
const unsigned long RESET_WIFI_MS   = 5000;

// =====================================================================
//  Состояние интерфейса
// =====================================================================
enum Mode { COUNTDOWN, SCHEDULE, TRACK, STANDINGS, MODE_COUNT };
int mode = COUNTDOWN;
int viewRace = 0;
int standPage = 0;
const int PER_PAGE = 6;

float carDist = 0, trackLen = 0;
int carX = -1, carY = -1;
unsigned long lastFrame = 0, lastTick = 0;
int cdKey = -1;

// =====================================================================
//  Вспомогательные функции
// =====================================================================
uint16_t rgbHex(const char* hex) {           // "F47600" -> RGB565
  long c = strtol(hex, nullptr, 16);
  return tft.color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// Цвет "по умолчанию" для команды, которой нет в OpenF1: стабильный по названию
uint16_t fallbackColor(const char* s) {
  static const uint16_t pal[] = {0xFD20, 0x07FF, 0xF81F, 0xAFE5, 0x7BEF, 0xFFE0, 0x5D1C, 0xFB56};
  unsigned h = 0;
  while (*s) h = h * 31 + (unsigned char)*s++;
  return pal[h % 8];
}

// Верхний регистр + замена букв с диакритикой (шрифт экрана их не знает)
void asciiUpper(char* dst, const char* src, int maxLen) {
  int j = 0;
  for (int i = 0; src[i] && j < maxLen - 1; i++) {
    unsigned char c = src[i];
    if (c < 0x80) { dst[j++] = toupper(c); continue; }
    if (c == 0xC3 && src[i + 1]) {
      unsigned char d = (unsigned char)src[++i] & 0xDF;
      char r = 0;
      if      (d >= 0x80 && d <= 0x85) r = 'A';
      else if (d == 0x87)              r = 'C';
      else if (d >= 0x88 && d <= 0x8B) r = 'E';
      else if (d >= 0x8C && d <= 0x8F) r = 'I';
      else if (d == 0x91)              r = 'N';
      else if ((d >= 0x92 && d <= 0x96) || d == 0x98) r = 'O';
      else if (d >= 0x99 && d <= 0x9C) r = 'U';
      else if (d == 0x9D || d == 0x9F) r = 'Y';
      if (r) dst[j++] = r;
      continue;
    }
    while ((src[i + 1] & 0xC0) == 0x80) i++;
  }
  dst[j] = 0;
}

// "Alpine F1 Team" -> "Alpine", "Red Bull Racing" -> "Red Bull"
void cleanTeam(char* dst, const char* src, int maxLen) {
  char tmp[40];
  strncpy(tmp, src, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
  const char* suffixes[] = {" F1 Team", " Team", " Racing"};
  for (auto suf : suffixes) {
    int lt = strlen(tmp), ls = strlen(suf);
    if (lt > ls && strcmp(tmp + lt - ls, suf) == 0) tmp[lt - ls] = 0;
  }
  strncpy(dst, tmp, maxLen - 1); dst[maxLen - 1] = 0;
}

void raceTitle(char* dst, const char* raceName, int maxLen) {
  char tmp[48];
  asciiUpper(tmp, raceName, sizeof(tmp));
  char* gp = strstr(tmp, " GRAND PRIX");
  if (gp) *gp = 0;
  strncpy(dst, tmp, maxLen - 1); dst[maxLen - 1] = 0;
}

long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

time_t parseUtc(const char* date, const char* tim) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (!date || sscanf(date, "%d-%d-%d", &y, &mo, &d) != 3) return 0;
  if (tim) sscanf(tim, "%d:%d:%d", &h, &mi, &s);
  return (time_t)daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s;
}

void fmtDay(time_t t, char* buf, int len)  { struct tm tm; localtime_r(&t, &tm); strftime(buf, len, "%a %d.%m", &tm); }
void fmtTime(time_t t, char* buf, int len) { struct tm tm; localtime_r(&t, &tm); strftime(buf, len, "%H:%M", &tm); }

time_t nowUtc() { return time(nullptr); }
bool timeValid() { return nowUtc() > 1700000000; }

// ---- Логика сессий ----
bool isRaceSession(const Race& r, int i) { return i == r.nSess - 1; }

// Гонка закончилась, когда опубликованы её результаты (или прошло 4 часа)
bool raceFinished(const Race& r) {
  time_t start = r.s[r.nSess - 1].start;
  return lastRound >= r.round || nowUtc() > start + 4 * 3600;
}

bool sessionDone(const Race& r, int i) {
  if (isRaceSession(r, i)) return raceFinished(r) && nowUtc() >= r.s[i].start + 30 * 60;
  return nowUtc() >= r.s[i].start + r.s[i].durMin * 60;
}

bool sessionLive(const Race& r, int i) {
  return nowUtc() >= r.s[i].start && !sessionDone(r, i);
}

// Этап остаётся "текущим" до 4 часов после старта гонки — после финиша
// на главном экране успевает побыть победитель
int currentRaceIndex() {
  time_t now = nowUtc();
  for (int i = 0; i < raceCount; i++) {
    Race& r = races[i];
    if (!r.nSess) continue;
    time_t start = r.s[r.nSess - 1].start;
    if (!raceFinished(r) || now < start + 4 * 3600) return i;
  }
  return -1;
}

bool isLive(const Race& r) {
  time_t now = nowUtc();
  return r.nSess && now >= r.s[0].start - 3600 && !raceFinished(r);
}

int lastSeasonRound() { return raceCount ? races[raceCount - 1].round : 0; }

// =====================================================================
//  Сеть
// =====================================================================
// Скачивает ответ целиком и разбирает JSON по фильтру
bool fetchJson(const char* url, JsonDocument& doc, JsonDocument& filter) {
  bool secure = strncmp(url, "https", 5) == 0;
  WiFiClientSecure sclient;
  WiFiClient pclient;
  if (secure) { sclient.setInsecure(); sclient.setTimeout(15); }

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool ok = secure ? http.begin(sclient, url) : http.begin(pclient, url);
  if (!ok) return false;
  http.addHeader("Accept-Encoding", "identity");

  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP %d: %s\n", code, url);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  Serial.printf("Получено %u байт: %s\n", body.length(), url);

  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    Serial.println(body.substring(0, 200));
  }
  return !err;
}

// Часовой пояс по IP-адресу (летнее/зимнее время учтётся при следующем обновлении)
bool fetchTimezone() {
  JsonDocument filter;
  filter["status"] = true;
  filter["offset"] = true;
  JsonDocument doc;
  if (!fetchJson("http://ip-api.com/json/?fields=status,offset", doc, filter)) return false;
  if (strcmp(doc["status"] | "", "success") != 0) return false;

  long off = doc["offset"] | 10800;            // секунды от UTC
  int h = off / 3600, m = labs(off % 3600) / 60;
  char tz[20];
  // в POSIX-формате знак обратный: UTC+3 записывается как "UTC-3"
  if (m) snprintf(tz, sizeof(tz), "UTC%+d:%02d", -h, m);
  else   snprintf(tz, sizeof(tz), "UTC%+d", -h);
  setenv("TZ", tz, 1);
  tzset();

  if (off == 10800) strcpy(tzLabel, "MSK");
  else if (m)       snprintf(tzLabel, sizeof(tzLabel), "UTC%+d:%02d", h, m);
  else              snprintf(tzLabel, sizeof(tzLabel), "UTC%+d", h);
  Serial.printf("Часовой пояс: %s\n", tzLabel);
  return true;
}

struct SessKey { const char* key; const char* label; int dur; };
const SessKey SESS_KEYS[] = {
  {"FirstPractice",    "FP1",    60},
  {"SecondPractice",   "FP2",    60},
  {"ThirdPractice",    "FP3",    60},
  {"SprintQualifying", "SPR Q",  45},
  {"Sprint",           "SPRINT", 60},
  {"Qualifying",       "QUALI",  60},
};

bool fetchSchedule() {
  JsonDocument filter;
  filter["MRData"]["RaceTable"]["season"] = true;
  JsonObject f = filter["MRData"]["RaceTable"]["Races"].add<JsonObject>();
  f["round"] = true; f["raceName"] = true; f["date"] = true; f["time"] = true;
  f["Circuit"]["circuitId"] = true;
  f["Circuit"]["circuitName"] = true;
  f["Circuit"]["Location"]["locality"] = true;
  f["Circuit"]["Location"]["lat"] = true;
  f["Circuit"]["Location"]["long"] = true;
  for (auto& k : SESS_KEYS) f[k.key] = true;

  JsonDocument doc;
  if (!fetchJson("https://api.jolpi.ca/ergast/f1/current.json", doc, filter)) return false;

  JsonArray arr = doc["MRData"]["RaceTable"]["Races"];
  if (arr.size() == 0) return false;
  strncpy(season, doc["MRData"]["RaceTable"]["season"] | "", sizeof(season) - 1);

  raceCount = 0;
  for (JsonObject r : arr) {
    if (raceCount >= MAX_RACES) break;
    Race& R = races[raceCount];
    R.round = atoi(r["round"] | "0");
    raceTitle(R.name, r["raceName"] | "", sizeof(R.name));

    strncpy(R.circuitId, r["Circuit"]["circuitId"] | "", sizeof(R.circuitId) - 1);
    R.circuitId[sizeof(R.circuitId) - 1] = 0;
    char cname[34], loc[20];
    asciiUpper(cname, r["Circuit"]["circuitName"] | "", sizeof(cname));
    asciiUpper(loc, r["Circuit"]["Location"]["locality"] | "", sizeof(loc));
    snprintf(R.circuit, sizeof(R.circuit), "%s, %s", cname, loc);
    R.lat = atof(r["Circuit"]["Location"]["lat"] | "0");
    R.lon = atof(r["Circuit"]["Location"]["long"] | "0");

    R.nSess = 0;
    for (auto& k : SESS_KEYS) {
      if (r[k.key].isNull() || R.nSess >= MAX_SESS - 1) continue;
      R.s[R.nSess++] = { k.label, parseUtc(r[k.key]["date"].as<const char*>(),
                                           r[k.key]["time"].as<const char*>()), k.dur };
    }
    for (int a = 1; a < R.nSess; a++)
      for (int b = a; b > 0 && R.s[b].start < R.s[b - 1].start; b--) {
        Session t = R.s[b]; R.s[b] = R.s[b - 1]; R.s[b - 1] = t;
      }
    R.s[R.nSess++] = { "RACE", parseUtc(r["date"].as<const char*>(), r["time"].as<const char*>()), 120 };
    raceCount++;
  }
  Serial.printf("Сезон %s, этапов: %d\n", season, raceCount);
  return true;
}

// Цвета команд из OpenF1: код пилота (VER) -> цвет и название команды
bool fetchTeamColors() {
  JsonDocument filter;
  JsonObject f = filter.add<JsonObject>();
  f["name_acronym"] = true;
  f["team_name"] = true;
  f["team_colour"] = true;

  JsonDocument doc;
  if (!fetchJson("https://api.openf1.org/v1/drivers?session_key=latest", doc, filter)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() == 0) return false;

  colorCount = 0;
  for (JsonObject d : arr) {
    if (colorCount >= MAX_COLORS) break;
    const char* code = d["name_acronym"] | "";
    const char* col  = d["team_colour"] | "";
    if (!code[0] || !col[0]) continue;
    TeamColor& c = colors[colorCount++];
    strncpy(c.code, code, 3); c.code[3] = 0;
    cleanTeam(c.team, d["team_name"] | "", sizeof(c.team));
    c.color = rgbHex(col);
  }
  Serial.printf("Цветов команд: %d\n", colorCount);
  return true;
}

void applyTeamColors() {
  for (int i = 0; i < driverCount; i++) {
    Driver& D = drivers[i];
    D.color = fallbackColor(D.team);
    for (int c = 0; c < colorCount; c++)
      if (strcmp(colors[c].code, D.code) == 0) {
        D.color = colors[c].color;
        if (colors[c].team[0]) strcpy(D.team, colors[c].team);
      }
  }
}

bool fetchStandings() {
  JsonDocument filter;
  JsonObject sl = filter["MRData"]["StandingsTable"]["StandingsLists"].add<JsonObject>();
  sl["round"] = true;
  JsonObject ds = sl["DriverStandings"].add<JsonObject>();
  ds["points"] = true;
  ds["Driver"]["familyName"] = true;
  ds["Driver"]["code"] = true;
  ds["Constructors"].add<JsonObject>()["name"] = true;

  JsonDocument doc;
  if (!fetchJson("https://api.jolpi.ca/ergast/f1/current/driverStandings.json", doc, filter)) return false;

  JsonObject list = doc["MRData"]["StandingsTable"]["StandingsLists"][0];
  if (list.isNull()) return false;
  standingsRound = atoi(list["round"] | "0");

  driverCount = 0;
  for (JsonObject d : list["DriverStandings"].as<JsonArray>()) {
    if (driverCount >= MAX_DRIVERS) break;
    Driver& D = drivers[driverCount++];
    asciiUpper(D.name, d["Driver"]["familyName"] | "?", sizeof(D.name));
    strncpy(D.code, d["Driver"]["code"] | "", 3); D.code[3] = 0;
    cleanTeam(D.team, d["Constructors"][0]["name"] | "", sizeof(D.team));
    D.points = atoi(d["points"] | "0");
  }
  Serial.printf("Пилотов: %d (после этапа %d)\n", driverCount, standingsRound);
  return true;
}

bool fetchLastWinner() {
  JsonDocument filter;
  JsonObject r = filter["MRData"]["RaceTable"]["Races"].add<JsonObject>();
  r["round"] = true;
  r["raceName"] = true;
  r["Results"].add<JsonObject>()["Driver"]["familyName"] = true;

  JsonDocument doc;
  if (!fetchJson("https://api.jolpi.ca/ergast/f1/current/last/results.json?limit=1", doc, filter)) return false;
  JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
  if (race.isNull()) { lastRound = 0; lastWinner[0] = 0; return true; }   // сезон ещё не начался
  lastRound = atoi(race["round"] | "0");
  asciiUpper(lastWinner, race["Results"][0]["Driver"]["familyName"] | "", sizeof(lastWinner));
  raceTitle(lastRaceName, race["raceName"] | "", sizeof(lastRaceName));
  return true;
}

// Индекс схем трасс: id файла и координаты
bool fetchTrackIndex() {
  JsonDocument filter;
  JsonObject f = filter.add<JsonObject>();
  f["id"] = true; f["lat"] = true; f["lon"] = true;

  JsonDocument doc;
  if (!fetchJson("https://raw.githubusercontent.com/bacinger/f1-circuits/master/f1-locations.json",
                 doc, filter)) return false;
  locCount = 0;
  for (JsonObject l : doc.as<JsonArray>()) {
    if (locCount >= MAX_LOCS) break;
    Loc& L = locs[locCount++];
    strncpy(L.id, l["id"] | "", sizeof(L.id) - 1); L.id[sizeof(L.id) - 1] = 0;
    L.lat = l["lat"] | 0.0f;
    L.lon = l["lon"] | 0.0f;
  }
  Serial.printf("Схем трасс в индексе: %d\n", locCount);
  return locCount > 0;
}

// Ближайшая к трассе схема (в пределах 5 км)
const char* findTrackFile(const Race& r) {
  int best = -1;
  float bestD = 1e9;
  for (int i = 0; i < locCount; i++) {
    float dx = (locs[i].lon - r.lon) * cosf(r.lat * M_PI / 180.0f) * 111.0f;
    float dy = (locs[i].lat - r.lat) * 111.0f;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < bestD) { bestD = d; best = i; }
  }
  return (best >= 0 && bestD < 5.0f) ? locs[best].id : nullptr;
}

// Загрузка схемы трассы + длина круга, число кругов и победитель прошлого года
bool loadTrack(const Race& r) {
  if (strcmp(trackFor, r.circuitId) == 0) return trackOk;
  strcpy(trackFor, r.circuitId);
  trackOk = false;
  trackN = 0;
  trackLength[0] = trackLaps[0] = trackWinner[0] = 0;

  if (!locCount) fetchTrackIndex();
  const char* file = findTrackFile(r);
  if (file) {
    char url[128];
    snprintf(url, sizeof(url),
             "https://raw.githubusercontent.com/bacinger/f1-circuits/master/circuits/%s.geojson", file);
    JsonDocument filter;
    JsonObject feat = filter["features"].add<JsonObject>();
    feat["properties"]["length"] = true;
    feat["geometry"]["coordinates"] = true;
    JsonDocument doc;
    if (fetchJson(url, doc, filter)) {
      JsonObject f0 = doc["features"][0];
      int len = f0["properties"]["length"] | 0;
      if (len) snprintf(trackLength, sizeof(trackLength), "%d.%03d km", len / 1000, len % 1000);

      JsonArray c = f0["geometry"]["coordinates"];
      int n = c.size();
      int step = (n + MAX_TRACK - 2) / (MAX_TRACK - 1);   // прореживание, если точек слишком много
      if (step < 1) step = 1;

      // проекция: долгота сжимается на cos(широты), Y вниз
      float k = cosf(r.lat * M_PI / 180.0f);
      float minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
      for (int i = 0; i < n; i++) {
        float x = c[i][0].as<float>() * k, y = -c[i][1].as<float>();
        minX = min(minX, x); maxX = max(maxX, x);
        minY = min(minY, y); maxY = max(maxY, y);
      }
      const float BX = 14, BY = 70, BW = 292, BH = 140;
      float s = min(BW / (maxX - minX), BH / (maxY - minY));
      float ox = BX + (BW - (maxX - minX) * s) / 2;
      float oy = BY + (BH - (maxY - minY) * s) / 2;
      for (int i = 0; i < n && trackN < MAX_TRACK - 1; i += step) {
        trackPts[trackN * 2]     = ox + (c[i][0].as<float>() * k - minX) * s;
        trackPts[trackN * 2 + 1] = oy + (-c[i][1].as<float>() - minY) * s;
        trackN++;
      }
      // замыкаем контур
      trackPts[trackN * 2] = trackPts[0];
      trackPts[trackN * 2 + 1] = trackPts[1];
      trackN++;
      trackOk = trackN > 3;
    }
  }

  // Прошлый год на этой трассе: победитель и число кругов
  trackPrevYear = atoi(season) - 1;
  char url[128];
  snprintf(url, sizeof(url), "https://api.jolpi.ca/ergast/f1/%d/circuits/%s/results/1.json",
           trackPrevYear, r.circuitId);
  JsonDocument filter;
  JsonObject rr = filter["MRData"]["RaceTable"]["Races"].add<JsonObject>();
  JsonObject res = rr["Results"].add<JsonObject>();
  res["laps"] = true;
  res["Driver"]["familyName"] = true;
  JsonDocument doc;
  if (fetchJson(url, doc, filter)) {
    JsonObject w = doc["MRData"]["RaceTable"]["Races"][0]["Results"][0];
    if (!w.isNull()) {
      strncpy(trackLaps, w["laps"] | "", sizeof(trackLaps) - 1);
      asciiUpper(trackWinner, w["Driver"]["familyName"] | "", sizeof(trackWinner));
    }
  }
  return trackOk;
}

void statusLine(const char* msg) {
  tft.fillRect(0, 140, 320, 20, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(GREY);
  tft.setCursor((320 - (int)strlen(msg) * 12) / 2, 142);
  tft.print(msg);
  Serial.println(msg);
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.reconnect();                              // сеть, сохранённая через WiFiManager
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

bool refreshData() {
  if (!ensureWiFi()) { Serial.println("Нет Wi-Fi"); return haveData; }
  fetchTimezone();
  bool ok = fetchSchedule();
  fetchLastWinner();
  if (fetchStandings()) {
    fetchTeamColors();          // если OpenF1 недоступен — остаются прошлые цвета
    applyTeamColors();
  }
  if (!locCount) fetchTrackIndex();
  if (ok) haveData = true;
  lastFetch = millis();
  return haveData;
}

// "Отпечаток" всех данных на экране: если он не изменился, перерисовывать нечего
uint32_t dataSignature() {
  uint32_t h = 2166136261u;
  auto mix = [&](const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  };
  auto mixStr = [&](const char* s) { mix(s, strlen(s)); };
  mixStr(season); mixStr(tzLabel); mixStr(lastWinner);
  mix(&lastRound, sizeof(lastRound)); mix(&standingsRound, sizeof(standingsRound));
  for (int i = 0; i < raceCount; i++) {
    mixStr(races[i].name);
    for (int k = 0; k < races[i].nSess; k++) mix(&races[i].s[k].start, sizeof(time_t));
  }
  for (int i = 0; i < driverCount; i++) {
    mixStr(drivers[i].name); mixStr(drivers[i].team);
    mix(&drivers[i].points, sizeof(int)); mix(&drivers[i].color, sizeof(uint16_t));
  }
  return h;
}

unsigned long refreshInterval() {
  int ri = currentRaceIndex();
  return (ri >= 0 && isLive(races[ri])) ? REFRESH_LIVE_MS : REFRESH_MS;
}

// =====================================================================
//  Общие элементы экрана
// =====================================================================
void drawHeader(const char* left, const char* right) {
  tft.fillRect(0, 0, 320, 30, F1_RED);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print(left);
  tft.setCursor(312 - strlen(right) * 12, 8);
  tft.print(right);
}

void headerSeason(const char* right) {
  char left[16];
  snprintf(left, sizeof(left), "F1 %s", season);
  drawHeader(left, right);
}

void drawDots(int count, int active) {
  int x0 = 312 - (count - 1) * 12;
  for (int i = 0; i < count; i++) {
    int x = x0 + i * 12;
    tft.fillCircle(x, 231, 3, ST77XX_BLACK);
    if (i == active) tft.fillCircle(x, 231, 3, ST77XX_WHITE);
    else             tft.drawCircle(x, 231, 3, GREY);
  }
}

void drawFooterHint(const char* hint) {
  tft.setTextSize(1);
  tft.setTextColor(GREY);
  tft.setCursor(8, 228);
  tft.print(hint);
}

void drawLiveBadge(int x, int y) {
  tft.fillRoundRect(x, y, 56, 22, 4, F1_RED);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(x + 5, y + 4);
  tft.print("LIVE");
}

void drawRaceTitle(const Race& r, int y) {
  int len = strlen(r.name);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(len <= 13 ? 3 : 2);
  tft.setCursor(8, len <= 13 ? y : y + 4);
  tft.print(r.name);
  if (isLive(r)) drawLiveBadge(256, y);
}

void roundLabel(const Race& r, char* buf, int len) {
  snprintf(buf, len, "ROUND %d/%d", r.round, lastSeasonRound());
}

void centerText(const char* s, int y, int size, uint16_t fg, uint16_t bg) {
  tft.setTextSize(size);
  tft.setTextColor(fg, bg);
  tft.setCursor((320 - (int)strlen(s) * 6 * size) / 2, y);
  tft.print(s);
}

// =====================================================================
//  Режим 1: обратный отсчёт
// =====================================================================
// Ближайшая сессия: индекс и признак "идёт сейчас"; -1 — всё закончилось
int targetSession(const Race& r, bool& live) {
  for (int i = 0; i < r.nSess; i++) {
    if (sessionLive(r, i))  { live = true;  return i; }
    if (!sessionDone(r, i)) { live = false; return i; }
  }
  live = false;
  return -1;
}

void drawSeasonOver() {
  tft.fillScreen(ST77XX_BLACK);
  headerSeason("FINISHED");
  centerText("SEASON OVER", 70, 3, ST77XX_WHITE, ST77XX_BLACK);
  if (driverCount) {
    centerText("CHAMPION", 120, 2, GREY, ST77XX_BLACK);
    centerText(drivers[0].name, 146, 3, ST77XX_YELLOW, ST77XX_BLACK);
  }
  drawFooterHint("2x tap: mode");
  drawDots(MODE_COUNT, mode);
}

void drawCountdownStatic(int ri, int si, bool live) {
  Race& r = races[ri];
  char buf[48];
  tft.fillScreen(ST77XX_BLACK);
  roundLabel(r, buf, sizeof(buf));
  headerSeason(buf);
  drawRaceTitle(r, 40);

  tft.setTextSize(1);
  tft.setTextColor(GREY);
  tft.setCursor(8, 70);
  tft.print(r.circuit);
  tft.drawFastHLine(0, 84, 320, GREY);

  if (si < 0) {
    // гонка завершена — показываем победителя, пока этап "текущий"
    centerText("RACE FINISHED", 100, 2, GREY, ST77XX_BLACK);
    centerText("WINNER", 132, 2, GREY, ST77XX_BLACK);
    centerText(lastRound == r.round ? lastWinner : "...", 160, 3, ST77XX_YELLOW, ST77XX_BLACK);
  } else {
    tft.setTextSize(2);
    tft.setTextColor(live ? GREEN : GREY);
    tft.setCursor(8, 96);
    tft.print(live ? "LIVE NOW" : "NEXT SESSION");
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(312 - strlen(r.s[si].label) * 12, 96);
    tft.print(r.s[si].label);

    char d[16], t[8];
    fmtDay(r.s[si].start, d, sizeof(d));
    fmtTime(r.s[si].start, t, sizeof(t));
    snprintf(buf, sizeof(buf), "%s  %s %s", d, t, tzLabel);
    centerText(buf, 176, 2, GREY, ST77XX_BLACK);
  }

  tft.drawFastHLine(0, 200, 320, DIM);
  if (lastWinner[0]) {
    tft.setTextSize(1);
    tft.setTextColor(GREY);
    tft.setCursor(8, 208);
    tft.print("Last winner: ");
    tft.setTextColor(ST77XX_YELLOW);
    tft.print(lastWinner);
    tft.setTextColor(GREY);
    tft.print("  (");
    tft.print(lastRaceName);
    tft.print(")");
  }
  drawDots(MODE_COUNT, mode);
}

void tickCountdown() {
  int ri = currentRaceIndex();
  if (ri < 0) {
    if (cdKey != -2) { cdKey = -2; drawSeasonOver(); }
    return;
  }
  bool live;
  int si = targetSession(races[ri], live);
  int key = ri * 100 + (si + 1) * 2 + (live ? 1 : 0) + (lastRound == races[ri].round ? 50 : 0);
  if (key != cdKey) { cdKey = key; drawCountdownStatic(ri, si, live); }

  time_t now = nowUtc();
  if (si >= 0) {
    const Session& s = races[ri].s[si];
    long diff = live ? (long)(now - s.start) : (long)(s.start - now);
    if (diff < 0) diff = 0;
    long days = diff / 86400, h = (diff / 3600) % 24, m = (diff / 60) % 60, sec = diff % 60;
    char buf[24];
    if (live)            snprintf(buf, sizeof(buf), "+%02ld:%02ld:%02ld", diff / 3600, m, sec);
    else if (days >= 10) snprintf(buf, sizeof(buf), "%ldd %02ld:%02ld", days, h, m);
    else if (days > 0)   snprintf(buf, sizeof(buf), " %ldd %02ld:%02ld ", days, h, m);
    else                 snprintf(buf, sizeof(buf), " %02ld:%02ld:%02ld ", h, m, sec);
    centerText(buf, 124, 5, live ? GREEN : ST77XX_WHITE, ST77XX_BLACK);
  }

  struct tm tm; localtime_r(&now, &tm);
  char clk[24];
  strftime(clk, sizeof(clk), "%H:%M:%S ", &tm);
  tft.setTextSize(1);
  tft.setTextColor(GREY, ST77XX_BLACK);
  tft.setCursor(8, 228);
  tft.print(clk);
  tft.print(tzLabel);
  tft.print(WiFi.status() == WL_CONNECTED ? "  WiFi   " : "  no WiFi");
}

// =====================================================================
//  Режим 2: расписание
// =====================================================================
void drawSchedule() {
  Race& r = races[viewRace];
  char buf[40];
  tft.fillScreen(ST77XX_BLACK);
  roundLabel(r, buf, sizeof(buf));
  headerSeason(buf);
  drawRaceTitle(r, 40);

  tft.setTextSize(1);
  tft.setTextColor(GREY);
  tft.setCursor(8, 70);
  tft.print(r.circuit);
  tft.drawFastHLine(0, 84, 320, GREY);

  bool nextMarked = false;
  tft.setTextSize(2);
  for (int i = 0; i < r.nSess; i++) {
    const Session& s = r.s[i];
    int y = 94 + i * 26;
    bool isRace = isRaceSession(r, i);
    bool done = sessionDone(r, i);
    bool live = sessionLive(r, i);
    bool next = !done && !live && !nextMarked;
    if (next || live) nextMarked = true;

    uint16_t bg = ST77XX_BLACK;
    if (live)            bg = 0x0320;
    else if (isRace)     bg = done ? 0x5000 : F1_RED;
    else if (i % 2 == 0) bg = DARK;
    if (bg != ST77XX_BLACK) tft.fillRect(0, y - 4, 320, 24, bg);

    char d[16], t[8];
    fmtDay(s.start, d, sizeof(d));
    fmtTime(s.start, t, sizeof(t));
    uint16_t doneCol = isRace ? GREY : DIM;

    tft.setTextColor(done ? doneCol : ST77XX_WHITE);
    tft.setCursor(8, y);
    tft.print(s.label);

    tft.setTextColor(done ? doneCol : (isRace || live ? ST77XX_WHITE : GREY));
    tft.setCursor(96, y);
    tft.print(d);

    tft.setTextColor(done ? doneCol : (isRace || live ? ST77XX_WHITE : ST77XX_YELLOW));
    tft.setCursor(248, y);
    tft.print(live ? " NOW" : t);

    if (next && !isRace) tft.fillTriangle(232, y + 2, 232, y + 12, 240, y + 7, ST77XX_YELLOW);
  }

  snprintf(buf, sizeof(buf), "%s | tap: next GP", tzLabel);
  drawFooterHint(buf);
  drawDots(MODE_COUNT, mode);
}

// =====================================================================
//  Режим 3: схема трассы
// =====================================================================
void drawSegment(int i, uint16_t color) {
  int x0 = trackPts[i * 2], y0 = trackPts[i * 2 + 1];
  int x1 = trackPts[i * 2 + 2], y1 = trackPts[i * 2 + 3];
  tft.drawLine(x0, y0, x1, y1, color);
  tft.drawLine(x0 + 1, y0, x1 + 1, y1, color);
  tft.drawLine(x0, y0 + 1, x1, y1 + 1, color);
}

void drawStartMark() {
  tft.fillCircle(trackPts[0], trackPts[1], 4, ST77XX_WHITE);
  tft.fillCircle(trackPts[0], trackPts[1], 2, F1_RED);
}

void pointAt(float dist, int& x, int& y) {
  for (int i = 0; i < trackN - 1; i++) {
    float dx = trackPts[i * 2 + 2] - trackPts[i * 2];
    float dy = trackPts[i * 2 + 3] - trackPts[i * 2 + 1];
    float seg = sqrtf(dx * dx + dy * dy);
    if (dist <= seg || i == trackN - 2) {
      float t = seg > 0 ? dist / seg : 0;
      if (t > 1) t = 1;
      x = trackPts[i * 2] + dx * t;
      y = trackPts[i * 2 + 1] + dy * t;
      return;
    }
    dist -= seg;
  }
}

void drawTrack() {
  Race& r = races[viewRace];
  char buf[40];
  tft.fillScreen(ST77XX_BLACK);
  roundLabel(r, buf, sizeof(buf));
  drawHeader("TRACK", buf);

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 38);
  tft.print(r.name);
  if (isLive(r)) drawLiveBadge(256, 36);

  carX = carY = -1;
  if (strcmp(trackFor, r.circuitId) != 0) {
    centerText("Loading map...", 132, 2, GREY, ST77XX_BLACK);
    loadTrack(r);
    tft.fillRect(0, 60, 320, 160, ST77XX_BLACK);
  }

  // длина | круги | победитель прошлого года
  tft.setTextSize(1);
  tft.setTextColor(GREY);
  tft.setCursor(8, 58);
  if (trackLength[0]) { tft.print(trackLength); tft.print(" | "); }
  if (trackLaps[0])   { tft.print(trackLaps);   tft.print(" laps | "); }
  tft.print(trackPrevYear);
  tft.print(": ");
  tft.setTextColor(ST77XX_YELLOW);
  tft.print(trackWinner[0] ? trackWinner : "new track");

  if (!trackOk) {
    centerText("No map for this track", 132, 1, GREY, ST77XX_BLACK);
  } else {
    trackLen = 0;
    for (int i = 0; i < trackN - 1; i++) {
      drawSegment(i, ST77XX_WHITE);
      float dx = trackPts[i * 2 + 2] - trackPts[i * 2];
      float dy = trackPts[i * 2 + 3] - trackPts[i * 2 + 1];
      trackLen += sqrtf(dx * dx + dy * dy);
    }
    drawStartMark();
    carDist = 0;
  }
  drawFooterHint("tap: next GP");
  drawDots(MODE_COUNT, mode);
}

void animateCar() {
  if (!trackOk || millis() - lastFrame < 30) return;
  lastFrame = millis();
  const int R = 6;
  if (carX >= 0) {
    tft.fillCircle(carX, carY, 4, ST77XX_BLACK);
    for (int i = 0; i < trackN - 1; i++) {
      int x0 = trackPts[i * 2], y0 = trackPts[i * 2 + 1];
      int x1 = trackPts[i * 2 + 2], y1 = trackPts[i * 2 + 3];
      if (carX < min(x0, x1) - R || carX > max(x0, x1) + R) continue;
      if (carY < min(y0, y1) - R || carY > max(y0, y1) + R) continue;
      drawSegment(i, ST77XX_WHITE);
    }
    if (abs(carX - trackPts[0]) < R + 4 && abs(carY - trackPts[1]) < R + 4) drawStartMark();
  }
  carDist += trackLen / 200.0;
  if (carDist >= trackLen) carDist -= trackLen;
  pointAt(carDist, carX, carY);
  tft.fillCircle(carX, carY, 3, ST77XX_YELLOW);
}

// =====================================================================
//  Режим 4: личный зачёт
// =====================================================================
int standPages() { return max(1, (driverCount + PER_PAGE - 1) / PER_PAGE); }

void drawStandings() {
  tft.fillScreen(ST77XX_BLACK);
  drawHeader("DRIVERS", season);

  tft.setTextSize(1);
  tft.setTextColor(GREY);
  tft.setCursor(8, 40);
  if (standingsRound) {
    tft.print("after Round ");
    tft.print(standingsRound);
    for (int i = 0; i < raceCount; i++)
      if (races[i].round == standingsRound) { tft.print(" ("); tft.print(races[i].name); tft.print(")"); }
  } else {
    tft.print("season has not started");
  }
  tft.setCursor(276, 40);
  tft.print("POINTS");
  tft.drawFastHLine(0, 52, 320, GREY);

  int first = standPage * PER_PAGE;
  for (int r = 0; r < PER_PAGE; r++) {
    int i = first + r;
    if (i >= driverCount) break;
    Driver& d = drivers[i];
    int y = 60 + r * 27;
    if (r % 2 == 0) tft.fillRect(0, y - 3, 320, 25, DARK);
    tft.fillRect(0, y - 3, 5, 25, d.color);

    tft.setTextSize(2);
    tft.setTextColor(i < 3 ? ST77XX_YELLOW : ST77XX_WHITE);
    tft.setCursor(i + 1 < 10 ? 20 : 10, y);
    tft.print(i + 1);

    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(44, y);
    tft.print(d.name);

    tft.setTextSize(1);
    tft.setTextColor(d.color);
    tft.setCursor(176, y + 4);
    tft.print(d.team);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", d.points);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(312 - strlen(buf) * 12, y);
    tft.print(buf);
  }
  drawFooterHint("tap: next page");
  drawDots(MODE_COUNT, mode);
}

// =====================================================================
//  Управление
// =====================================================================
void redraw() {
  cdKey = -1;
  switch (mode) {
    case COUNTDOWN: tickCountdown();  break;
    case SCHEDULE:  drawSchedule();   break;
    case TRACK:     drawTrack();      break;
    case STANDINGS: drawStandings();  break;
  }
}

// Одно нажатие: листаем внутри режима
void onClick() {
  switch (mode) {
    case COUNTDOWN:
      return;                                   // на экране отсчёта листать нечего
    case SCHEDULE:
    case TRACK: {
      int cur = max(0, currentRaceIndex());
      viewRace++;
      if (viewRace >= raceCount) viewRace = cur;
      break;
    }
    case STANDINGS:
      standPage = (standPage + 1) % standPages();
      break;
  }
  redraw();
}

// Двойное нажатие: следующий режим
void onDoubleClick() {
  mode = (mode + 1) % MODE_COUNT;
  standPage = 0;
  viewRace = max(0, currentRaceIndex());
  redraw();
}

// Удержание: обновить данные прямо сейчас
void onHold() {
  tft.fillScreen(ST77XX_BLACK);
  statusLine("Updating...");
  refreshData();
  trackFor[0] = 0;
  redraw();
}

void resetWiFi() {
  tft.fillScreen(ST77XX_BLACK);
  statusLine("WiFi reset...");
  WiFiManager wm;
  wm.resetSettings();
  delay(1000);
  ESP.restart();
}

void pollButton(Button& b) {
  bool down = digitalRead(b.pin) == LOW;
  unsigned long now = millis();

  if (down && !b.pressed && now - b.lastRelease > DEBOUNCE_MS) {
    b.pressed = true;
    b.pressStart = now;
  }
  if (down && b.pressed && b.canResetWiFi && now - b.pressStart > RESET_WIFI_MS) resetWiFi();

  if (!down && b.pressed && now - b.pressStart > DEBOUNCE_MS) {
    b.pressed = false;
    b.lastRelease = now;
    unsigned long held = now - b.pressStart;
    if (held >= HOLD_MS) { b.clicks = 0; onHold(); }
    else                 b.clicks++;
  }

  // второе нажатие не пришло вовремя — решаем, что это было
  if (!b.pressed && b.clicks > 0 && now - b.lastRelease > DOUBLE_GAP_MS) {
    if (b.clicks == 1) onClick();
    else               onDoubleClick();
    b.clicks = 0;
  }
}

// Экран с инструкцией, когда табло ждёт настройки Wi-Fi
void onConfigPortal(WiFiManager* wm) {
  tft.fillScreen(ST77XX_BLACK);
  drawHeader("F1 TIMER", "SETUP");
  centerText("WiFi setup", 44, 3, ST77XX_WHITE, ST77XX_BLACK);
  centerText("1. Connect your phone", 90, 2, GREY, ST77XX_BLACK);
  centerText("to WiFi network:", 112, 2, GREY, ST77XX_BLACK);
  centerText(AP_NAME, 136, 3, ST77XX_YELLOW, ST77XX_BLACK);
  centerText("2. Choose your network", 172, 2, GREY, ST77XX_BLACK);
  centerText("(or open 192.168.4.1)", 196, 2, GREY, ST77XX_BLACK);
}

// =====================================================================
void setup() {
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);            // синий светодиод на плате выключен
  Serial.begin(115200);
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("\n=== СТАРТ, причина: %s ===\n",
                rr == ESP_RST_POWERON  ? "включение питания" :
                rr == ESP_RST_BROWNOUT ? "ПРОСАДКА ПИТАНИЯ (brownout)" :
                rr == ESP_RST_PANIC    ? "ОШИБКА ПРОГРАММЫ (panic)" :
                rr == ESP_RST_SW       ? "программный перезапуск" :
                rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT
                                       ? "зависание (watchdog)" :
                rr == ESP_RST_EXT      ? "кнопка EN / Serial Monitor" : "другая");
  pinMode(BTN, INPUT_PULLUP);
  pinMode(BTN_EXT, INPUT_PULLUP);

  tft.init(240, 320, SPI_MODE3);   // если у вас работало с SPI_MODE0, оставьте его
  tft.setSPISpeed(10000000);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  drawHeader("F1 TIMER", "LIVE");
  centerText("F1 TIMER", 80, 4, ST77XX_WHITE, ST77XX_BLACK);

  // Wi-Fi: подключаемся к сохранённой сети, иначе — точка доступа для настройки
  statusLine("Connecting WiFi...");
  WiFiManager wm;
  wm.setAPCallback(onConfigPortal);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);      // 5 минут ждём настройки, потом перезагрузка
  if (!wm.autoConnect(AP_NAME)) {
    statusLine("No WiFi, restarting");
    delay(2000);
    ESP.restart();
  }
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // меньше пиковый ток -> меньше просадок питания
  WiFi.setSleep(false);                 // без режима сна радио: ток ровный, без импульсов
                                        // несколько раз в секунду (они и дают мерцание)
  tft.fillScreen(ST77XX_BLACK);
  drawHeader("F1 TIMER", "LIVE");
  centerText("F1 TIMER", 80, 4, ST77XX_WHITE, ST77XX_BLACK);

  statusLine("Syncing time...");
  configTzTime("MSK-3", "pool.ntp.org", "time.google.com");   // пояс уточнится по IP
  for (int i = 0; i < 40 && !timeValid(); i++) delay(250);

  while (true) {
    statusLine("Loading F1 data...");
    if (refreshData() && timeValid()) break;
    statusLine("Error, retry in 30s");
    delay(30000);
  }

  viewRace = max(0, currentRaceIndex());
  redraw();
}

void loop() {
  pollButton(btnBoot);
  pollButton(btnExt);

  if (millis() - lastFetch > refreshInterval()) {
    uint32_t before = dataSignature();
    refreshData();
    // Перерисовываем экран, только если данные действительно изменились —
    // иначе экран "мигал" бы при каждом фоновом обновлении
    if (dataSignature() != before) {
      Serial.println("Данные изменились, обновляю экран");
      trackFor[0] = 0;        // схему перезагрузим при следующем просмотре
      if (mode == COUNTDOWN) cdKey = -1;
      else if (mode != TRACK) redraw();   // анимацию трассы не прерываем
    }
  }

  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    if (mode == COUNTDOWN) tickCountdown();
  }

  if (mode == TRACK) animateCar();
}
