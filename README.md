# Ticker

A DIY desk stock and crypto ticker built on the **ESP32-3248S035C** development board. Displays live intraday candlestick charts, tracks a personal portfolio, and is fully configurable through a mobile-friendly web portal — no re-flashing required.

![Hardware](https://img.shields.io/badge/board-ESP32--3248S035C-blue)
![Display](https://img.shields.io/badge/display-480×320%20TFT-orange)
![Platform](https://img.shields.io/badge/platform-Arduino-teal)

---

## Hardware

| Component | Detail |
|---|---|
| Board | ESP32-3248S035C |
| Display | 480 × 320 TFT (landscape) |
| Touch | GT911 capacitive touchscreen |
| Connectivity | WiFi (built-in) |

---

## Features

### Display
- **5-minute candlestick charts** for up to 12 watchlist symbols
- **Portfolio line chart** — combined intraday $ value across all tracked positions
- **Market session awareness** — glow border changes color by session:
  - No border — Market open
  - Yellow — Pre-market
  - Purple — After-hours
  - Blue — Market closed
- **Starfield + shooting star animation** during closed/pre/after-hours
- **Live clock** (top-right corner, NTP-synced)
- **Navigation dots** at the bottom — one per symbol, plus a square for the portfolio slot

### Navigation
- **Swipe left/right** to cycle through watchlist symbols and the portfolio view
- **Auto-advance** — automatically cycles through symbols on a configurable interval (default 15 s, only during market hours)

### Web Portal
Accessible at **http://ticker.local** (or the device IP) after WiFi is connected. No app required — works from any phone browser.

- **Portfolio editor** — add/remove positions with shares and average cost
- **Watchlist editor** — add/remove ticker symbols (up to 12); default: SPY, LLY, MU, MSFT, GOOG, NVDA
- **Clock** — set UTC offset for the on-screen clock
- **Auto-advance interval** — set the auto-cycle time in seconds (5–120)
- **Color themes** — choose from 6 presets (see below)

All settings persist to flash — they survive power cycles and re-flashing the firmware.

### Color Themes

| Theme | Up color | Down color | Panel |
|---|---|---|---|
| Classic | Green | Red | Dark navy |
| Cyan / Orange | Cyan | Orange | Dark navy |
| Amber | Yellow | Red-orange | Dark charcoal |
| Monochrome | White | Dark gray | Black |
| Neon | Green | Magenta | Black |
| Navy | Cyan | Orange | Navy blue |

### Data
- **Source:** Yahoo Finance Chart API (`query1` / `query2.finance.yahoo.com`)
- **Timeframe:** 1-day, 5-minute intervals (up to 56 candles)
- **Supports:** US equities, ETFs, and crypto (e.g. `BTC-USD`, `ETH-USD`)
- **Cache:** Candle data cached for 5 minutes; last fetch persisted to flash for instant display on boot
- **Fetch interval:** Every 30 seconds in the background (Core 0), UI stays smooth on Core 1

---

## Libraries Required

Install all via the Arduino Library Manager or manually:

| Library | Notes |
|---|---|
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | TFT display driver — requires `User_Setup.h` config for this board |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | Captive portal for WiFi setup |
| [ArduinoJson](https://arduinojson.org/) | v6 — JSON parsing for Yahoo Finance responses |
| Preferences | Built-in ESP32 library — flash storage |
| HTTPClient | Built-in ESP32 — HTTPS requests |
| ESPmDNS | Built-in ESP32 — `ticker.local` hostname |

---

## First-Time Setup

1. Flash `ticker.ino` via Arduino IDE
2. On boot, the screen shows: **"Connect to WiFi: TICKER-SETUP"**
3. On your phone, connect to the `TICKER-SETUP` WiFi network
4. A captive portal opens automatically — configure:
   - Your WiFi network and password
   - UTC offset for the clock
5. The device connects, displays its IP address for 4 seconds, then starts showing charts
6. Open **http://ticker.local** in your browser to set up your portfolio and watchlist

---

## Web Portal

After initial WiFi setup the portal is always available at:
- `http://ticker.local`
- `http://<device-ip>`

### Saving changes
All sections (Portfolio, Clock, Auto-advance, Watchlist, Theme) are saved together with a single **"Save to Device"** button. The display redraws immediately.

### Portfolio tracking
Each position stores:
- Symbol (e.g. `AAPL`)
- Number of shares
- Average cost basis

The portfolio view fetches live prices for each position and plots combined intraday value as a line chart, colored green or red relative to the opening value.

---

## Configuration (compile-time)

All tuneable values live at the top of `ticker.ino` under clearly marked sections. You should not need to edit anything else.

```cpp
#define UTC_OFFSET  -7        // Default timezone (overridden by portal)
#define MAX_CANDLES  56       // Max 5-min candles to display
#define CACHE_TTL_MS 300000   // Candle cache TTL (5 min)

// UI layout
#define UI_INSET       5      // Border inset from screen edge (px)
#define UI_CORNER_R   10      // Rounded corner radius (px)
#define UI_CANDLE_BODY 0.6f   // Candle body width (fraction of slot)
#define UI_NUM_STARS  60      // Starfield density
#define UI_NUM_SHOOT   3      // Max simultaneous shooting stars
```

---

## Architecture

```
Core 1 (main loop)          Core 0 (fetch task)
─────────────────           ───────────────────
Touch handling              Yahoo Finance HTTPS requests
Chart rendering             Candle data parsing
Star animation              Cache updates
Web server                  Portfolio price updates
Auto-advance
```

Flash namespaces used by Preferences:
| Namespace | Contents |
|---|---|
| `cfg` | UTC offset, color theme index, auto-advance interval |
| `port` | Portfolio positions (up to 10) |
| `wlist` | Watchlist symbols (up to 12) |
| `ccache` | Candle data cache per symbol slot |

---

## Project Structure

```
ticker/
├── ticker.ino    # Main sketch (~1700 lines, fully self-contained)
├── qrcode.c      # QR code library (included, reserved for future use)
├── qrcode.h
└── README.md
```

---

## Notes

- The RGB LED on the board is disabled at startup (GPIOs 4, 16, 17, 2 held HIGH) to avoid interference with the WiFi stack
- Market session detection uses US Eastern time with automatic DST switching (EDT/EST)
- Crypto symbols like `BTC-USD` work the same as equity symbols — just add them to the watchlist
- Auto-advance only cycles during market open hours; during closed sessions the star screen is static so there is nothing to cycle
