// ═══════════════════════════════════════════════════════════════════════════
//  ticker.ino  –  ESP32-3248S035C  |  480×320 TFT (portrait: 320 wide × 480 tall)
//  DIY Desk Stock/Crypto Ticker
//  Libraries: TFT_eSPI, WiFiManager, ArduinoJson v6, Preferences
// ═══════════════════════════════════════════════════════════════════════════

#include <FS.h>
#include <SPIFFS.h>
using namespace fs;
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <ESPmDNS.h>

// ─── GT911 capacitive touch pins (ESP32-3248S035C) ───────────────────────────
#define GT911_SDA   33
#define GT911_SCL   32
#define GT911_INT   21
#define GT911_RST   25

// GT911 reports this panel in portrait coordinates (320x480). TFT_eSPI
// rotation(1) is landscape, so swap X/Y and invert Y by default.
#define GT911_TOUCH_SWAP_XY   true
#define GT911_TOUCH_INVERT_X  false
#define GT911_TOUCH_INVERT_Y  true

// ─── Screen dimensions (landscape) ───────────────────────────────────────────
#define SCREEN_W    480
#define SCREEN_H    320

// ─── Colour palette ──────────────────────────────────────────────────────────
#define C_BG        TFT_BLACK
#define C_GREEN     0x07E0
#define C_RED       0xF800
#define C_YELLOW    0xFFE0
#define C_WHITE     0xFFFF
#define C_GRAY      0x7BEF
#define C_DARKGRAY  0x2965
#define C_PURPLE    0x780F
#define C_DKBLUE    0x1082

// ─── UI layout & style ───────────────────────────────────────────────────────
// ONE place to tune every visual detail of the chart screen.
// Each #define is used directly in drawChart() / initStars() / tickStars().
// See "Colour palette" above for C_BG, C_GREEN, C_RED, C_WHITE, C_GRAY, etc.

// Panel border & background
#define UI_INSET        5       // gap from screen edge to glow border (px, X and Y)
#define UI_CORNER_R     10      // rounded-corner radius (px; 0 = sharp corners)


// Candle chart
#define UI_CANDLE_BODY  0.6f    // body width as fraction of each slot (0.0–1.0; bigger = fatter)
#define UI_PRICE_PAD    0.05f   // vertical margin above/below price range (fraction of range)
#define UI_LABEL_W      52      // width of the price-label column right of the chart (px)

// Portfolio line chart
#define UI_PORT_PAD     0.08f   // vertical margin above/below portfolio value range

// Bottom info bar
#define UI_BAR_TY       7       // text row (L/H/timeframe): offset from top of bottom bar (px)
#define UI_BAR_DOTY     30      // nav-dots row: offset from top of bottom bar (px)
#define UI_DOT_STEP     20      // horizontal spacing between navigation dots (px)

// Starfield (shown during pre/after-hours and market closed)
#define UI_NUM_STARS    60      // total simultaneous stars on screen
#define UI_STAR_SPEED   6       // brightness change per 80 ms tick (higher = faster twinkle)
#define UI_STAR_BIG     4       // 1-in-N chance a star is 2×2 px (higher N = fewer big stars)

// Shooting stars
#define UI_NUM_SHOOT    3       // max simultaneous shooting stars
#define UI_SHOOT_ODDS   120     // 1-in-N chance per slot per tick to spawn (~4 s avg with 3 slots)
#define UI_SHOOT_SEGS   5       // trail length in segments (each ~25 px — raise for longer tail)

// ─── Clock timezone ───────────────────────────────────────────────────────────
// UTC_OFFSET = your local timezone, used ONLY for the clock display on screen.
// At runtime the WiFiManager portal lets you change it without reflashing.
//   Eastern:  -5 (EST, Nov–Mar)   -4 (EDT, Mar–Nov)
//   Central:  -6 (CST)            -5 (CDT)
//   Mountain: -7 (MST)            -6 (MDT)
//   Pacific:  -8 (PST)            -7 (PDT)  ← current
#define UTC_OFFSET  -7   // ← your local offset (PDT); override in the setup portal

// US Eastern offset used ONLY for market session detection.
// computeETOffset() returns -4 (EDT, Mar–Nov) or -5 (EST, Nov–Mar) automatically.
int g_utcOffset = UTC_OFFSET; // actual runtime value (loaded from flash on boot)
enum MarketSession { SESSION_OPEN = 0, SESSION_PRE, SESSION_AFTER, SESSION_CLOSED };

// Per-session theme: 4 glow ring colours (faint→bright) + status badge text
struct SessionTheme {
  uint16_t g0, g1, g2, g3;
  const char* badge;
};
static const SessionTheme THEMES[] = {
  { 0x0000, 0x0000, 0x0000, 0x0000, ""       }, // OPEN    – orange
  { 0x2200, 0x4400, 0x6800, 0xFFE0, "PRE"    }, // PRE     – yellow
  { 0x1002, 0x2005, 0x400A, 0x780F, "AH"     }, // AFTER   – purple
  { 0x0841, 0x1082, 0x18C3, 0x2965, "CLOSED" }, // CLOSED  – cool blue
};

// ─── Color themes (user-selectable via web portal) ───────────────────────────
struct ColorTheme {
  const char* name;
  const char* webUp;    // CSS hex for portal preview
  const char* webDown;
  uint16_t    cUp;      // RGB565 – rising candles / up price
  uint16_t    cDown;    // RGB565 – falling candles / down price
  uint16_t    cPanel;   // RGB565 – chart panel background
};
static const ColorTheme COLOR_THEMES[] = {
  { "Classic",     "#00ff00", "#ff0000", 0x07E0, 0xF800, 0x0862 },
  { "Cyan / Orange","#00e5ff", "#ff8000", 0x07FF, 0xFD00, 0x0862 },
  { "Amber",       "#ffff00", "#ff4400", 0xFFE0, 0xF880, 0x18C3 },
  { "Monochrome",  "#ffffff", "#424242", 0xFFFF, 0x4208, 0x0000 },
  { "Neon",        "#00ff00", "#ff00ff", 0x07E0, 0xF81F, 0x000C },
};
static const int NUM_COLOR_THEMES = 5;
int      g_themeIdx = 0;
uint16_t g_cUp      = COLOR_THEMES[0].cUp;
uint16_t g_cDown    = COLOR_THEMES[0].cDown;
uint16_t g_cPanel   = COLOR_THEMES[0].cPanel;

// ─── Starfield ────────────────────────────────────────────────────────────────
struct Star { int16_t x, y; uint8_t bright; int8_t dir; uint8_t sz; };
static Star    stars[UI_NUM_STARS];
static bool    starsReady      = false;
static bool    starsFullScreen = false;
static int16_t starBX, starBY, starBW, starBH;

struct ShootStar { float x, y, dx, dy; bool active; };
static ShootStar shoots[UI_NUM_SHOOT];

// ─── Watchlist ───────────────────────────────────────────────────────────────
#define MAX_WATCHLIST 12
static char g_watchlist[MAX_WATCHLIST][12];
static int  g_watchlistLen = 0;

// ─── UI customisation ────────────────────────────────────────────────────────
#define TIMEFRAME_LBL "1D 5m"      // timeframe label shown in bottom bar


// ─── Candle data ─────────────────────────────────────────────────────────────
#define MAX_CANDLES 56
struct Candle { float open, high, low, close; long ts; };
Candle candles[MAX_CANDLES];
int    candleCount = 0;

// ─── Per-symbol cache ─────────────────────────────────────────────────────────
// Stores the last successful fetch for each watchlist slot (~1.1 KB each, ~7 KB total).
// Swipes load the cache instantly; a background re-fetch fires only when stale.
#define CACHE_TTL_MS  300000UL  // 5 minutes — change to refresh more/less often

struct SymbolCache {
  Candle        candles[MAX_CANDLES];
  int           count;
  float         price, open, high, low, pctChange;
  unsigned long fetchedAt; // millis() of last fill; 0 = never fetched
};
static SymbolCache symCache[MAX_WATCHLIST];

// ─── Portfolio chart data ─────────────────────────────────────────────────────
// Combined $ value of all positions at each intraday 5-min slot.
float portfolioValues[MAX_CANDLES];
long  portTimes[MAX_CANDLES];
int           portTimeLen   = 0;
float         portValueOpen = 0;
unsigned long portFetchedAt = 0; // millis() when portfolio was last built (0 = never)

// ─── Quote data ──────────────────────────────────────────────────────────────
float quotePrice     = 0;
float quoteOpen      = 0;
float quoteHigh      = 0;
float quoteLow       = 0;
float quotePrevClose = 0;
float quotePctChange = 0;
int   currentSymIdx  = 0;
char  fetchStatus[24] = "";

// ─── Portfolio ───────────────────────────────────────────────────────────────
#define MAX_POSITIONS 10
struct Position {
  char  symbol[12];
  float shares;
  float avgCost;
  float currentPrice;   // -1 = not yet loaded
};
Position portfolio[MAX_POSITIONS];
int      positionCount = 0;

// ─── Timing / debounce ───────────────────────────────────────────────────────
unsigned long lastFetchMs      = 0;
unsigned long lastTouchMs      = 0;
unsigned long lastAdvanceMs    = 0;
unsigned long lastAnimMs       = 0;
bool needsFullRedraw = true; // set true whenever a non-chart screen overwrites the panel
const unsigned long FETCH_INTERVAL    = 30000UL;
unsigned long g_advanceMs = 15000UL; // auto-cycle interval (loaded from flash)
const unsigned long TOUCH_DEBOUNCE    = 180UL;   // ms — lower = snappier swipes
// HTTP timeouts now set directly in fetchCandles() via HTTPClient

// ─── Async fetch (runs on core 0, UI stays on core 1) ───────────────────────
// xSemaphoreGive(g_fetchSem) to request a fetch from any task context.
// Binary semaphore: multiple gives while fetching collapse to one extra wake.
// When complete, g_fetchDone is set true → loop() redraws.
static SemaphoreHandle_t    g_fetchSem  = nullptr;
static volatile bool        g_fetchDone = false;

// ─── Touch polling ────────────────────────────────────────────────────────────
// GT911 is polled every 16 ms in loop(). No ISR needed — the INT line on this
// board does not reliably generate a FALLING edge after the reset sequence.

// ─── Objects ─────────────────────────────────────────────────────────────────
TFT_eSPI        tft;
WebServer       server(80);
Preferences     prefs;
uint8_t         gt911Addr = 0;

// ─── Forward declarations ─────────────────────────────────────────────────────
void drawChart();
void fetchData();
void buildPortfolioChart();
bool fetchCandles(const char* sym, const char* interval = "5m", const char* range = "1d");
void loadCandleCache();
void saveCandleCache(int idx);
String yahooSym(const char* sym);
bool   isCrypto(const char* sym);
void loadPortfolio();
void savePortfolio();
String buildPortfolioHTML();
String handlePortfolioSave(const String& body);
void setupWebServer();
void handleTouch();
void switchSymbol(int newIdx);
void initGT911Touch();
bool readGT911Touch(int& tx, int& ty);
bool gt911Read(uint16_t reg, uint8_t* data, uint8_t len);
bool gt911WriteByte(uint16_t reg, uint8_t value);
bool gt911Probe(uint8_t addr);
void showFetchDot(bool on);
void fmtPrice(float p, char* out, int n);
MarketSession getMarketSession();
void initStars(int16_t x, int16_t y, int16_t w, int16_t h);
void tickStars();
void drawClock(int cy, int ch, uint16_t bg, MarketSession sess);

// ════════════════════════════════════════════════════════════════════════════
//  ASYNC FETCH TASK  (pinned to core 0 — TFT/touch stay on core 1)
// ════════════════════════════════════════════════════════════════════════════
static void fetchTaskFn(void*) {
  for (;;) {
    xSemaphoreTake(g_fetchSem, portMAX_DELAY); // sleeps until a fetch is requested
    fetchData();
    g_fetchDone = true;
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════════════
void setup() {
  // Backlight enable — MUST be first
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

  // RGB LED (common anode) — HIGH = off.
  // Cover all known pin variants for the ESP32-3248S035C.
  for (int p : {4, 16, 17, 2}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }

  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n=== TICKER BOOT ===");

  // Display init
  tft.init();
  tft.setRotation(1);       // landscape: 480 × 320
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(2);
  tft.drawCentreString("TICKER", SCREEN_W / 2, 136, 2);
  tft.setTextSize(1);
  tft.drawCentreString("Starting up...", SCREEN_W / 2, 168, 1);

  initGT911Touch();
  // INT pin left as INPUT; touch is polled in loop() — no ISR needed.

  // Load saved config (tz, color theme, auto-advance interval)
  {
    Preferences cfg; cfg.begin("cfg", true);
    g_utcOffset = cfg.getInt("tz",  UTC_OFFSET);
    g_themeIdx  = cfg.getInt("th",  0);
    if (g_themeIdx < 0 || g_themeIdx >= NUM_COLOR_THEMES) g_themeIdx = 0;
    g_cUp    = COLOR_THEMES[g_themeIdx].cUp;
    g_cDown  = COLOR_THEMES[g_themeIdx].cDown;
    g_cPanel = COLOR_THEMES[g_themeIdx].cPanel;
    int advSec = cfg.getInt("adv", 15);
    if (advSec < 5) advSec = 5; if (advSec > 120) advSec = 120;
    g_advanceMs = (unsigned long)advSec * 1000UL;
    cfg.end();
  }

  SPIFFS.begin(true); // mount filesystem (true = format if corrupt)
  loadPortfolio();
  loadWatchlist();
  loadCandleCache(); // pre-fill symbol cache from flash for instant first draw

  // ── WiFiManager captive portal ───────────────────────────────────────────
  tft.fillScreen(C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawCentreString("Connect to WiFi:", SCREEN_W / 2, 170, 1);
  tft.setTextColor(0xFD00, C_BG);  // orange
  tft.drawCentreString("TICKER-SETUP", SCREEN_W / 2, 192, 2);
  tft.setTextColor(C_GRAY, C_BG);
  tft.drawCentreString("then open browser → set timezone + portfolio", SCREEN_W / 2, 222, 1);

  WiFiManager wm;
  wm.setTitle("TICKER");
  wm.setDarkMode(true);
  // Dark orange theme matching the display UI
  wm.setCustomHeadElement(
    "<style>"
    "body,div.wrap{background:#0b0b0b!important}"
    "h1,h2,h3{color:#fd7700!important}"
    "a{color:#fd9933}"
    "input:not([type=submit]){background:#161616!important;border:1px solid #444!important;"
                              "color:#eee!important;border-radius:6px!important}"
    "input[type=submit],button{background:#fd7700!important;color:#000!important;"
                               "font-weight:bold!important;border-radius:6px!important}"
    ".msg{background:#161616!important;border:1px solid #fd7700!important;border-radius:8px!important}"
    "li a{color:#ccc!important}"
    "</style>"
  );
  // Extra menu link to portfolio editor
  wm.setCustomMenuHTML(
    "<li><a href='/portfolio'>&#128200; Portfolio &amp; Watchlist</a></li>"
  );

  // UTC offset field — pre-filled from flash
  char tzBuf[6]; snprintf(tzBuf, sizeof(tzBuf), "%d", g_utcOffset);
  WiFiManagerParameter p_tz("tz", "UTC offset  (ET=-4, CT=-5, MT=-6, PT=-7)", tzBuf, 5);
  wm.addParameter(&p_tz);

  // Inject portfolio editor into WiFiManager's internal web server
  wm.setWebServerCallback([&wm]() {
    wm.server->on("/portfolio", HTTP_GET, [&wm]() {
      wm.server->send(200, "text/html", buildPortfolioHTML());
    });
    wm.server->on("/save", HTTP_POST, [&wm]() {
      String body = wm.server->arg("plain");
      if (body.length() == 0 && wm.server->client().available())
        body = wm.server->client().readString();
      String err = handlePortfolioSave(body);
      if (err.length()) { wm.server->send(400, "text/plain", err); return; }
      wm.server->send(200, "text/plain", "Saved " + String(positionCount) + " position(s) — return to configure WiFi.");
    });
  });

  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("TICKER-SETUP")) {
    tft.setTextColor(C_RED, C_BG);
    tft.drawCentreString("WiFi failed — restarting", SCREEN_W / 2, 260, 1);
    delay(3000);
    ESP.restart();
  }

  // Kill RGB LED again — WiFi stack can pull GPIO2 (green) LOW during connect
  for (int p : {4, 16, 17, 2}) digitalWrite(p, HIGH);

  // Persist UTC offset chosen in portal
  g_utcOffset = atoi(p_tz.getValue());
  { Preferences cfg; cfg.begin("cfg", false); cfg.putInt("tz", g_utcOffset); cfg.end(); }
  Serial.printf("UTC offset set to %d\n", g_utcOffset);

  // NTP time sync (needed for candle from/to timestamps)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // Wait up to 5 s for NTP
  time_t now = 0;
  for (int i = 0; i < 10 && now < 100000; i++) { delay(500); time(&now); }

  // Web server
  setupWebServer();
  server.begin();
  MDNS.begin("ticker"); // → http://ticker.local

  // Show IP on screen for 4 seconds so you can always find the web UI
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawCentreString("ticker.local  |  " + WiFi.localIP().toString(), SCREEN_W / 2, 150, 2);
  tft.setTextColor(C_GRAY, C_BG);
  tft.drawCentreString("open in browser to manage portfolio", SCREEN_W / 2, 180, 1);
  delay(4000);

  // Draw immediately — async task handles the first fetch in the background
  drawChart();

  // Start async fetch task on core 0
  g_fetchSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(fetchTaskFn, "fetch", 8192, NULL, 1, NULL, 0);
  xSemaphoreGive(g_fetchSem); // kick first fetch immediately
}

// ════════════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();

  // Touch: poll GT911 status register at ~60 Hz (one I2C read per 16 ms)
  {
    static unsigned long lastTouchPollMs = 0;
    unsigned long now = millis();
    if (now - lastTouchPollMs >= 16) {
      lastTouchPollMs = now;
      handleTouch();
    }
  }

  // Periodic data refresh — skip when market is closed (no new data)
  if (millis() - lastFetchMs >= FETCH_INTERVAL) {
    lastFetchMs = millis();
    if (getMarketSession() == SESSION_OPEN) xSemaphoreGive(g_fetchSem);
  }

  // Auto-advance — skip when market is closed (all views show the same star screen)
  if (millis() - lastAdvanceMs >= g_advanceMs) {
    lastAdvanceMs = millis();
    if (getMarketSession() == SESSION_OPEN)
      switchSymbol((currentSymIdx + 1) % (g_watchlistLen + 1));
  }

  // Fetch complete on core 0 → redraw on core 1
  if (g_fetchDone) {
    g_fetchDone = false;
    drawChart();
  }

  // Starfield animation tick — runs every 80 ms during non-market hours
  if (millis() - lastAnimMs >= 80) {
    lastAnimMs = millis();
    if (getMarketSession() != SESSION_OPEN) tickStars();
  }

  // Clock text update — checks every 10 s; drawClock only redraws on minute change
  static unsigned long lastClockMs = 0;
  if (millis() - lastClockMs >= 10000) {
    lastClockMs = millis();
    MarketSession s = getMarketSession();
    if (s != SESSION_OPEN && starsReady)
      drawClock(0, SCREEN_H, g_cPanel, s);
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  SYMBOL SWITCHING  (used by touch + auto-advance)
// ════════════════════════════════════════════════════════════════════════════
// Changes the active slot, loads cached data for instant display, then
// triggers a background fetch only if the cache is missing or stale.
void switchSymbol(int newIdx) {
  currentSymIdx = newIdx;

  if (currentSymIdx < g_watchlistLen) {
    SymbolCache& c = symCache[currentSymIdx];
    if (c.count > 0) {
      // Restore cached candles + quote so drawChart() draws immediately
      candleCount    = c.count;
      memcpy(candles, c.candles, candleCount * sizeof(Candle));
      quotePrice     = c.price;
      quoteOpen      = c.open;
      quoteHigh      = c.high;
      quoteLow       = c.low;
      quotePctChange = c.pctChange;
    } else {
      candleCount = 0; // never fetched — chart shows "Loading…"
    }
    // Fetch in background only when cache is absent or older than CACHE_TTL_MS
    if (c.fetchedAt == 0 || millis() - c.fetchedAt >= CACHE_TTL_MS)
      xSemaphoreGive(g_fetchSem);
  } else {
    // Portfolio slot — use cached data if fresh, otherwise re-fetch
    if (portFetchedAt == 0 || millis() - portFetchedAt >= CACHE_TTL_MS) {
      portTimeLen = 0;
      xSemaphoreGive(g_fetchSem);
    }
    // else: portTimeLen + portfolioValues[] still valid — drawChart() uses them immediately
  }

  drawChart();
}

// ════════════════════════════════════════════════════════════════════════════
//  TOUCH HANDLING
// ════════════════════════════════════════════════════════════════════════════
void handleTouch() {
  int tx, ty;
  if (!readGT911Touch(tx, ty)) return;
  if (g_watchlistLen == 0) return; // Ignore touch if no watchlist
  unsigned long now = millis();
  if (now - lastTouchMs < TOUCH_DEBOUNCE) return;
  lastTouchMs = now;

  if (tx < SCREEN_W / 3) {
    switchSymbol((currentSymIdx - 1 + g_watchlistLen + 1) % (g_watchlistLen + 1));
  } else if (tx >= (SCREEN_W * 2) / 3) {
    switchSymbol((currentSymIdx + 1) % (g_watchlistLen + 1));
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  SYMBOL HELPERS
// ════════════════════════════════════════════════════════════════════════════
void initGT911Touch() {
  Wire.begin(GT911_SDA, GT911_SCL);
  Wire.setClock(400000);

  pinMode(GT911_INT, OUTPUT);
  digitalWrite(GT911_INT, LOW); // Select 0x5D on GT911 reset.
  pinMode(GT911_RST, OUTPUT);
  digitalWrite(GT911_RST, LOW);
  delay(10);
  digitalWrite(GT911_RST, HIGH);
  delay(60);
  pinMode(GT911_INT, INPUT_PULLUP); // keep INT line from floating between polls
  delay(50);

  if (gt911Probe(0x5D)) {
    gt911Addr = 0x5D;
  } else if (gt911Probe(0x14)) {
    gt911Addr = 0x14;
  } else {
    gt911Addr = 0;
  }
}

bool readGT911Touch(int& tx, int& ty) {
  if (!gt911Addr) return false;

  uint8_t status = 0;
  if (!gt911Read(0x814E, &status, 1)) return false;
  if ((status & 0x80) == 0) return false;

  uint8_t points = status & 0x0F;
  if (points == 0 || points > 5) {
    gt911WriteByte(0x814E, 0);
    return false;
  }

  uint8_t data[8];
  bool ok = gt911Read(0x8150, data, sizeof(data));
  gt911WriteByte(0x814E, 0); // Tell GT911 the touch report was consumed.
  if (!ok) return false;

  int rawX = data[1] | (data[2] << 8);
  int rawY = data[3] | (data[4] << 8);
  rawX = constrain(rawX, 0, 319);
  rawY = constrain(rawY, 0, 479);

  int sx = GT911_TOUCH_SWAP_XY ? rawY : rawX;
  int sy = GT911_TOUCH_SWAP_XY ? rawX : rawY;
  if (GT911_TOUCH_INVERT_X) sx = SCREEN_W - 1 - sx;
  if (GT911_TOUCH_INVERT_Y) sy = SCREEN_H - 1 - sy;

  tx = constrain(sx, 0, SCREEN_W - 1);
  ty = constrain(sy, 0, SCREEN_H - 1);
  return true;
}

bool gt911Read(uint16_t reg, uint8_t* data, uint8_t len) {
  Wire.beginTransmission(gt911Addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;

  uint8_t got = Wire.requestFrom((int)gt911Addr, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) data[i] = Wire.read();
  return true;
}

bool gt911WriteByte(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(gt911Addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool gt911Probe(uint8_t addr) {
  gt911Addr = addr;
  uint8_t id[4] = { 0 };
  if (!gt911Read(0x8140, id, sizeof(id))) return false;
  return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

bool isCrypto(const char* sym) {
  return strcmp(sym, "BTC") == 0 || strcmp(sym, "ETH") == 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  DATA FETCHING
// ════════════════════════════════════════════════════════════════════════════
void showFetchDot(bool on) {
  (void)on; // fetch indicator removed — dot left ghost on panel background
}

// Fetches 1-week hourly candles for every portfolio position and builds a
// combined $ value array using a single batch HTTP request to Yahoo's spark API.
void buildPortfolioChart() {
  portTimeLen   = 0;
  portValueOpen = 0;
  for (int t = 0; t < MAX_CANDLES; t++) portfolioValues[t] = 0;
  if (positionCount == 0) return;

  // Build comma-separated symbol list
  String symbols;
  for (int i = 0; i < positionCount; i++) {
    if (i > 0) symbols += ',';
    symbols += portfolio[i].symbol;
  }

  // Single HTTP request for all symbols at once
  WiFiClientSecure secClient;
  secClient.setInsecure();
  HTTPClient http;
  String url = "https://query1.finance.yahoo.com/v7/finance/spark?symbols="
               + symbols + "&range=1mo&interval=1d&includeTimestamps=true";
  if (!http.begin(secClient, url)) return;
  http.addHeader("User-Agent",      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
  http.addHeader("Accept",          "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.setTimeout(10000);

  int code = http.GET();
  Serial.printf("Portfolio spark HTTP %d  symbols=%s\n", code, symbols.c_str());
  if (code != 200) { http.end(); return; }
  String body = http.getString();
  http.end();
  Serial.printf("Portfolio spark body %d bytes\n", body.length());
  if (body.length() < 10) return;

  StaticJsonDocument<256> filter;
  filter["spark"]["result"][0]["symbol"]                                        = true;
  filter["spark"]["result"][0]["response"][0]["timestamp"]                      = true;
  filter["spark"]["result"][0]["response"][0]["indicators"]["quote"][0]["close"]= true;

  static StaticJsonDocument<12288> doc;
  doc.clear();
  auto err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) { Serial.printf("Portfolio spark JSON err: %s\n", err.c_str()); return; }

  JsonArray results = doc["spark"]["result"].as<JsonArray>();
  if (results.isNull()) return;

  for (JsonObject result : results) {
    const char* sym = result["symbol"] | "";
    int posIdx = -1;
    for (int p = 0; p < positionCount; p++)
      if (strcasecmp(portfolio[p].symbol, sym) == 0) { posIdx = p; break; }
    if (posIdx < 0) continue;

    JsonArray resp = result["response"].as<JsonArray>();
    if (resp.isNull() || resp.size() == 0) continue;
    JsonObject r  = resp[0];
    JsonArray  ts = r["timestamp"].as<JsonArray>();
    JsonArray  cl = r["indicators"]["quote"][0]["close"].as<JsonArray>();
    if (ts.isNull() || cl.isNull() || ts.size() == 0) continue;

    int total    = (int)ts.size();
    int startIdx = max(0, total - MAX_CANDLES);
    int n        = total - startIdx;
    float shares = portfolio[posIdx].shares;

    // Latest non-null close = current price
    float lastClose = 0;
    for (int c = total - 1; c >= 0 && lastClose == 0; c--)
      lastClose = cl[c] | 0.0f;
    if (lastClose > 0) portfolio[posIdx].currentPrice = lastClose;

    if (portTimeLen == 0) {
      portTimeLen = n;
      for (int i = 0; i < n; i++) {
        portTimes[i]       = (long)(ts[startIdx + i] | 0);
        float c            = cl[startIdx + i] | 0.0f;
        portfolioValues[i] = shares * (c > 0 ? c : lastClose);
      }
    } else {
      // US stocks share the same trading-day calendar — align by index
      int alignLen = min(portTimeLen, n);
      for (int i = 0; i < alignLen; i++) {
        float c = cl[startIdx + i] | 0.0f;
        portfolioValues[i] += shares * (c > 0 ? c : lastClose);
      }
    }
    Serial.printf("Portfolio: %s %.4f sh, %d bars, last=%.2f\n",
                  sym, shares, n, lastClose);
  }

  if (portTimeLen > 0) { portValueOpen = portfolioValues[0]; portFetchedAt = millis(); }
  Serial.printf("Portfolio chart: %d pts  open=$%.2f  cur=$%.2f\n",
                portTimeLen, portValueOpen,
                portTimeLen > 0 ? portfolioValues[portTimeLen-1] : 0);
}

void fetchData() {
  lastFetchMs = millis();

  // Snapshot the index now — user may swipe while core 0 is mid-fetch,
  // and we must save the result to the slot we actually fetched, not wherever
  // currentSymIdx has drifted to by the time the HTTP response arrives.
  int fetchIdx = currentSymIdx;

  // Portfolio slot: build the combined chart from all positions
  if (fetchIdx == g_watchlistLen) {
    showFetchDot(true);
    buildPortfolioChart();
    showFetchDot(false);
    lastFetchMs = millis(); // reset timer — multi-symbol fetch can exceed FETCH_INTERVAL
    return;
  }

  showFetchDot(true);
  const char* sym = g_watchlist[fetchIdx];
  bool yahooOk = fetchCandles(sym);

  if (yahooOk) {
    strncpy(fetchStatus, "ok", sizeof(fetchStatus));
    fetchStatus[sizeof(fetchStatus) - 1] = '\0';

    // Save to the slot we fetched — use fetchIdx, not currentSymIdx
    SymbolCache& c = symCache[fetchIdx];
    c.count      = candleCount;
    memcpy(c.candles, candles, candleCount * sizeof(Candle));
    c.price      = quotePrice;
    c.open       = quoteOpen;
    c.high       = quoteHigh;
    c.low        = quoteLow;
    c.pctChange  = quotePctChange;
    c.fetchedAt  = millis();
    saveCandleCache(fetchIdx); // persist to flash for instant display on next boot
  }

  // Propagate live price to any matching portfolio position
  for (int i = 0; i < positionCount; i++) {
    if (strcasecmp(portfolio[i].symbol, sym) == 0) {
      portfolio[i].currentPrice = quotePrice;
    }
  }

  showFetchDot(false);
}

// Map ticker symbols to Yahoo Finance format
String yahooSym(const char* sym) {
  if (strcmp(sym, "BTC") == 0) return "BTC-USD";
  if (strcmp(sym, "ETH") == 0) return "ETH-USD";
  return String(sym);
}

bool fetchCandles(const char* sym, const char* interval, const char* range) {
  // HTTPClient handles chunked transfer encoding automatically.
  // Yahoo sometimes blocks query1; try query2 as fallback.
  WiFiClientSecure secClient;
  secClient.setInsecure();
  HTTPClient http;

  const char* hosts[] = { "query1.finance.yahoo.com", "query2.finance.yahoo.com" };
  String body;

  for (int attempt = 0; attempt < 2; attempt++) {
    String url = String("https://") + hosts[attempt] + "/v8/finance/chart/"
                 + yahooSym(sym) + "?interval=" + interval + "&range=" + range;

    if (!http.begin(secClient, url)) {
      Serial.printf("Yahoo begin failed (%s)\n", hosts[attempt]);
      continue;
    }

    http.addHeader("User-Agent",      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    http.addHeader("Accept",          "application/json");
    http.addHeader("Accept-Encoding", "identity");        // prevent gzip — ESP32 can't decode it
    http.addHeader("Accept-Language", "en-US,en;q=0.9");
    http.addHeader("Referer",         "https://finance.yahoo.com/");
    http.setTimeout(8000);

    int httpCode = http.GET();
    Serial.printf("Yahoo %s %s → HTTP %d\n", hosts[attempt], sym, httpCode);

    if (httpCode == 200) {
      body = http.getString();
      http.end();
      break;
    }
    snprintf(fetchStatus, sizeof(fetchStatus), "yhttp %d", httpCode);
    http.end();
  }

  Serial.printf("Yahoo body %d bytes for %s\n", body.length(), sym);

  if (body.length() < 10) {
    if (fetchStatus[0] == '\0') strncpy(fetchStatus, "empty body", sizeof(fetchStatus));
    Serial.println("Yahoo empty/no body");
    return false;
  }

  StaticJsonDocument<300> filter;
  filter["chart"]["result"][0]["timestamp"]                        = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["open"]   = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["high"]   = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["low"]    = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"]  = true;

  static StaticJsonDocument<20480> doc; // static = BSS, not heap — no alloc/free per fetch
  doc.clear();
  auto err = deserializeJson(doc, body, DeserializationOption::Filter(filter));

  if (err) {
    snprintf(fetchStatus, sizeof(fetchStatus), "yjson %s", err.c_str());
    Serial.printf("Yahoo JSON failed for %s: %s\n", sym, err.c_str());
    Serial.printf("Body preview: %.120s\n", body.c_str());
    return false;
  }

  JsonArray ts = doc["chart"]["result"][0]["timestamp"].as<JsonArray>();
  JsonObject q  = doc["chart"]["result"][0]["indicators"]["quote"][0];
  JsonArray ao  = q["open"],  ah = q["high"], al = q["low"], ac = q["close"];

  int total = (int)ts.size();
  if (total == 0) {
    strncpy(fetchStatus, "yempty", sizeof(fetchStatus));
    fetchStatus[sizeof(fetchStatus) - 1] = '\0';
    Serial.printf("Yahoo empty timestamps for %s\n", sym);
    return false;
  }

  candleCount = 0;
  quoteHigh   = 0.0f;
  quoteLow    = 0.0f;
  int startIdx = max(0, total - MAX_CANDLES);
  for (int src = startIdx; src < total && candleCount < MAX_CANDLES; src++) {
    float c = ac[src] | 0.0f;
    if (c <= 0.0f) continue;

    float o = ao[src] | c;
    float h = ah[src] | c;
    float l = al[src] | c;
    if (o <= 0.0f) o = c;
    if (h <= 0.0f) h = c;
    if (l <= 0.0f) l = c;

    candles[candleCount].close = c;
    candles[candleCount].open  = o;
    candles[candleCount].high  = h;
    candles[candleCount].low   = l;
    candles[candleCount].ts    = ts[src] | 0L;

    if (candleCount == 0) {
      quoteOpen = o;
      quoteHigh = h;
      quoteLow  = l;
    } else {
      if (h > quoteHigh) quoteHigh = h;
      if (l < quoteLow)  quoteLow  = l;
    }
    quotePrice = c;
    candleCount++;
  }

  if (candleCount == 0) {
    strncpy(fetchStatus, "ynull", sizeof(fetchStatus));
    fetchStatus[sizeof(fetchStatus) - 1] = '\0';
    Serial.printf("Yahoo all-null candles for %s\n", sym);
    return false;
  }

  quotePrevClose = quoteOpen;
  quotePctChange = (quoteOpen > 0.0f)
                   ? (quotePrice - quoteOpen) / quoteOpen * 100.0f
                   : 0.0f;

  Serial.printf("Yahoo OK %s  price=%.2f  chg=%+.2f%%  candles=%d\n",
                sym, quotePrice, quotePctChange, candleCount);
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  PRICE FORMAT HELPER
// ════════════════════════════════════════════════════════════════════════════
void fmtPrice(float p, char* out, int n) {
  if      (p >= 10000) snprintf(out, n, "%.0f",  p);
  else if (p >= 100)   snprintf(out, n, "%.2f",  p);
  else if (p >= 1)     snprintf(out, n, "%.3f",  p);
  else                 snprintf(out, n, "%.5f",  p);
}

// ════════════════════════════════════════════════════════════════════════════
//  MARKET SESSION
// ════════════════════════════════════════════════════════════════════════════

// Returns -4 (EDT, Mar–Nov) or -5 (EST, Nov–Mar) based on US DST rules.
// DST starts: 2nd Sunday of March at 02:00 ET (07:00 UTC).
// DST ends:   1st Sunday of November at 02:00 ET (06:00 UTC).
static int computeETOffset() {
  time_t now; time(&now);
  struct tm t; gmtime_r(&now, &t);
  int mo = t.tm_mon + 1; // 1–12
  if (mo < 3 || mo > 11) return -5;
  if (mo > 3 && mo < 11) return -4;
  if (mo == 3) {
    struct tm m = {}; m.tm_year = t.tm_year; m.tm_mon = 2; m.tm_mday = 1;
    mktime(&m); // fills tm_wday for March 1
    int dstDay = 8 + (7 - m.tm_wday) % 7; // 2nd Sunday (Mar 8–14)
    if (t.tm_mday < dstDay) return -5;
    if (t.tm_mday > dstDay) return -4;
    return (t.tm_hour >= 7) ? -4 : -5; // switch at 07:00 UTC
  }
  // mo == 11
  struct tm m = {}; m.tm_year = t.tm_year; m.tm_mon = 10; m.tm_mday = 1;
  mktime(&m);
  int stdDay = 1 + (7 - m.tm_wday) % 7; // 1st Sunday (Nov 1–7)
  if (t.tm_mday < stdDay) return -4;
  if (t.tm_mday > stdDay) return -5;
  return (t.tm_hour >= 6) ? -5 : -4; // switch at 06:00 UTC
}

MarketSession getMarketSession() {
  return SESSION_OPEN; 
  time_t now; time(&now);
  struct tm t; gmtime_r(&now, &t);
  // Session boundaries are always in ET — market hours never move.
  // g_utcOffset is only used for the clock display, not here.
  int etMin = ((t.tm_hour + computeETOffset()) * 60 + t.tm_min + 1440) % 1440;
  int dow   = t.tm_wday; // 0=Sun, 6=Sat
  if (dow == 0 || dow == 6)                 return SESSION_CLOSED;
  if (etMin >= 570  && etMin < 960)         return SESSION_OPEN;   //  9:30–16:00 ET  (6:30–13:00 PT)
  if (etMin >= 240  && etMin < 570)         return SESSION_PRE;    //  4:00– 9:30 ET  (1:00– 6:30 PT)
  if (etMin >= 960  && etMin < 1200)        return SESSION_AFTER;  // 16:00–20:00 ET  (13:00–17:00 PT)
  return SESSION_CLOSED;
}

// ════════════════════════════════════════════════════════════════════════════
//  STARFIELD  (used when market is not open)
// ════════════════════════════════════════════════════════════════════════════
// Converts 0-255 brightness to a warm-white RGB565 colour
static inline uint16_t starCol(uint8_t b) {
  return ((uint16_t)(b >> 3) << 11) | ((uint16_t)(b >> 2) << 5) | (uint16_t)((b * 7 / 8) >> 3);
}

void initStars(int16_t x, int16_t y, int16_t w, int16_t h) {
  starBX = x; starBY = y; starBW = w; starBH = h;
  randomSeed(millis());
  for (int i = 0; i < UI_NUM_STARS; i++) {
    stars[i].x      = x + random(w);
    stars[i].y      = y + random(h);
    stars[i].bright = random(60, 230);
    stars[i].dir    = (random(2) ? UI_STAR_SPEED : -UI_STAR_SPEED);
    stars[i].sz     = (random(UI_STAR_BIG) == 0) ? 2 : 1;
  }
  for (int i = 0; i < UI_NUM_SHOOT; i++) shoots[i].active = false;
  starsReady = true;
}

// Call from loop() on a fast timer (every ~80 ms) to animate stars in-place.
// Only touches the star pixels — does NOT redraw the whole chart area.
void tickStars() {
  if (!starsReady) return;
  const uint16_t BG = g_cPanel;
  for (int i = 0; i < UI_NUM_STARS; i++) {
    Star& s = stars[i];
    // Erase old position
    if (s.sz == 2) tft.fillRect(s.x, s.y, 2, 2, BG);
    else           tft.drawPixel(s.x, s.y, BG);
    // Update brightness (twinkle)
    int nb = (int)s.bright + s.dir;
    if (nb > 230) { nb = 230; s.dir = -abs(s.dir); }
    if (nb <  40) { nb =  40; s.dir =  abs(s.dir);
      if (random(4) == 0) {              // occasionally drift to a new position
        s.x = starBX + random(starBW);
        s.y = starBY + random(starBH);
      }
    }
    s.bright = (uint8_t)nb;
    // Redraw at new brightness
    if (s.sz == 2) tft.fillRect(s.x, s.y, 2, 2, starCol(s.bright));
    else           tft.drawPixel(s.x, s.y, starCol(s.bright));
  }

  // ── Shooting stars ────────────────────────────────────────────────────────
  // Trail brightness from head → tail
  static const uint8_t TRAIL[] = { 255, 210, 140, 75, 30, 8 };

  for (int i = 0; i < UI_NUM_SHOOT; i++) {
    ShootStar& s = shoots[i];

    if (!s.active) {
      if (random(UI_SHOOT_ODDS) == 0) {
        // Speed: 22–38 px/tick horizontal, 5–16 px/tick downward
        s.dx = (float)(22 + random(16));
        s.dy = (float)(5  + random(11));
        // ~25 % chance the star flies right-to-left instead
        if (random(4) == 0) {
          s.dx = -s.dx;
          s.x  = (float)(starBX + starBW - 1 - random(starBW / 4));
        } else {
          s.x  = (float)(starBX + random(starBW / 3));
        }
        s.y      = (float)(starBY + random(starBH * 2 / 3));
        s.active = true;
      }
      continue;
    }

    // Erase old trail (draw BG along each segment)
    for (int t = 0; t < UI_SHOOT_SEGS; t++) {
      tft.drawLine(
        (int)(s.x - s.dx * t),       (int)(s.y - s.dy * t),
        (int)(s.x - s.dx * (t + 1)), (int)(s.y - s.dy * (t + 1)),
        BG);
    }

    // Advance head
    s.x += s.dx;
    s.y += s.dy;

    // Retire if it flew off the panel
    if (s.x < starBX || s.x > starBX + starBW ||
        s.y < starBY || s.y > starBY + starBH) {
      s.active = false;
      continue;
    }

    // Draw new trail — bright at head, fading toward tail
    for (int t = 0; t < UI_SHOOT_SEGS; t++) {
      tft.drawLine(
        (int)(s.x - s.dx * t),       (int)(s.y - s.dy * t),
        (int)(s.x - s.dx * (t + 1)), (int)(s.y - s.dy * (t + 1)),
        starCol(TRAIL[t]));
    }
  }
}

// Draw the full starfield from the stars[] array (call after clearing chart area)
static void drawStars() {
  for (int i = 0; i < UI_NUM_STARS; i++) {
    Star& s = stars[i];
    if (s.sz == 2) tft.fillRect(s.x, s.y, 2, 2, starCol(s.bright));
    else           tft.drawPixel(s.x, s.y, starCol(s.bright));
  }
}

// Draw clock + session label centred in the chart area
void drawClock(int cy, int ch, uint16_t bg, MarketSession sess) {
  time_t now; time(&now);
  struct tm t; gmtime_r(&now, &t);
  int etH = (t.tm_hour + g_utcOffset + 24) % 24;
  int etM = t.tm_min;
  bool pm = etH >= 12;
  int h12 = etH % 12; if (h12 == 0) h12 = 12;

  // Skip redraw if minute and session haven't changed — prevents any flicker
  static int           lastMin  = -1;
  static MarketSession lastSess = SESSION_OPEN; // SESSION_OPEN forces draw on first call
  if (etM == lastMin && sess == lastSess) return;
  lastMin  = etM;
  lastSess = sess;

  char timeBuf[8];
  snprintf(timeBuf, sizeof(timeBuf), "%d:%02d", h12, etM);

  int midY = cy + ch / 2;
  tft.setTextDatum(TL_DATUM);

  // Erase previous clock area before redrawing
  tft.fillRect(0, midY - 30, SCREEN_W, 80, bg);

  // Time digits centered — font 6 only has digits/:, no letters
  tft.setTextColor(C_WHITE, bg);
  int tw = tft.textWidth(timeBuf, 6);
  tft.drawString(timeBuf, (SCREEN_W - tw) / 2, midY - 24, 6);

  // Session label centred below
  const char* label = (sess == SESSION_PRE)   ? "PRE-MARKET"   :
                      (sess == SESSION_AFTER)  ? "AFTER HOURS"  : "MARKET CLOSED";
  tft.setTextColor(C_DARKGRAY, bg);
  int lw = tft.textWidth(label, 2);
  tft.drawString(label, (SCREEN_W - lw) / 2, midY + 34, 2);
}

// ════════════════════════════════════════════════════════════════════════════
//  DRAW: CHART MODE
// ════════════════════════════════════════════════════════════════════════════
void drawChart() {
  // ── Session detection ─────────────────────────────────────────────────────
  MarketSession session = getMarketSession();
  const SessionTheme& theme = THEMES[(int)session];

  // ── Candle colours ────────────────────────────────────────────────────────
  const uint16_t C_BULL = g_cUp;
  const uint16_t C_BEAR = g_cDown;

  // ── Panel geometry — edit the UI_* defines at the top of the file ─────────
  const int PX = UI_INSET, PY = UI_INSET;
  const int PW = SCREEN_W - PX * 2;
  const int PH = SCREEN_H - PY * 2;
  const int BORDER_R = UI_CORNER_R;
  const uint16_t C_PANEL = g_cPanel; // local alias for readability below

  // ── Background + glow border ──────────────────────────────────────────────
  // Redraw border only when needsFullRedraw is set
  // (first boot, or returning from portfolio/QR). Otherwise skip it to avoid
  // the black flash, and just clear the three content regions below.
  bool fullDraw = needsFullRedraw;
  if (fullDraw) {
    if (session == SESSION_OPEN) {
      // Market open: black background + glow rings + inset panel
      tft.fillScreen(C_BG);
      tft.drawRoundRect(PX-3, PY-3, PW+6, PH+6, BORDER_R+3, theme.g0);
      tft.drawRoundRect(PX-2, PY-2, PW+4, PH+4, BORDER_R+2, theme.g1);
      tft.drawRoundRect(PX-1, PY-1, PW+2, PH+2, BORDER_R+1, theme.g2);
      tft.drawRoundRect(PX,   PY,   PW,   PH,   BORDER_R,   theme.g3);
      tft.fillRoundRect(PX+1, PY+1, PW-2, PH-2, BORDER_R-1, C_PANEL);
    } else {
      // Non-market: fill edge-to-edge — no inset, no border
      tft.fillScreen(C_PANEL);
    }
    needsFullRedraw = false;
  } else {
    if (starsFullScreen && session == SESSION_OPEN) {
      // Transitioning from full-screen star-mode back to market open
      needsFullRedraw = true; // let fullDraw path rebuild the border cleanly next call
      starsReady = false;
      starsFullScreen = false;
      return;
    } else if (session == SESSION_OPEN) {
      tft.fillRect(PX+1, PY+1, PW-2, 37, C_PANEL); // header row only
    }
    // Non-open sessions: leave stars untouched — tickStars() animates them in-place
  }

  // ── Full-screen starfield for PRE, AFTER, and CLOSED ─────────────────────
  // Stars fill edge-to-edge (no border). Only initialised once per session entry;
  // tickStars() animates in-place and the loop() clock timer updates the time.
  if (session != SESSION_OPEN) {
    if (!starsReady || !starsFullScreen) {
      if (!fullDraw) tft.fillScreen(C_PANEL);
      initStars(0, 0, SCREEN_W, SCREEN_H);
      starsFullScreen = true;
      drawStars();
      drawClock(0, SCREEN_H, C_PANEL, session);
    }
    // Nav dots — always draw so swipes have visible feedback during closed hours
    {
      int dotY  = SCREEN_H - UI_BAR_DOTY;
      int total = g_watchlistLen + 1;
      int dotsX = (SCREEN_W - total * UI_DOT_STEP) / 2;
      for (int i = 0; i < total; i++) {
        int dx = dotsX + i * UI_DOT_STEP + UI_DOT_STEP / 2;
        bool active = (i == currentSymIdx);
        if (i == g_watchlistLen) {
          uint16_t col = active ? C_WHITE : C_DARKGRAY;
          tft.fillRect(dx - 4, dotY - 4, 9, 9, col);
        } else {
          if (active) tft.fillCircle(dx, dotY, 5, C_WHITE);
          else        tft.fillCircle(dx, dotY, 4, C_DARKGRAY);
        }
      }
    }
    return;
  }

  // Empty watchlist prompt
  if (g_watchlistLen == 0) {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_WHITE, C_BG);
    tft.setTextSize(2);
    tft.drawCentreString("No Watchlist", SCREEN_W / 2, 120, 2);
    tft.setTextSize(1);
    tft.setTextColor(C_GRAY, C_BG);
    tft.drawCentreString("Open in browser:", SCREEN_W / 2, 160, 1);
    tft.setTextColor(0xFD00, C_BG);  // orange
    tft.drawCentreString("ticker.local", SCREEN_W / 2, 180, 2);
    tft.setTextColor(C_GRAY, C_BG);
    tft.drawCentreString("Add symbols in the Watchlist section", SCREEN_W / 2, 210, 1);
    return;
  }

  bool isPortfolio = (currentSymIdx == g_watchlistLen);

  char buf[32];
  const int hy     = PY + 6;
  const int sepY   = PY + 44;
  const int CY     = sepY + 1;
  const int CH     = 218;
  const int LABEL_W = UI_LABEL_W;
  const int CX     = PX + 1;
  const int CW     = PW - 2 - LABEL_W;

  // ── Header ────────────────────────────────────────────────────────────────
  if (isPortfolio) {
    // "PORTFOLIO . $12345"  +5.2% (intraday)
    float curVal = (portTimeLen > 0) ? portfolioValues[portTimeLen-1] : 0;
    int hx = PX + 8;
    tft.setFreeFont(&FreeSansBold18pt7b);
    tft.setTextColor(C_WHITE, C_PANEL);
    hx += tft.drawString("PORTFOLIO", hx, hy);
    tft.setTextFont(4);
    if (curVal > 0) {
      tft.setTextColor(C_DARKGRAY, C_PANEL);
      hx += tft.drawString(" . ", hx, hy + 10, 4);
      snprintf(buf, sizeof(buf), "$%.0f", curVal);
      tft.setTextColor(C_WHITE, C_PANEL);
      tft.drawString(buf, hx, hy + 10, 4);
      if (portValueOpen > 0) {
        float gl = (curVal - portValueOpen) / portValueOpen * 100.0f;
        snprintf(buf, sizeof(buf), "%+.2f%%", gl);
        tft.setTextColor(gl >= 0 ? g_cUp : g_cDown, C_PANEL);
        tft.drawRightString(buf, PX + PW - 8, hy + 10, 4);
      }
    }
  } else {
    const char* sym = g_watchlist[currentSymIdx];
    bool     up = quotePctChange >= 0;
    uint16_t pc = up ? g_cUp : g_cDown;
    fmtPrice(quotePrice, buf, sizeof(buf));
    int hx = PX + 8;
    // Symbol in FreeSansBold18pt7b (~33px proper bold sans-serif)
    tft.setFreeFont(&FreeSansBold18pt7b);
    tft.setTextColor(C_WHITE, C_PANEL);
    hx += tft.drawString(sym, hx, hy);
    tft.setTextFont(4);
    tft.setTextColor(C_DARKGRAY, C_PANEL);
    hx += tft.drawString(" . ", hx, hy + 10, 4);
    tft.setTextColor(C_WHITE, C_PANEL);
    tft.drawString(buf, hx, hy + 10, 4);
    snprintf(buf, sizeof(buf), "%+.2f%%", quotePctChange);
    tft.setTextColor(pc, C_PANEL);
    tft.drawRightString(buf, PX + PW - 8, hy + 10, 4);
  }

  tft.drawFastHLine(PX+1, sepY, PW-2, C_DARKGRAY);
  tft.fillRect(CX, CY, CW + LABEL_W, CH, C_PANEL);

  // ── Chart / content area ──────────────────────────────────────────────────
  if (isPortfolio) {
    if (positionCount == 0) {
      tft.setTextColor(C_GRAY, C_PANEL);
      tft.drawCentreString("No positions.", SCREEN_W/2, CY+CH/2-8, 2);
      tft.drawCentreString("Visit ticker.local to add some.", SCREEN_W/2, CY+CH/2+10, 1);
    } else if (portTimeLen < 2) {
      tft.setTextColor(C_GRAY, C_PANEL);
      tft.drawCentreString("Loading...", SCREEN_W/2, CY+CH/2, 2);
    } else {
      // ── Portfolio value line chart ─────────────────────────────────────
      float pLow = portfolioValues[0], pHigh = portfolioValues[0];
      for (int t = 1; t < portTimeLen; t++) {
        if (portfolioValues[t] < pLow)  pLow  = portfolioValues[t];
        if (portfolioValues[t] > pHigh) pHigh = portfolioValues[t];
      }
      float pad   = (pHigh - pLow) * UI_PORT_PAD;
      float vLow  = pLow  - pad;
      float vHigh = pHigh + pad;
      float range = vHigh - vLow;
      if (range < 0.01f) range = 1.0f;

      auto toY = [&](float v) -> int {
        return CY + CH - 1 - (int)((v - vLow) / range * (CH - 1));
      };

      float curVal = portfolioValues[portTimeLen - 1];
      bool  up     = curVal >= portValueOpen;
      uint16_t lineCol = up ? g_cUp : g_cDown;
      uint16_t fillCol = up ? 0x0300 : 0x2800;

      // Open baseline (dashed horizontal)
      if (portValueOpen > vLow && portValueOpen < vHigh) {
        int baseY = toY(portValueOpen);
        for (int x = CX; x < CX + CW; x += 6)
          tft.drawFastHLine(x, baseY, 3, C_DARKGRAY);
      }

      // Grid lines + $ labels (25/50/75%)
      for (int g = 1; g <= 3; g++) {
        float frac = g * 0.25f;
        int   gy   = CY + CH - 1 - (int)(frac * (CH - 1));
        tft.drawFastHLine(CX, gy, CW, 0x1082);
        snprintf(buf, sizeof(buf), "$%.0f", vLow + frac * range);
        tft.setTextColor(C_DARKGRAY, C_PANEL);
        tft.drawString(buf, CX + CW + 3, gy - 4, 1);
      }
      snprintf(buf, sizeof(buf), "$%.0f", vHigh);
      tft.setTextColor(C_DARKGRAY, C_PANEL);
      tft.drawString(buf, CX + CW + 3, CY, 1);

      // Filled area below the line
      for (int t = 0; t < portTimeLen; t++) {
        int x = CX + (int)((float)t / (portTimeLen - 1) * (CW - 1));
        int y = toY(portfolioValues[t]);
        tft.drawFastVLine(x, y, CY + CH - y, fillCol);
      }

      // Line (2px thick)
      for (int t = 1; t < portTimeLen; t++) {
        int x1 = CX + (int)((float)(t-1) / (portTimeLen-1) * (CW-1));
        int x2 = CX + (int)((float) t    / (portTimeLen-1) * (CW-1));
        int y1 = toY(portfolioValues[t-1]);
        int y2 = toY(portfolioValues[t]);
        tft.drawLine(x1, y1,   x2, y2,   lineCol);
        tft.drawLine(x1, y1+1, x2, y2+1, lineCol);
      }

      // Current value dot
      tft.fillCircle(CX + CW - 1, toY(curVal), 4, lineCol);
    }

  } else if (candleCount > 0) {
    float minL = candles[0].low, maxH = candles[0].high;
    for (int i = 1; i < candleCount; i++) {
      if (candles[i].low  < minL) minL = candles[i].low;
      if (candles[i].high > maxH) maxH = candles[i].high;
    }
    float pad   = (maxH - minL) * UI_PRICE_PAD;
    float pLow  = minL - pad;
    float pHigh = maxH + pad;
    float range = pHigh - pLow;
    if (range < 0.0001f) range = 1.0f;

    // Maps a price to a y pixel coordinate inside the chart area
    auto toY = [&](float p) -> int {
      return CY + CH - 1 - (int)((p - pLow) / range * (CH - 1));
    };

    // Horizontal grid lines at 25 / 50 / 75 % of chart height + top label
    for (int g = 1; g <= 3; g++) {
      float frac = g * 0.25f;
      int   gy   = CY + CH - 1 - (int)(frac * (CH - 1));
      fmtPrice(pLow + frac * range, buf, sizeof(buf));
      tft.drawFastHLine(CX, gy, CW, C_DARKGRAY);
      tft.setTextColor(C_GRAY, C_PANEL);
      tft.drawString(buf, CX + CW + 3, gy - 4, 1); // price label right of chart
    }
    fmtPrice(pHigh, buf, sizeof(buf));
    tft.setTextColor(C_GRAY, C_PANEL);
    tft.drawString(buf, CX + CW + 3, CY, 1);

    // Candle bodies + wicks
    // bodyFrac = fraction of each candle slot used for the body (0.0–1.0)
    // Increase for fatter candles; decrease for thinner ones.
    float cStep    = (float)CW / (float)candleCount;
    float bodyFrac = UI_CANDLE_BODY;
    int   bodyW    = max(1, (int)(cStep * bodyFrac));
    for (int i = 0; i < candleCount; i++) {
      int   xMid = CX + (int)(i * cStep + cStep * 0.5f);
      float pO = candles[i].open, pC = candles[i].close;
      float pH = candles[i].high, pL = candles[i].low;
      int yH = toY(pH), yL = toY(pL), yO = toY(pO), yC = toY(pC);
      uint16_t col = (pC >= pO) ? C_BULL : C_BEAR;
      tft.drawFastVLine(xMid, yH, yL - yH + 1, col);       // wick
      int bTop = min(yO, yC), bBot = max(yO, yC);
      tft.fillRect(xMid - bodyW/2, bTop, bodyW, max(1, bBot - bTop), col); // body
    }
  } else {
    // No data — show placeholder + last fetch error
    tft.setTextColor(C_GRAY, C_PANEL);
    tft.drawCentreString("No Data", SCREEN_W / 2, CY + CH / 2 - 8, 2);
    if (fetchStatus[0])
      tft.drawCentreString(fetchStatus, SCREEN_W / 2, CY + CH / 2 + 12, 1);
  }

  // ── Bottom bar ────────────────────────────────────────────────────────────
  int barY = CY + CH; // top of the bottom info strip
  // In partial-redraw mode, clear just the bottom bar (no full panel fill above)
  if (!fullDraw) tft.fillRect(PX+1, barY+1, PW-2, PH-(barY-PY)-2, C_PANEL);
  tft.drawFastHLine(PX+1, barY, PW-2, C_DARKGRAY);

  // ty  = y baseline for the L / H / timeframe text row
  // dotY = y centre of the navigation dot row
  // Adjust these to move items up or down within the bottom bar.
  int ty   = barY + UI_BAR_TY;
  int dotY = barY + UI_BAR_DOTY;

  int bx = PX + 8;
  if (isPortfolio) {
    // Portfolio bottom bar: just show timeframe — L/H are per-stock, not meaningful here
    tft.setTextColor(C_DARKGRAY, C_PANEL);
    bx += tft.drawString("1M 1d", bx, ty, 2);
  } else {
    // L: low price
    /*tft.setTextColor(C_GRAY, C_PANEL);
    bx += tft.drawString("L:", bx, ty, 2);
    fmtPrice(quoteLow, buf, sizeof(buf));
    tft.setTextColor(C_WHITE, C_PANEL);
    bx += tft.drawString(buf, bx, ty, 2);

    bx += 14;

    // H: high price
    tft.setTextColor(C_GRAY, C_PANEL);
    bx += tft.drawString("H:", bx, ty, 2);
    fmtPrice(quoteHigh, buf, sizeof(buf));
    tft.setTextColor(C_WHITE, C_PANEL);
    bx += tft.drawString(buf, bx, ty, 2);

    bx += 14;

    // Timeframe label — edit TIMEFRAME_LBL define at top of file
    tft.setTextColor(C_DARKGRAY, C_PANEL);
    tft.drawString(TIMEFRAME_LBL, bx, ty, 2);
    */
  }

  // Session badge (PRE / AH / CLOSED) — right-aligned in bottom bar
  if (theme.badge[0]) {
    tft.setTextColor(theme.g3, C_PANEL);
    tft.drawRightString(theme.badge, PX + PW - 8, ty, 2);
  }

  // Navigation dots: one per watchlist symbol + one for portfolio (last)
  int totalSlots = g_watchlistLen + 1;
  int dStep = UI_DOT_STEP;
  int dotsX = (SCREEN_W - totalSlots * dStep) / 2;
  for (int i = 0; i < totalSlots; i++) {
    int dx = dotsX + i * dStep + dStep / 2;
    bool active = (i == currentSymIdx);
    if (i == g_watchlistLen) {
      // Portfolio slot — small square to distinguish from stock dots
      uint16_t col = active ? C_WHITE : C_DARKGRAY;
      tft.fillRect(dx - 4, dotY - 4, 9, 9, col);
    } else {
      if (active) tft.fillCircle(dx, dotY, 5, C_WHITE);
      else        tft.fillCircle(dx, dotY, 4, C_DARKGRAY);
    }
  }

  // Fetch error status (only visible when something went wrong)
  if (fetchStatus[0] && strcmp(fetchStatus, "ok") != 0) {
    tft.setTextColor(C_DARKGRAY, C_PANEL);
    tft.drawString(fetchStatus, PX + 8, PY + PH - 12, 1);
  }

  // Flip sprite to display (no-op in direct-draw mode)
}



// ════════════════════════════════════════════════════════════════════════════
//  PORTFOLIO FLASH STORAGE
// ════════════════════════════════════════════════════════════════════════════
void loadPortfolio() {
  prefs.begin("port", true);
  positionCount = prefs.getInt("count", 0);
  if (positionCount > MAX_POSITIONS) positionCount = MAX_POSITIONS;
  for (int i = 0; i < positionCount; i++) {
    char sk[8], hk[8], ck[8];
    snprintf(sk, sizeof(sk), "s%d", i);
    snprintf(hk, sizeof(hk), "h%d", i);
    snprintf(ck, sizeof(ck), "c%d", i);
    String sym = prefs.getString(sk, "");
    strncpy(portfolio[i].symbol, sym.c_str(), sizeof(portfolio[i].symbol) - 1);
    portfolio[i].symbol[sizeof(portfolio[i].symbol) - 1] = '\0';
    portfolio[i].shares       = prefs.getFloat(hk, 0.0f);
    portfolio[i].avgCost      = prefs.getFloat(ck, 0.0f);
    portfolio[i].currentPrice = -1.0f;
  }
  prefs.end();
}

void savePortfolio() {
  prefs.begin("port", false);
  prefs.putInt("count", positionCount);
  for (int i = 0; i < positionCount; i++) {
    char sk[8], hk[8], ck[8];
    snprintf(sk, sizeof(sk), "s%d", i);
    snprintf(hk, sizeof(hk), "h%d", i);
    snprintf(ck, sizeof(ck), "c%d", i);
    prefs.putString(sk, portfolio[i].symbol);
    prefs.putFloat(hk,  portfolio[i].shares);
    prefs.putFloat(ck,  portfolio[i].avgCost);
  }
  prefs.end();
}

// ─── Watchlist flash storage ─────────────────────────────────────────────────
void loadWatchlist() {
  File f = SPIFFS.open("/watchlist.txt", "r");
  if (f && f.size() > 0) {
    String data = f.readString();
    f.close();
    int count = 0;
    int pos = 0;
    while (pos < data.length() && count < MAX_WATCHLIST) {
      int nl = data.indexOf('\n', pos);
      if (nl == -1) nl = data.length();
      String sym = data.substring(pos, nl);
      sym.trim();
      if (sym.length() > 0) {
        strncpy(g_watchlist[count], sym.c_str(), sizeof(g_watchlist[0]) - 1);
        g_watchlist[count][sizeof(g_watchlist[0]) - 1] = '\0';
        count++;
      }
      pos = nl + 1;
    }
    g_watchlistLen = count;
    Serial.printf("Loaded %d watchlist symbols from file\n", count);
  } else {
    if (f) f.close();
    g_watchlistLen = 0;
    Serial.println("No watchlist found — fresh device");
  }
}

void saveWatchlist() {
  File f = SPIFFS.open("/watchlist.txt", "w");
  if (!f) {
    Serial.println("Failed to open watchlist file for writing");
    return;
  }
  Serial.printf("Saving watchlist: %d symbols\n", g_watchlistLen);
  for (int i = 0; i < g_watchlistLen; i++) {
    f.println(g_watchlist[i]);
    Serial.printf("  [%d] %s\n", i, g_watchlist[i]);
  }
  f.close();
  Serial.printf("Watchlist saved to /watchlist.txt\n");
}

// ─── Candle flash cache ───────────────────────────────────────────────────────
// Persists symbol candle data across power cycles for instant chart display on boot.
// fetchedAt is set to 0 on load so a background refresh is always triggered,
// but the stale candle shape shows immediately while the fetch completes.
void loadCandleCache() {
  Preferences cache;
  cache.begin("ccache", true);
  for (int i = 0; i < g_watchlistLen; i++) {
    char key[4]; snprintf(key, sizeof(key), "c%d", i);
    if (cache.getBytesLength(key) == sizeof(SymbolCache)) {
      cache.getBytes(key, &symCache[i], sizeof(SymbolCache));
      symCache[i].fetchedAt = 0; // treat as stale → background refresh on first view
    }
  }
  cache.end();
}

void saveCandleCache(int idx) {
  Preferences cache;
  cache.begin("ccache", false);
  char key[4]; snprintf(key, sizeof(key), "c%d", idx);
  cache.putBytes(key, &symCache[idx], sizeof(SymbolCache));
  cache.end();
}

// ════════════════════════════════════════════════════════════════════════════
//  PORTFOLIO PAGE HELPERS  (shared by WiFiManager portal + main web server)
// ════════════════════════════════════════════════════════════════════════════
String buildPortfolioHTML() {
  String html =
    "<!doctype html>"
    "<meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Ticker Portfolio</title>"
    "<style>"
      "*{box-sizing:border-box;margin:0;padding:0}"
      "body{background:#0b0b0b;color:#eee;font-family:sans-serif;"
           "padding:16px;max-width:480px;margin:auto}"
      "h2{color:#fd7700;margin:12px 0 16px;letter-spacing:1px}"
      ".r{display:flex;gap:6px;align-items:center;padding:8px 0;"
          "border-bottom:1px solid #222}"
      ".sym{color:#fd7700;font-weight:bold;width:58px;font-size:.95em}"
      "input{min-width:0;flex:1;background:#161616;border:1px solid #333;"
             "border-radius:6px;padding:8px;color:#eee;font-size:.9em}"
      ".hdr{color:#666;font-size:.75em;text-align:center}"
      ".add-box{background:#111;border:1px solid #333;border-radius:8px;"
                "padding:12px;margin:14px 0}"
      "button{border:0;border-radius:6px;padding:10px;font-size:.95em;"
              "font-weight:bold;cursor:pointer;width:100%;margin-top:6px}"
      ".btn-add{background:#1a3a20;color:#4ef}"
      ".btn-save{background:#fd7700;color:#000}"
      ".btn-del{background:#4a1010;color:#f88;width:36px;margin:0;flex:none}"
      "#st{text-align:center;color:#aaa;padding:8px;font-size:.9em}"
      ".thcard{display:flex;align-items:center;gap:10px;border:2px solid #333;"
               "border-radius:8px;padding:10px 14px;margin-bottom:6px;cursor:pointer}"
      ".thcard.sel{border-color:#fd7700}"
      ".sw{width:18px;height:18px;border-radius:4px;flex:none}"
    "</style>"
    "<h2>&#128200; Portfolio</h2>"
    "<div class=r>"
      "<span class=sym>SYM</span>"
      "<span class=hdr style=flex:1>Shares</span>"
      "<span class=hdr style=flex:1>Avg Cost</span>"
      "<span style=width:36px></span>"
    "</div>"
    "<div id=p>";

  for (int i = 0; i < positionCount; i++) {
    html += "<div class=r>"
            "<span class=sym>" + String(portfolio[i].symbol) + "</span>"
            "<input id=h" + i + " type=number step=.0001 value='" + String(portfolio[i].shares,  4) + "'>"
            "<input id=c" + i + " type=number step=.01   value='" + String(portfolio[i].avgCost, 2) + "'>"
            "<button class=btn-del onclick=D(" + i + ")>✕</button>"
            "</div>";
  }

  html +=
    "</div>"
    "<div class=add-box>"
      "<div class=r style='border:0;padding:4px 0'>"
        "<input id=ns maxlength=10 placeholder='SYMBOL' style=width:80px;flex:none>"
        "<input id=nh type=number step=.0001 placeholder=Shares>"
        "<input id=nc type=number step=.01   placeholder='Avg Cost'>"
      "</div>"
      "<button class=btn-add onclick=A()>+ Add Position</button>"
    "</div>"
    "<hr style='border-color:#222;margin:16px 0'>"
    "<h2 style='margin-bottom:12px'>&#9200; Clock</h2>"
    "<div class=r style='border:0'>"
      "<span style='color:#aaa;flex:1'>UTC Offset</span>"
      "<input id=tz type=number min=-12 max=14 value='" + String(g_utcOffset) + "' "
             "style='flex:none;width:70px;text-align:center'>"
    "</div>"
    "<div style='color:#555;font-size:.78em;padding:4px 0 6px'>"
      "ET&nbsp;-4/-5&nbsp;&nbsp;CT&nbsp;-5/-6&nbsp;&nbsp;MT&nbsp;-6/-7&nbsp;&nbsp;PT&nbsp;-7/-8"
    "</div>"
    "<div class=r style='border:0'>"
      "<span style='color:#aaa;flex:1'>Auto-advance (sec)</span>"
      "<input id=adv type=number min=5 max=120 value='" + String(g_advanceMs / 1000UL) + "' "
             "style='flex:none;width:70px;text-align:center'>"
    "</div>"
    "<hr style='border-color:#222;margin:16px 0'>"
    "<h2 style='margin-bottom:12px'>&#128198; Watchlist</h2>"
    "<div id=wlr></div>"
    "<div class=add-box>"
      "<div class=r style='border:0;padding:4px 0'>"
        "<input id=nw maxlength=10 placeholder='Symbol e.g. AAPL' style='text-transform:uppercase'>"
      "</div>"
      "<button class=btn-add onclick=AW()>+ Add to Watchlist</button>"
    "</div>"
    "<hr style='border-color:#222;margin:16px 0'>"
    "<h2 style='margin-bottom:12px'>&#127912; Theme</h2>"
    "<div id=thmsec>";

  for (int i = 0; i < NUM_COLOR_THEMES; i++) {
    bool sel = (i == g_themeIdx);
    html += "<div class='thcard" + String(sel ? " sel" : "") + "' onclick=ST(" + i + ")>"
            "<span class=sw style='background:" + COLOR_THEMES[i].webUp + "'></span>"
            "<span class=sw style='background:" + COLOR_THEMES[i].webDown + "'></span>"
            "<span style='flex:1'>" + COLOR_THEMES[i].name + "</span>"
            "</div>";
  }

  html +=
    "</div>"
    "<hr style='border-color:#222;margin:16px 0'>"
    "<button class=btn-save onclick=S()>Save to Device</button>"
    "<p id=st></p>"
    "<script>"
      "let R=[";

  for (int i = 0; i < positionCount; i++) {
    if (i) html += ",";
    html += "{s:'" + String(portfolio[i].symbol)
            + "',h:" + String(portfolio[i].shares, 4)
            + ",c:" + String(portfolio[i].avgCost, 2) + "}";
  }

  html +=
      "];"
      "function D(i){R.splice(i,1);V()}"
      "function A(){"
        "let s=ns.value.trim().toUpperCase(),h=+nh.value,c=+nc.value;"
        "if(!s||h<=0||c<=0){st.textContent='Fill all fields';return}"
        "R.push({s,h,c});ns.value=nh.value=nc.value='';V()"
      "}"
      "function V(){"
        "let p=document.getElementById('p');p.innerHTML='';"
        "R.forEach((r,i)=>p.innerHTML+=`<div class=r>"
          "<span class=sym>${r.s}</span>"
          "<input id=h${i} type=number step=.0001 value=${r.h}>"
          "<input id=c${i} type=number step=.01 value=${r.c}>"
          "<button class=btn-del onclick=D(${i})>✕</button>"
        "</div>`)"
      "}"
      "let W=[";

  for (int i = 0; i < g_watchlistLen; i++) {
    if (i) html += ",";
    html += "'" + String(g_watchlist[i]) + "'";
  }

  html +=
      "];"
      "function RW(i){W.splice(i,1);VW()}"
      "function AW(){"
        "let s=nw.value.trim().toUpperCase();"
        "if(!s){st.textContent='Enter a symbol';return}"
        "if(W.length>=12){st.textContent='Max 12 symbols';return}"
        "W.push(s);nw.value='';VW()"
      "}"
      "function VW(){"
        "let d=document.getElementById('wlr');d.innerHTML='';"
        "W.forEach((s,i)=>d.innerHTML+=`<div class=r>"
          "<span class=sym style='flex:1'>${s}</span>"
          "<button class=btn-del style='width:36px;margin:0;padding:8px 0' onclick=RW(${i})>✕</button>"
        "</div>`)"
      "}"
      "let TH=" + String(g_themeIdx) + ";"
      "function ST(i){"
        "TH=i;"
        "document.querySelectorAll('.thcard').forEach((c,j)=>c.classList.toggle('sel',j===i))"
      "}"
      "function S(){"
        "let d=document;"
        "let positions=R.map((r,i)=>({symbol:r.s,"
          "shares:+d.getElementById('h'+i).value,"
          "avgCost:+d.getElementById('c'+i).value}));"
        "let tz=+d.getElementById('tz').value;"
        "let advanceSec=+d.getElementById('adv').value;"
        "st.textContent='Saving…';"
        "fetch('/save',{method:'POST',"
          "headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({positions,tz,watchlist:W,themeIdx:TH,advanceSec})})"
        ".then(r=>r.text()).then(t=>st.textContent=t)"
        ".catch(_=>st.textContent='Save failed')"
      "}"
      "VW();"
    "</script>";

  return html;
}

// Parses JSON body and writes positions + optional tz offset to flash.
// Returns empty string on success, error message on failure.
String handlePortfolioSave(const String& body) {
  Serial.printf("Save body (%d bytes): %.120s\n", body.length(), body.c_str());

  if (body.length() == 0) return "Empty body";

  DynamicJsonDocument doc(8192);  // Increased from 6144
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON parse failed: %s (error code: %d)\n", err.c_str(), err.code());
    Serial.printf("Body length: %d, buffer capacity: 8192\n", body.length());
    return String("JSON error: ") + err.c_str();
  }
  Serial.println("JSON parsed successfully");

  // UTC offset (optional — only sent from main web UI)
  if (doc.containsKey("tz")) {
    g_utcOffset = doc["tz"] | g_utcOffset;
    Preferences cfg; cfg.begin("cfg", false); cfg.putInt("tz", g_utcOffset); cfg.end();
    Serial.printf("UTC offset → %d\n", g_utcOffset);
  }

  JsonArray positions = doc["positions"].as<JsonArray>();
  Serial.printf("Positions in JSON: %d\n", (int)positions.size());
  positionCount = 0;
  for (JsonObject p : positions) {
    if (positionCount >= MAX_POSITIONS) break;
    String sym = p["symbol"] | "";
    sym.toUpperCase(); sym.trim();
    if (sym.length() == 0) continue;
    strncpy(portfolio[positionCount].symbol, sym.c_str(), sizeof(portfolio[0].symbol) - 1);
    portfolio[positionCount].symbol[sizeof(portfolio[0].symbol) - 1] = '\0';
    portfolio[positionCount].shares       = p["shares"]  | 0.0f;
    portfolio[positionCount].avgCost      = p["avgCost"] | 0.0f;
    portfolio[positionCount].currentPrice = -1.0f;
    Serial.printf("  [%d] %s  %.4f sh @ %.2f\n", positionCount,
                  portfolio[positionCount].symbol,
                  portfolio[positionCount].shares,
                  portfolio[positionCount].avgCost);
    Serial.flush();
    positionCount++;
  }
  Serial.println("About to save portfolio...");
  Serial.flush();
  savePortfolio();
  Serial.printf("Saved %d positions\n", positionCount);
  Serial.flush();

  // Watchlist (optional)
  Serial.printf("JSON has watchlist key: %s\n", doc.containsKey("watchlist") ? "YES" : "NO");
  if (doc.containsKey("watchlist")) {
    JsonArray wl = doc["watchlist"].as<JsonArray>();
    int newLen = 0;
    for (JsonVariant v : wl) {
      if (newLen >= MAX_WATCHLIST) break;
      String sym = v.as<String>();
      sym.toUpperCase(); sym.trim();
      if (sym.length() == 0) continue;
      strncpy(g_watchlist[newLen], sym.c_str(), sizeof(g_watchlist[0]) - 1);
      g_watchlist[newLen][sizeof(g_watchlist[0]) - 1] = '\0';
      newLen++;
    }
    if (newLen > 0) {
      g_watchlistLen = newLen;
      saveWatchlist();
      // Invalidate in-memory cache so background fetch re-populates correct symbols
      for (int i = 0; i < MAX_WATCHLIST; i++) symCache[i].fetchedAt = 0;
      // Clamp current index to valid range
      if (currentSymIdx > g_watchlistLen) currentSymIdx = 0;
      Serial.printf("Watchlist updated: %d symbols\n", g_watchlistLen);
    }
  }

  // Color theme (optional)
  if (doc.containsKey("themeIdx")) {
    int th = doc["themeIdx"] | 0;
    if (th >= 0 && th < NUM_COLOR_THEMES) {
      g_themeIdx = th;
      g_cUp    = COLOR_THEMES[th].cUp;
      g_cDown  = COLOR_THEMES[th].cDown;
      g_cPanel = COLOR_THEMES[th].cPanel;
      Preferences cfg; cfg.begin("cfg", false); cfg.putInt("th", th); cfg.end();
      Serial.printf("Theme → %d (%s)\n", th, COLOR_THEMES[th].name);
    }
  }

  // Auto-advance interval (optional)
  if (doc.containsKey("advanceSec")) {
    int sec = doc["advanceSec"] | 15;
    if (sec < 5) sec = 5; if (sec > 120) sec = 120;
    g_advanceMs = (unsigned long)sec * 1000UL;
    Preferences cfg; cfg.begin("cfg", false); cfg.putInt("adv", sec); cfg.end();
    Serial.printf("Auto-advance → %d s\n", sec);
  }

  return "";
}

// ════════════════════════════════════════════════════════════════════════════
//  WEB SERVER  (port 80, served from ESP32 after WiFi connects)
// ════════════════════════════════════════════════════════════════════════════
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", buildPortfolioHTML());
  });

  server.on("/save", HTTP_POST, []() {
    // arg("plain") can be empty for application/json on some WebServer builds —
    // fall back to reading directly from the client stream.
    String body = server.arg("plain");
    if (body.length() == 0 && server.client().available()) {
      body = server.client().readString();
    }
    String errMsg = handlePortfolioSave(body);
    if (errMsg.length()) {
      server.send(400, "text/plain", errMsg);
      return;
    }
    server.send(200, "text/plain",
      "Saved " + String(positionCount) + " position(s), " +
      String(g_watchlistLen) + " watchlist symbol(s).");
    drawChart();
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found.");
  });
}
