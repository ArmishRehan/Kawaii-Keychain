/*
  ============================================================
   Kawaii Face Keychain
   ESP32-C3 Super Mini + 128x64 I2C OLED (SH1106 or SSD1306)
  ============================================================

  Wiring:
    OLED VDD -> 3V3        OLED GND -> GND
    OLED SDA -> GPIO 8     OLED SCK -> GPIO 9
    Button   -> GPIO 3 and GND (use two DIAGONAL legs of a 4-pin button)

  What it does:
    - Shows 16 animated kawaii faces (they blink, bounce, blush...)
    - Tap the button   -> next face (after the last one it goes back to the first)
    - Hold the button  -> menu: Faces / Mochi Run / Float / Stack / Reaction / Quotes / Stopwatch
        Menu:      tap = next item, hold = choose
        Mochi Run: tap = jump over the thorns, catch hearts for +10. Hold = back to menu
        Float:     tap = flap, glide through the gaps. Hold = menu
        Stack:     tap = drop the sliding block on the tower. Hold = menu
        Reaction:  wait for the eyes to pop open, then tap as fast as you can. Hold = menu
        Quotes:    tap = next quote. Hold = menu
        Stopwatch: tap = start/pause, hold while paused = reset, otherwise hold = menu
    - Remembers the last face and your best scores, even after you switch it off
    - After AUTO_SLEEP_MINUTES without a press it takes a nap (screen off,
      tiny power use). Press the button to wake it up.

  Libraries (Arduino IDE > Tools > Manage Libraries):
    "Adafruit SH110X" (or "Adafruit SSD1306" if USE_SH1106 is 0) - say YES to
    also install Adafruit GFX + Adafruit BusIO.
    Preferences is built into the ESP32 board package.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// ================= Settings you can change =================
#define USE_SH1106          1     // 1 = SH1106 screen chip, 0 = SSD1306. If the picture is squeezed
                                  //     into a thin strip with noise below it, flip this.
#define SDA_PIN             8
#define SCL_PIN             9
#define BUTTON_PIN          3

#define OLED_CONTRAST       0x30  // 0x00 = dimmest ... 0xFF = brightest. Lower = longer battery life
#define FRAME_MS            50    // time between frames: 50 ms = 20 frames per second
#define GAME_FRAME_MS       33    // the runner game runs faster (about 30 frames per second)
#define DEBOUNCE_MS         25    // button must be stable this long to count as a press
#define LONG_PRESS_MS       700   // hold this long to open the menu
#define SAVE_LAST_FACE      1     // 1 = remember the face after power off, 0 = always start with the first face
#define AUTO_SLEEP_MINUTES  5     // nap after this many minutes without a press (0 = never nap)
#define DEBUG_SERIAL        1     // 1 = print messages to the Serial Monitor (set to 0 for the final keychain)

// Engineer Armish Rehan's quote for the Quotes screen - paste it between the quote marks.
// While it's empty, it's simply skipped.
#define ARMISH_REHAN_QUOTE  "Zarmeen is \"pickme\""
// ===========================================================

#define SCREEN_W 128
#define SCREEN_H 64

#if DEBUG_SERIAL
  #define LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(...)
#endif

#if USE_SH1106
  #include <Adafruit_SH110X.h>
  Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);  // -1 = the OLED has no reset pin
  #define WHITE SH110X_WHITE
  #define BLACK SH110X_BLACK
#else
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
#endif
Preferences prefs;                                         // saves the face number and best scores in flash

// The faces, in the order the button steps through them
enum { HAPPY, SLEEPY, SURPRISED, LOVE, WINK, SHY, CRYING, ANGRY, STARRY, KITTY,
       DIZZY, SMUG, NERVOUS, KISS, COOL, PANDA, FACE_COUNT };
const char* const FACE_NAMES[FACE_COUNT] = {
  "happy", "sleepy", "surprised", "in love", "wink",
  "shy", "crying", "angry", "starry", "kitty",
  "dizzy", "smug", "nervous", "kiss", "cool", "panda"
};

// Where the face parts sit on the screen (before any bounce/shake)
const int EYE_LX = 40, EYE_RX = 88, EYE_Y = 26;
const int MOUTH_X = 64, MOUTH_Y = 44;
const int BLUSH_LX = 22, BLUSH_RX = 106, BLUSH_Y = 38;

// Positions for the current frame = base positions + bounce offset (filled in by setPositions)
struct Positions { int ox, oy, lx, rx, ey, mx, my, blx, brx, by; };
Positions pos;

int currentFace = HAPPY;
unsigned long faceChangedAt = 0;   // when the current face appeared (for the little hop + name label)
unsigned long lastFrameAt = 0;
unsigned long lastActivityAt = 0;  // last button press (for auto-nap)

// Blink state
const unsigned long BLINK_MS = 200;
bool blinking = false;
unsigned long blinkStartAt = 0;
unsigned long nextBlinkAt = 2000;

// Button state (for debouncing)
int buttonReading = HIGH;          // what the pin reads right now (may bounce)
int buttonStable = HIGH;           // the debounced, trusted state
unsigned long buttonFlipAt = 0;    // when the reading last changed
unsigned long pressStartAt = 0;    // when the current press began
bool longFired = false;            // true once the current press counted as a long press

// What the button just did (readButton returns one of these)
enum { BTN_NONE, BTN_DOWN, BTN_TAP, BTN_LONG };

// Which screen we're on
enum { MODE_FACES, MODE_MENU, MODE_RUNNER, MODE_FLOAT, MODE_STACK, MODE_REACTION, MODE_QUOTES, MODE_STOPWATCH };
int mode = MODE_FACES;

// Menu
const int MENU_COUNT = 7;
const char* const MENU_ITEMS[MENU_COUNT] = {
  "Faces", "Mochi Run", "Float", "Stack", "Reaction", "Quotes", "Stopwatch"
};
int menuIndex = 0;
unsigned long menuAt = 0;          // last press in the menu (it closes itself after a while)

// Game states (shared by all the games)
enum { GAME_READY, GAME_PLAYING, GAME_OVER };

// Mochi Run (the runner game)
const int GROUND_Y = 55;           // y of the ground line
const int BLOB_X = 18, BLOB_R = 6; // where the player stands and how big it is
const float GRAVITY = 0.00049f;    // pixels per ms per ms
const float JUMP_V = 0.147f;       // pixels per ms -> a hop about 22 px high, 0.6 s long
struct Obstacle { float x; int w, h; bool active; };
struct Pickup { float x; int y; bool active; };
Obstacle obs[3];
Pickup heart;
int runState = GAME_READY;
unsigned long runStateAt = 0;      // when the current game state began
unsigned long heartPopAt = 0;      // when a heart was last caught (shows "+10")
float blobY = 0, blobV = 0;        // height above the ground and upward speed
float speed = 0.06f;               // pixels per ms
float distance = 0, nextSpawnIn = 0;
float cloudX[2] = { 30, 95 };
int bonus = 0;
uint16_t runBest = 0;
bool newBest = false;

// Reaction game
enum { RX_INTRO, RX_WAIT, RX_GO, RX_RESULT, RX_EARLY };
int rxState = RX_INTRO;
unsigned long rxAt = 0, rxGoAt = 0, rxTime = 0;
uint16_t rxBest = 0;               // 0 = no best time yet

// Float (tap to flap through the gaps)
const int FLOAT_X = 26;            // where the player flies
const int COL_W = 12, GAP_H = 28;  // pillar width and the size of the gap
const float FLOAT_GRAVITY = 0.00035f, FLAP_V = -0.11f;
struct Column { float x; int gapY; bool passed, active; };
Column cols[3];
int flState = GAME_READY, flScore = 0, flLastGap = 20;
unsigned long flStateAt = 0, flFlapAt = 0;
float flY = 28, flV = 0;
uint16_t flBest = 0;
bool flNewBest = false;

// Stack (drop the sliding block on the tower)
const int STACK_H = 5;             // pixels per layer (4 px block + 1 px gap)
const int STACK_BASE_Y = 59;       // top of the bottom block
const int STACK_START_W = 48;
int stState = GAME_READY, stLevel = 0;
int stTopX = 0, stTopW = STACK_START_W;   // the block on top of the tower
int stXs[16], stWs[16];                   // the last 16 layers, for drawing
float stX = 0, stDir = 1;                 // the sliding block
float cutX = 0, cutY = 0, cutV = 0;       // the piece that got cut off and falls away
int cutW = 0;
bool cutActive = false;
unsigned long stStateAt = 0, stPerfectAt = 0;
uint16_t stBest = 0;
bool stNewBest = false;

// Quotes
struct Quote { const char* text; const char* author; };
const Quote QUOTES[] = {
  { ARMISH_REHAN_QUOTE,                       "Engr. Armish Rehan" },
  { "Small steps still move you forward.",    "mochi" },
  { "Be soft. Be brave. Be you.",             "mochi" },
  { "Every bug you fix makes you stronger.",  "mochi" },
  { "Rest is part of the work.",              "mochi" },
  { "You are doing better than you think.",   "mochi" },
  { "Stay curious, stay kind.",               "mochi" },
  { "Debug your day: one problem at a time.", "mochi" },
  { "Even tiny lights glow in the dark.",     "mochi" },
  { "Drink water. Then conquer the world.",   "mochi" },
  { "Done with love beats done perfectly.",   "mochi" },
};
const int QUOTE_COUNT = sizeof(QUOTES) / sizeof(QUOTES[0]);
int quoteIndex = 0;
char qLines[6][22];                // the current quote, word-wrapped into lines

// Stopwatch
bool swRunning = false;
unsigned long swStartAt = 0, swElapsed = 0;   // swElapsed = time banked before the last start


// ---------------------------------------------------------------
//  Small math helper
// ---------------------------------------------------------------

// A smooth back-and-forth value between -1 and +1 that repeats every periodMs
float wave(unsigned long t, unsigned long periodMs, float phase) {
  return sinf((t % periodMs) * TWO_PI / periodMs + phase);
}


// ---------------------------------------------------------------
//  Drawing helpers (shapes the GFX library doesn't have)
// ---------------------------------------------------------------

// Part of a circle outline. Angles in degrees: 0 = right, 90 = down, 180 = left, 270 = up.
// thick = how many pixels thick the line is (grows inward).
void drawArc(int cx, int cy, int r, int fromDeg, int toDeg, int thick, uint16_t color) {
  for (int k = 0; k < thick; k++) {
    int rr = r - k;
    int px = cx + (int)lroundf(rr * cosf(fromDeg * DEG_TO_RAD));
    int py = cy + (int)lroundf(rr * sinf(fromDeg * DEG_TO_RAD));
    for (int a = fromDeg; a < toDeg; ) {
      a = min(a + 10, toDeg);
      int x = cx + (int)lroundf(rr * cosf(a * DEG_TO_RAD));
      int y = cy + (int)lroundf(rr * sinf(a * DEG_TO_RAD));
      display.drawLine(px, py, x, y, color);
      px = x;
      py = y;
    }
  }
}

// A filled oval. rx = half width, ry = half height.
void fillEllipse(int cx, int cy, int rx, int ry, uint16_t color) {
  if (ry <= 0) {
    display.drawFastHLine(cx - rx, cy, 2 * rx + 1, color);
    return;
  }
  for (int y = -ry; y <= ry; y++) {
    float f = 1.0f - (float)(y * y) / (ry * ry);
    int w = (int)(rx * sqrtf(f) + 0.5f);
    display.drawFastHLine(cx - w, cy + y, 2 * w + 1, color);
  }
}

// An oval filled with a checkerboard pattern - looks "grey", perfect for blush
void ditherEllipse(int cx, int cy, int rx, int ry) {
  for (int y = -ry; y <= ry; y++) {
    float f = 1.0f - (float)(y * y) / (ry * ry);
    int w = (int)(rx * sqrtf(f) + 0.5f);
    for (int x = -w; x <= w; x++) {
      if (((cx + x + cy + y) & 1) == 0) display.drawPixel(cx + x, cy + y, WHITE);
    }
  }
}

// A filled heart. (cx, cy) is the middle of the two round tops, r = size of each round top.
void fillHeart(int cx, int cy, int r) {
  display.fillCircle(cx - r, cy, r, WHITE);
  display.fillCircle(cx + r, cy, r, WHITE);
  display.fillTriangle(cx - 2 * r, cy + 1, cx + 2 * r, cy + 1, cx, cy + 2 * r + 1, WHITE);
}

// A 4-pointed twinkle star
void drawSparkle(int x, int y, int s, uint16_t color) {
  display.drawFastVLine(x, y - s, 2 * s + 1, color);
  display.drawFastHLine(x - s, y, 2 * s + 1, color);
  if (s >= 3) {
    display.drawPixel(x - 1, y - 1, color);
    display.drawPixel(x + 1, y - 1, color);
    display.drawPixel(x - 1, y + 1, color);
    display.drawPixel(x + 1, y + 1, color);
  }
}

// A water drop: round bottom at (x, y), pointy top
void fillDrop(int x, int y, int r) {
  display.fillCircle(x, y, r, WHITE);
  display.fillTriangle(x - r, y, x + r, y, x, y - 2 * r - 1, WHITE);
}

// Blush on both cheeks. r = how big (half width).
void drawBlush(int r) {
  ditherEllipse(pos.blx, pos.by, r, r / 2 + 1);
  ditherEllipse(pos.brx, pos.by, r, r / 2 + 1);
}

// A big open "D" smile: flat top at cy, round bottom
void drawOpenSmile(int cx, int cy, int r) {
  display.fillCircle(cx, cy, r, WHITE);
  display.fillRect(cx - r, cy - r, 2 * r + 1, r, BLACK);  // erase the top half
}

// A round shiny eye. open: 1.0 = fully open, 0.0 = closed (used for blinking).
void drawRoundEye(int cx, int cy, int rx, int ry, float open) {
  if (open < 0.2f) {                                // closed: just a line
    display.fillRect(cx - rx, cy, 2 * rx + 1, 2, WHITE);
    return;
  }
  int h = max(1, (int)(ry * open + 0.5f));
  fillEllipse(cx, cy, rx, h, WHITE);
  if (open > 0.6f) {                                // the little shine makes it cute
    display.fillCircle(cx + rx / 3, cy - h / 2, max(1, rx / 3), BLACK);
    display.drawPixel(cx - rx / 3, cy + h / 2, BLACK);
  }
}

// A closed happy eye shaped like "^"
void drawHappyEye(int cx, int cy) {
  drawArc(cx, cy + 4, 7, 190, 350, 3, WHITE);
}


// ---------------------------------------------------------------
//  The faces. Each one draws itself using pos (which already includes bounce).
//  t = time in ms (for animation), open = how open the eyes are (for blinking)
// ---------------------------------------------------------------

// 0. Happy  ^_^
void faceHappy(unsigned long t) {
  drawBlush(7 + lroundf(wave(t, 2000, 0)));      // blush gently pulses
  drawHappyEye(pos.lx, pos.ey);
  drawHappyEye(pos.rx, pos.ey);
  drawOpenSmile(pos.mx, pos.my - 3, 7);
}

// 1. Sleepy  - closed eyes, breathing mouth, floating z's
void faceSleepy(unsigned long t) {
  drawArc(pos.lx, pos.ey - 2, 7, 20, 160, 2, WHITE);   // closed eyes curved downward
  drawArc(pos.rx, pos.ey - 2, 7, 20, 160, 2, WHITE);
  int r = (wave(t, 4000, 0) > 0) ? 4 : 3;              // little "o" mouth breathes in and out
  display.drawCircle(pos.mx + 3, pos.my + 1, r, WHITE);

  // Two z's float up one after the other; each grows as it rises
  display.setTextColor(WHITE);
  for (int i = 0; i < 2; i++) {
    unsigned long ph = (t + i * 1500) % 3000;          // 0..2999, each z has its own cycle
    display.setTextSize(ph > 1500 ? 2 : 1);
    display.setCursor(100 + ph / 250, 24 - ph / 150);  // drift right and up
    display.print('z');
  }
}

// A wide-open ring eye (for surprised), squashes when blinking
void drawRingEye(int cx, int cy, float open, int pupil) {
  if (open < 0.2f) {
    display.fillRect(cx - 9, cy, 19, 2, WHITE);
    return;
  }
  fillEllipse(cx, cy, 9, max(1, (int)(9 * open + 0.5f)), WHITE);
  fillEllipse(cx, cy, 7, max(0, (int)(7 * open + 0.5f)), BLACK);
  if (open > 0.6f) display.fillCircle(cx, cy, pupil, WHITE);
}

// 2. Surprised  O_O
void faceSurprised(unsigned long t, float open) {
  int pupil = 2 + (t / 300) % 2;                        // pupils tremble a little
  drawRingEye(pos.lx, pos.ey, open, pupil);
  drawRingEye(pos.rx, pos.ey, open, pupil);
  drawArc(pos.lx, pos.ey - 4, 12, 235, 305, 1, WHITE);  // raised eyebrows
  drawArc(pos.rx, pos.ey - 4, 12, 235, 305, 1, WHITE);
  display.drawCircle(pos.mx, pos.my + 2, 4, WHITE);     // small "o" mouth
  display.drawCircle(pos.mx, pos.my + 2, 3, WHITE);
  fillDrop(112 + pos.ox, 14 + pos.oy + (t % 1600) / 200, 3);  // sweat drop sliding down
}

// 3. In love - beating heart eyes + a tiny heart floating away
void faceLove(unsigned long t) {
  unsigned long beat = t % 1000;
  int r = (beat < 120 || (beat > 240 && beat < 360)) ? 6 : 5;   // "ba-dum" double beat
  drawBlush(7);
  fillHeart(pos.lx, pos.ey - r / 2 - 1, r);
  fillHeart(pos.rx, pos.ey - r / 2 - 1, r);
  drawArc(pos.mx, pos.my - 5, 7, 30, 150, 2, WHITE);   // smile

  unsigned long ph = t % 2500;
  int y = 20 - (int)(ph / 110);                         // rises from y=20 off the top
  if (y > -4) fillHeart(114 + pos.ox, y, 2);
}

// 4. Wink - one open eye, one ^ eye, twinkle star, tongue out
void faceWink(unsigned long t, float open) {
  drawBlush(6);
  drawRoundEye(pos.lx, pos.ey, 6, 8, open);
  drawHappyEye(pos.rx, pos.ey);
  int sz = 2 + lroundf(1.5f + 1.5f * wave(t, 900, 0));   // star size 2..5
  drawSparkle(pos.rx + 15, pos.ey - 9, sz, WHITE);
  drawArc(pos.mx, pos.my - 5, 7, 30, 150, 2, WHITE);     // smile
  display.fillCircle(pos.mx + 3, pos.my + 3, 3, WHITE);  // tongue
  display.drawFastVLine(pos.mx + 3, pos.my + 3, 3, BLACK);
}

// 5. Shy / blushing - eyes looking away, big pulsing blush with /// lines, wobbly mouth
void faceShy(unsigned long t, float open) {
  drawBlush(9 + lroundf(wave(t, 1400, 0)));              // 8..10, pulses faster than happy
  for (int i = -1; i <= 1; i++) {                        // the "///" blush lines
    int x1 = pos.blx + i * 4, x2 = pos.brx + i * 4;
    display.drawLine(x1 - 1, pos.by + 2, x1 + 2, pos.by - 2, WHITE);
    display.drawLine(x2 - 1, pos.by + 2, x2 + 2, pos.by - 2, WHITE);
  }
  int look = lroundf(2 * wave(t, 3000, 0));              // eyes drift side to side
  drawRoundEye(pos.lx + look, pos.ey + 3, 5, 6, open);
  drawRoundEye(pos.rx + look, pos.ey + 3, 5, 6, open);
  for (int x = -6; x <= 6; x++) {                        // wavy "~" mouth
    int y = (int)lroundf(1.5f * sinf(x * 0.9f));
    display.fillRect(pos.mx + x, pos.my + y, 1, 2, WHITE);
  }
}

// One "T" eye with tears running down it
void drawTearyEye(int cx, unsigned long t) {
  display.fillRect(cx - 8, pos.ey - 4, 17, 3, WHITE);              // top bar of the "T"
  display.drawFastVLine(cx, pos.ey - 1, SCREEN_H - pos.ey, WHITE);  // stream of tears
  for (int k = 0; k < 3; k++) {                                     // drops sliding down
    int y = pos.ey + (int)((t / 30 + k * 9) % 27);
    display.fillRect(cx - 1, y, 3, 3, WHITE);
  }
}

// 6. Crying  T_T
void faceCrying(unsigned long t) {
  drawTearyEye(pos.lx, t);
  drawTearyEye(pos.rx, t);
  int wob = (t / 150) % 2;                                    // trembling lip
  drawArc(pos.mx + wob, pos.my + 7, 7, 210, 330, 2, WHITE);   // frown
}

// The cartoon "anger mark" (four little curves). d = how spread out it is.
void drawAngerMark(int cx, int cy, int d) {
  drawArc(cx - d, cy - d, 3, 0, 90, 2, WHITE);
  drawArc(cx + d, cy - d, 3, 90, 180, 2, WHITE);
  drawArc(cx + d, cy + d, 3, 180, 270, 2, WHITE);
  drawArc(cx - d, cy + d, 3, 270, 360, 2, WHITE);
}

// 7. Angry - slanted brows, frown, throbbing anger mark (the whole face shakes, see updateMotion)
void faceAngry(unsigned long t, float open) {
  for (int k = 0; k < 3; k++) {   // thick eyebrows slanting down toward the middle
    display.drawLine(pos.lx - 10, pos.ey - 13 + k, pos.lx + 8, pos.ey - 6 + k, WHITE);
    display.drawLine(pos.rx + 10, pos.ey - 13 + k, pos.rx - 8, pos.ey - 6 + k, WHITE);
  }
  int h = max(1, (int)(5 * open + 0.5f));
  fillEllipse(pos.lx + 1, pos.ey + 2, 5, h, WHITE);
  fillEllipse(pos.rx - 1, pos.ey + 2, 5, h, WHITE);
  drawArc(pos.mx, pos.my + 8, 8, 215, 325, 2, WHITE);   // frown
  drawAngerMark(108 + pos.ox, 12 + pos.oy, 4 + (t / 250) % 2);
}

// A big sparkly anime eye
void drawStarEye(int cx, int cy, float open, unsigned long t) {
  if (open < 0.2f) {
    display.fillRect(cx - 8, cy, 17, 2, WHITE);
    return;
  }
  int h = max(1, (int)(10 * open + 0.5f));
  fillEllipse(cx, cy, 8, h, WHITE);
  if (open > 0.6f) {
    display.fillCircle(cx + 3, cy - 4, 2 + (t / 300) % 2, BLACK);   // twinkling big shine
    display.fillCircle(cx - 3, cy + 5, 1, BLACK);                   // small shine
  }
}

// 8. Starry-eyed / excited - shiny eyes, open smile, sparkles all around
void faceStarry(unsigned long t, float open) {
  drawStarEye(pos.lx, pos.ey, open, t);
  drawStarEye(pos.rx, pos.ey, open, t);
  drawOpenSmile(pos.mx, pos.my - 2, 7);
  const int sx[4] = { 12, 116, 118, 10 };
  const int sy[4] = { 10, 8, 52, 54 };
  for (int i = 0; i < 4; i++) {          // each sparkle twinkles at a different moment
    int sz = lroundf(3 * wave(t, 1300, i * 1.6f));
    if (sz > 0) drawSparkle(sx[i] + pos.ox, sy[i] + pos.oy, sz, WHITE);
  }
}

// 9. Kitty  - ears, whiskers, omega mouth; one ear twitches now and then
void faceKitty(unsigned long t, float open) {
  int tw = (t % 3500 < 180) ? 3 : 0;
  display.drawTriangle(pos.ox + 8,   pos.oy + 20, pos.ox + 28,  pos.oy + 9, pos.ox + 10,       pos.oy + 2,      WHITE);
  display.drawTriangle(pos.ox + 120, pos.oy + 20, pos.ox + 100, pos.oy + 9, pos.ox + 118 + tw, pos.oy + 2 + tw, WHITE);
  drawRoundEye(pos.lx, pos.ey + 2, 5, 6, open);
  drawRoundEye(pos.rx, pos.ey + 2, 5, 6, open);
  display.fillTriangle(pos.mx - 3, pos.my - 7, pos.mx + 3, pos.my - 7, pos.mx, pos.my - 4, WHITE);  // nose
  drawArc(pos.mx - 4, pos.my - 4, 4, 0, 180, 2, WHITE);   // the two bumps of the "w" mouth
  drawArc(pos.mx + 4, pos.my - 4, 4, 0, 180, 2, WHITE);
  display.drawLine(pos.ox + 4,   pos.oy + 36, pos.ox + 22,  pos.oy + 39, WHITE);   // whiskers
  display.drawLine(pos.ox + 4,   pos.oy + 46, pos.ox + 22,  pos.oy + 43, WHITE);
  display.drawLine(pos.ox + 124, pos.oy + 36, pos.ox + 106, pos.oy + 39, WHITE);
  display.drawLine(pos.ox + 124, pos.oy + 46, pos.ox + 106, pos.oy + 43, WHITE);
}


// A spiral eye that spins (for dizzy)
void drawSpiralEye(int cx, int cy, unsigned long t) {
  float spin = (t % 1000) * TWO_PI / 1000;
  int px = cx, py = cy;
  for (int a = 30; a <= 720; a += 30) {   // two turns, getting wider
    float r = a / 720.0f * 8;
    float ang = a * DEG_TO_RAD + spin;
    int x = cx + (int)lroundf(r * cosf(ang));
    int y = cy + (int)lroundf(r * sinf(ang));
    display.drawLine(px, py, x, y, WHITE);
    px = x;
    py = y;
  }
}

// 10. Dizzy - spinning spiral eyes, wobbly mouth, stars circling the head
void faceDizzy(unsigned long t) {
  drawSpiralEye(pos.lx, pos.ey, t);
  drawSpiralEye(pos.rx, pos.ey, t);
  for (int x = -7; x <= 7; x++) {
    int y = (int)lroundf(2 * sinf(x * 0.8f + (t % 2000) * TWO_PI / 2000));
    display.fillRect(pos.mx + x, pos.my + y, 1, 2, WHITE);
  }
  for (int i = 0; i < 3; i++) {
    float a = (t % 1500) * TWO_PI / 1500 + i * TWO_PI / 3;
    drawSparkle(64 + pos.ox + (int)(34 * cosf(a)), 7 + (int)(4 * sinf(a)), 2, WHITE);
  }
}

// A half-closed "heh" eye: heavy lid with the eye peeking out underneath
void drawSmugEye(int cx, int cy, float open) {
  if (open > 0.3f) {
    display.fillCircle(cx + 3, cy + 1, 4, WHITE);
    display.fillRect(cx - 2, cy - 4, 10, 3, BLACK);   // cut off the top
  }
  display.fillRect(cx - 8, cy - 1, 17, 2, WHITE);     // the lid
}

// 11. Smug - half-lidded eyes, one eyebrow up, side smirk
void faceSmug(unsigned long t, float open) {
  drawBlush(5);
  drawSmugEye(pos.lx, pos.ey, open);
  drawSmugEye(pos.rx, pos.ey, open);
  drawArc(pos.rx, pos.ey - 4, 9, 220, 320, 1, WHITE);   // raised eyebrow
  drawArc(pos.mx + 4, pos.my - 6, 8, 30, 110, 2, WHITE);
}

// 12. Nervous - shivering eyes, worried brows, zig-zag mouth, sweat drops
void faceNervous(unsigned long t, float open) {
  int jit = ((t / 80) % 2) ? 1 : -1;
  drawRoundEye(pos.lx + jit, pos.ey + 1, 5, 6, open);
  drawRoundEye(pos.rx + jit, pos.ey + 1, 5, 6, open);
  display.drawLine(pos.lx - 8, pos.ey - 8, pos.lx + 5, pos.ey - 12, WHITE);   // inner ends up = worried
  display.drawLine(pos.rx + 8, pos.ey - 8, pos.rx - 5, pos.ey - 12, WHITE);
  for (int x = -8; x < 8; x += 4) {
    display.drawLine(pos.mx + x, pos.my, pos.mx + x + 2, pos.my - 2, WHITE);
    display.drawLine(pos.mx + x + 2, pos.my - 2, pos.mx + x + 4, pos.my, WHITE);
  }
  fillDrop(108 + pos.ox, 14 + pos.oy + (t % 1200) / 150, 3);
  fillDrop(18 + pos.ox, 20 + pos.oy + ((t + 600) % 1200) / 150, 2);
}

// 13. Kiss - one eye closed, "3" lips, little hearts drifting away
void faceKiss(unsigned long t, float open) {
  drawBlush(7);
  drawHappyEye(pos.lx, pos.ey);
  drawRoundEye(pos.rx, pos.ey, 6, 8, open);
  drawArc(pos.mx - 2, pos.my - 4, 3, 270, 450, 2, WHITE);
  drawArc(pos.mx - 2, pos.my + 2, 3, 270, 450, 2, WHITE);
  for (int i = 0; i < 2; i++) {
    unsigned long ph = (t + i * 900) % 1800;
    fillHeart(pos.mx + 10 + ph / 60, pos.my - 4 - ph / 70, ph < 900 ? 1 : 2);
  }
}

// 14. Cool - sunglasses with a shine sweeping across, relaxed smirk
void faceCool(unsigned long t) {
  display.fillRoundRect(pos.lx - 12, pos.ey - 6, 24, 13, 4, WHITE);
  display.fillRoundRect(pos.rx - 12, pos.ey - 6, 24, 13, 4, WHITE);
  display.drawLine(pos.lx + 12, pos.ey - 3, pos.rx - 12, pos.ey - 3, WHITE);   // bridge
  display.drawLine(pos.lx - 12, pos.ey - 4, pos.lx - 22, pos.ey - 7, WHITE);   // arms
  display.drawLine(pos.rx + 12, pos.ey - 4, pos.rx + 22, pos.ey - 7, WHITE);
  unsigned long ph = t % 3000;
  if (ph < 600) {   // the shine: two black streaks gliding over the lenses
    int sx = pos.lx - 20 + (int)(ph * (pos.rx - pos.lx + 40) / 600);
    for (int k = 0; k < 2; k++) {
      display.drawLine(sx + k * 4, pos.ey + 7, sx + k * 4 + 8, pos.ey - 7, BLACK);
    }
  }
  drawArc(pos.mx + 3, pos.my - 6, 8, 30, 110, 2, WHITE);
}

// A panda eye: a big patch with a small shiny eye inside (lit pixels are the "dark" fur)
void drawPandaEye(int cx, int cy, float open) {
  fillEllipse(cx, cy + 1, 9, 7, WHITE);
  if (open < 0.2f) {
    display.fillRect(cx - 3, cy, 7, 2, BLACK);
    return;
  }
  fillEllipse(cx, cy, 3, max(1, (int)(3 * open + 0.5f)), BLACK);
  if (open > 0.6f) display.drawPixel(cx + 1, cy - 1, WHITE);
}

// 15. Panda - round ears, eye patches, munching every few seconds
void facePanda(unsigned long t, float open) {
  display.fillCircle(pos.ox + 12, pos.oy + 4, 9, WHITE);
  display.fillCircle(pos.ox + 116, pos.oy + 4, 9, WHITE);
  drawPandaEye(pos.lx + 2, pos.ey + 2, open);
  drawPandaEye(pos.rx - 2, pos.ey + 2, open);
  display.fillTriangle(pos.mx - 4, pos.my - 8, pos.mx + 4, pos.my - 8, pos.mx, pos.my - 4, WHITE);   // nose
  bool chew = (t % 4000 < 1500) && ((t / 200) % 2);   // munch munch
  if (chew) {
    display.drawCircle(pos.mx, pos.my, 2, WHITE);
  } else {
    drawArc(pos.mx - 4, pos.my - 4, 4, 0, 180, 2, WHITE);
    drawArc(pos.mx + 4, pos.my - 4, 4, 0, 180, 2, WHITE);
  }
}


// ---------------------------------------------------------------
//  Animation timing
// ---------------------------------------------------------------

// Returns how open the eyes are right now: 1.0 = open, 0.0 = closed
float eyeOpenness(unsigned long t) {
  if (!blinking && (long)(t - nextBlinkAt) >= 0) {
    blinking = true;
    blinkStartAt = t;
  }
  if (!blinking) return 1.0f;

  unsigned long e = t - blinkStartAt;
  if (e >= BLINK_MS) {
    blinking = false;
    // usually wait 2.5-5.5 s before the next blink, but 1 in 5 times do a quick double blink
    nextBlinkAt = t + (random(100) < 20 ? 200 : random(2500, 5500));
    return 1.0f;
  }
  float p = (float)e / BLINK_MS;          // 0 -> 1 through the blink
  if (p < 0.3f) return 1.0f - p / 0.3f;   // closing
  if (p < 0.6f) return 0.0f;              // closed for a moment
  return (p - 0.6f) / 0.4f;               // opening
}

void setPositions(int ox, int oy) {
  pos.ox = ox;              pos.oy = oy;
  pos.lx = EYE_LX + ox;     pos.rx = EYE_RX + ox;     pos.ey = EYE_Y + oy;
  pos.mx = MOUTH_X + ox;    pos.my = MOUTH_Y + oy;
  pos.blx = BLUSH_LX + ox;  pos.brx = BLUSH_RX + ox;  pos.by = BLUSH_Y + oy;
}

// Decides how the whole face moves this frame (bounce, shake...)
void updateMotion(unsigned long t) {
  int ox = 0, oy = 0;
  switch (currentFace) {
    case SLEEPY: oy = lroundf(2 * wave(t, 4000, 0)); break;   // slow sleepy breathing
    case CRYING: oy = (t / 150) % 2;                 break;   // little sobbing hiccups
    case ANGRY:  ox = random(-1, 2);                 break;   // trembling with rage
    case LOVE:   oy = lroundf(2 * wave(t, 1000, 0)); break;   // excited, faster bounce
    case DIZZY:  ox = lroundf(3 * wave(t, 1600, 0));          // woozy swaying
                 oy = lroundf(wave(t, 800, 0));      break;
    case NERVOUS: ox = (t / 120) % 2;                break;   // can't keep still
    case COOL:   oy = (t / 400) % 2;                 break;   // nodding along to music
    default:     oy = lroundf(2 * wave(t, 1800, 0)); break;   // gentle idle bounce
  }
  unsigned long since = t - faceChangedAt;
  if (since < 300) oy -= (int)(5 * sinf(PI * since / 300.0f));   // a little hop when a face appears
  setPositions(ox, oy);
}

// Shows the face's name at the bottom for a moment after switching
void drawLabel(const char* text) {
  int w = strlen(text) * 6;     // the built-in font is 6 pixels per letter
  int x = (SCREEN_W - w) / 2;
  display.fillRect(x - 2, 55, w + 3, 9, BLACK);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(x, 56);
  display.print(text);
}

void drawFrame(unsigned long t) {
  float open = eyeOpenness(t);
  updateMotion(t);
  display.clearDisplay();
  switch (currentFace) {
    case HAPPY:     faceHappy(t);           break;
    case SLEEPY:    faceSleepy(t);          break;
    case SURPRISED: faceSurprised(t, open); break;
    case LOVE:      faceLove(t);            break;
    case WINK:      faceWink(t, open);      break;
    case SHY:       faceShy(t, open);       break;
    case CRYING:    faceCrying(t);          break;
    case ANGRY:     faceAngry(t, open);     break;
    case STARRY:    faceStarry(t, open);    break;
    case KITTY:     faceKitty(t, open);     break;
    case DIZZY:     faceDizzy(t);           break;
    case SMUG:      faceSmug(t, open);      break;
    case NERVOUS:   faceNervous(t, open);   break;
    case KISS:      faceKiss(t, open);      break;
    case COOL:      faceCool(t);            break;
    case PANDA:     facePanda(t, open);     break;
  }
  if (t - faceChangedAt < 900) drawLabel(FACE_NAMES[currentFace]);
  display.display();   // send the finished picture to the OLED
}

// Text centred across the screen. size 1 = 6x8 pixel letters, size 2 = 12x16.
void drawCentered(const char* text, int y, int size) {
  display.setTextSize(size);
  display.setTextColor(WHITE);
  display.setCursor((SCREEN_W - (int)strlen(text) * 6 * size) / 2, y);
  display.print(text);
}


// ---------------------------------------------------------------
//  Menu (hold the button to open it)
// ---------------------------------------------------------------

void openMenu(unsigned long now) {
  mode = MODE_MENU;
  menuAt = now;
  lastFrameAt = 0;   // draw straight away
}

void startFaces(unsigned long now) {
  mode = MODE_FACES;
  faceChangedAt = now;   // little hop + name label
  lastFrameAt = 0;
}

void startRunner(unsigned long now);
void startFloat(unsigned long now);
void startStack(unsigned long now);
void startReaction(unsigned long now);
void startQuotes(unsigned long now);
void startStopwatch(unsigned long now);

void menuInput(int ev, unsigned long now) {
  if (ev == BTN_TAP) {
    menuIndex = (menuIndex + 1) % MENU_COUNT;
    menuAt = now;
  } else if (ev == BTN_LONG) {
    switch (menuIndex) {
      case 0: startFaces(now);     break;
      case 1: startRunner(now);    break;
      case 2: startFloat(now);     break;
      case 3: startStack(now);     break;
      case 4: startReaction(now);  break;
      case 5: startQuotes(now);    break;
      case 6: startStopwatch(now); break;
    }
  } else if (now - menuAt > 15000) {
    startFaces(now);   // nobody's choosing - go back to the faces
  }
}

void menuDraw() {
  const int ROWS = 4;   // how many items fit on the screen at once
  int first = constrain(menuIndex - 1, 0, MENU_COUNT - ROWS);   // scroll so the choice stays visible
  display.clearDisplay();
  drawCentered("tap:next hold:ok", 0, 1);
  for (int r = 0; r < ROWS; r++) {
    int i = first + r;
    int y = 14 + r * 12;
    if (i == menuIndex) {   // highlighted item: white bar with black text
      display.fillRoundRect(6, y - 2, 110, 12, 3, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }
    display.setTextSize(1);
    display.setCursor(16, y);
    display.print(MENU_ITEMS[i]);
  }
  for (int i = 0; i < MENU_COUNT; i++) {   // little dots on the right show where you are
    int y = 14 + i * 6;
    if (i == menuIndex) display.fillCircle(123, y, 2, WHITE);
    else display.drawPixel(123, y, WHITE);
  }
  display.display();
}


// ---------------------------------------------------------------
//  Mochi Run - a one-button runner. Tap to hop over thorns, catch hearts.
// ---------------------------------------------------------------

void runnerReset(unsigned long now) {
  blobY = 0;
  blobV = 0;
  speed = 0.06f;
  distance = 0;
  bonus = 0;
  nextSpawnIn = 60;
  for (int i = 0; i < 3; i++) obs[i].active = false;
  heart.active = false;
  runStateAt = now;
}

void startRunner(unsigned long now) {
  mode = MODE_RUNNER;
  runState = GAME_READY;
  runnerReset(now);
  lastFrameAt = 0;
}

int runScore() {
  return (int)(distance / 8) + bonus;
}

void spawnObstacle() {
  for (int i = 0; i < 3; i++) {
    if (obs[i].active) continue;
    int spikes = random(1, 4);
    obs[i].x = SCREEN_W + 2;
    obs[i].w = spikes * 6;
    obs[i].h = random(8, 13);
    obs[i].active = true;
    // The next gap is always long enough to land and hop again (longer when faster)
    nextSpawnIn = obs[i].w + speed * 650 + random(25, 90);
    if (!heart.active && random(100) < 35) {   // sometimes a heart floats above the thorns
      heart.active = true;
      heart.x = obs[i].x + obs[i].w / 2;
      heart.y = GROUND_Y - 30;
    }
    return;
  }
  nextSpawnIn = 10;   // all slots busy, try again soon
}

// Does a circle touch a box? (used for collisions in the games)
bool circleHitsRect(float cx, float cy, float r, float left, float top, float right, float bottom) {
  float nx = constrain(cx, left, right);
  float ny = constrain(cy, top, bottom);
  float dx = cx - nx, dy = cy - ny;
  return dx * dx + dy * dy < r * r;
}

// Does the blob touch obstacle i? The thorns count as a slightly
// smaller box than they look, so near-misses feel fair.
bool blobHits(int cx, int cy, int i) {
  return circleHitsRect(cx, cy, BLOB_R - 1, obs[i].x + 2, GROUND_Y - obs[i].h + 3,
                        obs[i].x + obs[i].w - 2, GROUND_Y);
}

void runnerGameOver(unsigned long now) {
  runState = GAME_OVER;
  runStateAt = now;
  newBest = runScore() > runBest;
  if (newBest) {
    runBest = runScore();
    prefs.putUShort("runBest", runBest);
  }
}

void runnerInput(int ev, unsigned long now) {
  if (ev == BTN_LONG) { openMenu(now); return; }
  if (ev != BTN_DOWN) return;
  if (runState == GAME_READY) {
    runState = GAME_PLAYING;
    runnerReset(now);
  } else if (runState == GAME_PLAYING) {
    if (blobY < 1.0f) blobV = JUMP_V;   // only hop when standing on the ground
  } else if (now - runStateAt > 700) {  // short pause after crashing so you don't restart by accident
    runState = GAME_PLAYING;
    runnerReset(now);
  }
}

void runnerUpdate(unsigned long dt, unsigned long now) {
  if (runState != GAME_PLAYING) return;

  speed = min(0.06f + (now - runStateAt) / 1000.0f * 0.002f, 0.15f);   // slowly speeds up
  float step = speed * dt;
  distance += step;

  if (blobY > 0 || blobV > 0) {         // in the air: move, then let gravity pull down
    blobY += blobV * dt;
    blobV -= GRAVITY * dt;
    if (blobY <= 0) { blobY = 0; blobV = 0; }
  }

  for (int i = 0; i < 2; i++) {         // clouds drift slower than the ground
    cloudX[i] -= step * 0.25f;
    if (cloudX[i] < -20) cloudX[i] = SCREEN_W + random(0, 40);
  }
  for (int i = 0; i < 3; i++) {
    if (!obs[i].active) continue;
    obs[i].x -= step;
    if (obs[i].x + obs[i].w < 0) obs[i].active = false;
  }
  if (heart.active) {
    heart.x -= step;
    if (heart.x < -6) heart.active = false;
  }

  nextSpawnIn -= step;
  if (nextSpawnIn <= 0) spawnObstacle();

  int cx = BLOB_X, cy = GROUND_Y - BLOB_R - (int)blobY;
  for (int i = 0; i < 3; i++) {
    if (obs[i].active && blobHits(cx, cy, i)) {
      runnerGameOver(now);
      return;
    }
  }
  if (heart.active && abs((int)heart.x - cx) < 8 && abs(heart.y - cy) < 8) {
    heart.active = false;
    bonus += 10;
    heartPopAt = now;
  }
}

// The player: a round little mochi. mood 0 = running, 1 = hopping (^ ^), 2 = crashed (x x)
void drawBlob(int cx, int cy, int mood) {
  display.fillCircle(cx, cy, BLOB_R, WHITE);
  if (mood == 2) {
    display.drawLine(cx - 4, cy - 3, cx - 2, cy - 1, BLACK);
    display.drawLine(cx - 4, cy - 1, cx - 2, cy - 3, BLACK);
    display.drawLine(cx + 2, cy - 3, cx + 4, cy - 1, BLACK);
    display.drawLine(cx + 2, cy - 1, cx + 4, cy - 3, BLACK);
  } else if (mood == 1) {
    display.drawLine(cx - 4, cy - 1, cx - 3, cy - 2, BLACK);
    display.drawLine(cx - 3, cy - 2, cx - 2, cy - 1, BLACK);
    display.drawLine(cx + 2, cy - 1, cx + 3, cy - 2, BLACK);
    display.drawLine(cx + 3, cy - 2, cx + 4, cy - 1, BLACK);
  } else {
    display.fillRect(cx - 3, cy - 2, 2, 2, BLACK);
    display.fillRect(cx + 2, cy - 2, 2, 2, BLACK);
  }
  display.drawFastHLine(cx - 1, cy + 2, 3, BLACK);   // little mouth
}

// A row of n thorns, h pixels tall, starting at x
void drawThorns(int x, int n, int h) {
  for (int k = 0; k < n; k++) {
    display.fillTriangle(x + k * 6, GROUND_Y, x + k * 6 + 5, GROUND_Y, x + k * 6 + 2, GROUND_Y - h, WHITE);
  }
}

void drawCloud(int x, int y) {
  // white puffs, then black insides, leaves just the outline of the whole cloud
  display.fillCircle(x, y, 4, WHITE);
  display.fillCircle(x + 5, y - 2, 5, WHITE);
  display.fillCircle(x + 10, y, 4, WHITE);
  display.fillCircle(x, y, 3, BLACK);
  display.fillCircle(x + 5, y - 2, 4, BLACK);
  display.fillCircle(x + 10, y, 3, BLACK);
}

void runnerDraw(unsigned long now) {
  display.clearDisplay();

  if (runState == GAME_PLAYING) {
    drawCloud((int)cloudX[0], 14);
    drawCloud((int)cloudX[1], 22);
  }

  display.drawFastHLine(0, GROUND_Y, SCREEN_W, WHITE);
  for (int i = 0; i < 6; i++) {         // pebbles scroll by so you can feel the speed
    int px = ((i * 29 - (int)distance) % SCREEN_W + SCREEN_W) % SCREEN_W;
    display.drawPixel(px, GROUND_Y + 3, WHITE);
    display.drawFastHLine((px + 13) % SCREEN_W, GROUND_Y + 6, 2, WHITE);
  }

  for (int i = 0; i < 3; i++) {
    if (obs[i].active) drawThorns((int)obs[i].x, obs[i].w / 6, obs[i].h);
  }
  if (heart.active) fillHeart((int)heart.x, heart.y - 1, 2);

  int cy = GROUND_Y - BLOB_R - (int)blobY;
  int mood = 0;
  if (runState == GAME_OVER) mood = 2;
  else if (blobY > 0) mood = 1;
  else if (runState == GAME_PLAYING) cy -= ((int)(distance / 6)) % 2;   // tiny running bob
  else cy -= (wave(now, 1200, 0) > 0) ? 1 : 0;                        // idle bounce
  drawBlob(BLOB_X, cy, mood);

  char buf[20];
  display.setTextSize(1);
  display.setTextColor(WHITE);
  snprintf(buf, sizeof buf, "HI %u", runBest);
  display.setCursor(0, 0);
  display.print(buf);

  if (runState == GAME_READY) {
    drawCentered("MOCHI RUN", 16, 1);
    drawCentered("tap to start", 30, 1);
  } else {
    snprintf(buf, sizeof buf, "%d", runScore());
    display.setCursor(SCREEN_W - (int)strlen(buf) * 6, 0);
    display.print(buf);
  }
  if (runState == GAME_OVER) {
    drawCentered("oops!", 14, 2);
    if (newBest && (now / 300) % 2) drawCentered("NEW BEST!", 36, 1);
  }
  if (now - heartPopAt < 400) {
    display.setCursor(BLOB_X + 8, cy - 14);
    display.print("+10");
  }
  display.display();
}


// ---------------------------------------------------------------
//  Reaction game - wait for the eyes to pop open, then tap!
// ---------------------------------------------------------------

void startReaction(unsigned long now) {
  mode = MODE_REACTION;
  rxState = RX_INTRO;
  rxAt = now;
  lastFrameAt = 0;
}

void reactionInput(int ev, unsigned long now) {
  if (ev == BTN_LONG) { openMenu(now); return; }
  if (ev != BTN_DOWN) return;
  switch (rxState) {
    case RX_INTRO:
      rxState = RX_WAIT;
      rxGoAt = now + random(1500, 4500);   // random wait so you can't guess it
      break;
    case RX_WAIT:
      rxState = RX_EARLY;                  // pressed before the eyes opened
      rxAt = now;
      break;
    case RX_GO:
      rxTime = now - rxAt;
      rxState = RX_RESULT;
      rxAt = now;
      if (rxBest == 0 || rxTime < rxBest) {
        rxBest = rxTime;
        prefs.putUShort("rxBest", rxBest);
      }
      break;
    default:                               // RX_RESULT / RX_EARLY: tap again for a new round
      if (now - rxAt > 500) {
        rxState = RX_WAIT;
        rxGoAt = now + random(1500, 4500);
      }
      break;
  }
}

void reactionDraw(unsigned long now) {
  if (rxState == RX_WAIT && (long)(now - rxGoAt) >= 0) {
    rxState = RX_GO;
    rxAt = now;
  }
  setPositions(0, lroundf(2 * wave(now, 1800, 0)));
  display.clearDisplay();
  char buf[24];
  switch (rxState) {
    case RX_INTRO:
      faceSleepy(now);
      drawLabel("tap when eyes open");
      break;
    case RX_WAIT:
      faceSleepy(now);
      break;
    case RX_GO:
      faceSurprised(now, 1.0f);
      drawLabel("NOW!");
      break;
    case RX_RESULT:
      faceHappy(now);
      snprintf(buf, sizeof buf, "%lu ms  best %u", rxTime, rxBest);
      drawLabel(buf);
      break;
    case RX_EARLY:
      faceAngry(now, 1.0f);
      drawLabel("too soon!");
      break;
  }
  display.display();
}


// Score line shared by the games: best on the left, current score on the right
void drawScores(uint16_t best, int score, bool showScore) {
  char buf[12];
  display.setTextSize(1);
  display.setTextColor(WHITE);
  snprintf(buf, sizeof buf, "HI %u", best);
  display.setCursor(0, 0);
  display.print(buf);
  if (showScore) {
    snprintf(buf, sizeof buf, "%d", score);
    display.setCursor(SCREEN_W - (int)strlen(buf) * 6, 0);
    display.print(buf);
  }
}

// The "oops!" screen shared by the games (on a black box so it's readable over the playfield)
void drawGameOver(bool best, unsigned long now) {
  display.fillRect(28, 11, 72, 34, BLACK);
  drawCentered("oops!", 14, 2);
  if (best && (now / 300) % 2) drawCentered("NEW BEST!", 34, 1);
}


// ---------------------------------------------------------------
//  Float - tap to flap, glide through the gaps between the pillars
// ---------------------------------------------------------------

void floatReset(unsigned long now) {
  flY = 28;
  flV = 0;
  flScore = 0;
  flLastGap = 18;
  for (int i = 0; i < 3; i++) cols[i].active = false;
  flStateAt = now;
}

void startFloat(unsigned long now) {
  mode = MODE_FLOAT;
  flState = GAME_READY;
  floatReset(now);
  lastFrameAt = 0;
}

void floatGameOver(unsigned long now) {
  flState = GAME_OVER;
  flStateAt = now;
  flNewBest = flScore > flBest;
  if (flNewBest) {
    flBest = flScore;
    prefs.putUShort("flBest", flBest);
  }
}

void spawnColumn() {
  for (int i = 0; i < 3; i++) {
    if (cols[i].active) continue;
    // the gap moves at most 20 px from the last one, so it's always reachable
    flLastGap = constrain(flLastGap + (int)random(-20, 21), 6, SCREEN_H - GAP_H - 6);
    cols[i].x = SCREEN_W + 2;
    cols[i].gapY = flLastGap;
    cols[i].passed = false;
    cols[i].active = true;
    return;
  }
}

void floatInput(int ev, unsigned long now) {
  if (ev == BTN_LONG) { openMenu(now); return; }
  if (ev != BTN_DOWN) return;
  if (flState == GAME_OVER && now - flStateAt <= 700) return;   // short pause after crashing
  if (flState != GAME_PLAYING) {
    flState = GAME_PLAYING;
    floatReset(now);
  }
  flV = FLAP_V;
  flFlapAt = now;
}

void floatUpdate(unsigned long dt, unsigned long now) {
  if (flState != GAME_PLAYING) return;

  flV += FLOAT_GRAVITY * dt;
  flY += flV * dt;
  if (flY < BLOB_R) { flY = BLOB_R; flV = 0; }                  // bump the ceiling gently
  if (flY > SCREEN_H - BLOB_R) { floatGameOver(now); return; }   // fell to the ground

  float step = min(0.045f + flScore * 0.001f, 0.08f) * dt;       // a little faster every pillar
  float rightmost = -100;
  for (int i = 0; i < 3; i++) {
    if (!cols[i].active) continue;
    cols[i].x -= step;
    if (!cols[i].passed && cols[i].x + COL_W < FLOAT_X) {
      cols[i].passed = true;
      flScore++;
    }
    if (cols[i].x + COL_W < 0) {
      cols[i].active = false;
      continue;
    }
    rightmost = max(rightmost, cols[i].x);
    float x = cols[i].x;
    if (circleHitsRect(FLOAT_X, flY, BLOB_R - 1, x, -10, x + COL_W, cols[i].gapY) ||
        circleHitsRect(FLOAT_X, flY, BLOB_R - 1, x, cols[i].gapY + GAP_H, x + COL_W, SCREEN_H + 10)) {
      floatGameOver(now);
      return;
    }
  }
  if (rightmost < SCREEN_W - 58) spawnColumn();
}

void floatDraw(unsigned long now) {
  display.clearDisplay();
  for (int i = 0; i < 3; i++) {
    if (!cols[i].active) continue;
    int x = (int)cols[i].x, g = cols[i].gapY;
    display.drawRoundRect(x, -4, COL_W, g + 4, 3, WHITE);
    display.drawRoundRect(x, g + GAP_H, COL_W, SCREEN_H - g - GAP_H + 4, 3, WHITE);
    for (int y = 3; y < SCREEN_H; y += 6) {   // dotted pattern inside the pillars
      if (y < g - 2 || y > g + GAP_H + 2) display.drawPixel(x + 3 + (y / 6 % 2) * 5, y, WHITE);
    }
  }

  int cy = (int)flY;
  if (flState == GAME_READY) cy += (wave(now, 1200, 0) > 0) ? 1 : 0;
  bool flap = now - flFlapAt < 150;
  int wingY = flap ? cy - 5 : cy + 3;   // wings up right after a flap
  display.drawTriangle(FLOAT_X - 6, cy - 1, FLOAT_X - 6, cy + 2, FLOAT_X - 11, wingY, WHITE);
  display.drawTriangle(FLOAT_X + 6, cy - 1, FLOAT_X + 6, cy + 2, FLOAT_X + 11, wingY, WHITE);
  drawBlob(FLOAT_X, cy, flState == GAME_OVER ? 2 : (flap ? 1 : 0));

  drawScores(flBest, flScore, flState != GAME_READY);
  if (flState == GAME_READY) {
    drawCentered("FLOAT", 16, 1);
    drawCentered("tap to flap", 30, 1);
  }
  if (flState == GAME_OVER) drawGameOver(flNewBest, now);
  display.display();
}


// ---------------------------------------------------------------
//  Stack - tap to drop the sliding block. Overhangs get cut off.
// ---------------------------------------------------------------

void stackReset(unsigned long now) {
  stLevel = 0;
  stTopW = STACK_START_W;
  stTopX = (SCREEN_W - STACK_START_W) / 2;
  stXs[0] = stTopX;
  stWs[0] = stTopW;
  stX = 0;
  stDir = 1;
  cutActive = false;
  stStateAt = now;
}

void startStack(unsigned long now) {
  mode = MODE_STACK;
  stState = GAME_READY;
  stackReset(now);
  lastFrameAt = 0;
}

// How far the tower has scrolled down, so the top always stays on screen
int stackCam() {
  return max(0, (stLevel + 1) * STACK_H - 30);
}

int layerY(int level) {
  return STACK_BASE_Y - level * STACK_H + stackCam();
}

void stackGameOver(unsigned long now) {
  stState = GAME_OVER;
  stStateAt = now;
  stNewBest = stLevel > stBest;
  if (stNewBest) {
    stBest = stLevel;
    prefs.putUShort("stBest", stBest);
  }
}

void stackDrop(unsigned long now) {
  int mx = (int)lroundf(stX);
  int left = max(mx, stTopX);
  int right = min(mx + stTopW, stTopX + stTopW);

  if (abs(mx - stTopX) <= 1) {          // close enough: a perfect drop, nothing is lost
    left = stTopX;
    right = stTopX + stTopW;
    stPerfectAt = now;
  } else if (right <= left) {           // missed the tower completely
    cutX = mx;
    cutW = stTopW;
    cutY = layerY(stLevel + 1);
    cutV = 0;
    cutActive = true;
    stackGameOver(now);
    return;
  } else {                              // the overhanging part breaks off and falls
    cutW = stTopW - (right - left);
    cutX = (mx < left) ? mx : right;
    cutY = layerY(stLevel + 1);
    cutV = 0;
    cutActive = true;
  }

  stLevel++;
  stTopX = left;
  stTopW = right - left;
  stXs[stLevel % 16] = stTopX;
  stWs[stLevel % 16] = stTopW;
  stDir = (stLevel % 2) ? -1 : 1;       // the next block comes from the other side
  stX = (stDir > 0) ? 0 : SCREEN_W - stTopW;
}

void stackInput(int ev, unsigned long now) {
  if (ev == BTN_LONG) { openMenu(now); return; }
  if (ev != BTN_DOWN) return;
  if (stState == GAME_PLAYING) {
    stackDrop(now);
  } else if (stState == GAME_READY || now - stStateAt > 700) {
    stState = GAME_PLAYING;
    stackReset(now);
  }
}

void stackUpdate(unsigned long dt, unsigned long now) {
  if (cutActive) {
    cutV += 0.0005f * dt;
    cutY += cutV * dt;
    if (cutY > SCREEN_H) cutActive = false;
  }
  if (stState != GAME_PLAYING) return;
  float sp = min(0.05f + stLevel * 0.004f, 0.14f);   // slides faster as the tower grows
  stX += stDir * sp * dt;
  if (stX <= 0) { stX = 0; stDir = 1; }
  if (stX >= SCREEN_W - stTopW) { stX = SCREEN_W - stTopW; stDir = -1; }
}

void stackDraw(unsigned long now) {
  display.clearDisplay();
  for (int lv = max(0, stLevel - 15); lv <= stLevel; lv++) {
    int y = layerY(lv);
    if (y < SCREEN_H) display.fillRect(stXs[lv % 16], y, stWs[lv % 16], STACK_H - 1, WHITE);
  }
  if (stState == GAME_PLAYING) {        // the sliding block, with a tiny face
    int x = (int)lroundf(stX), y = layerY(stLevel + 1);
    display.fillRect(x, y, stTopW, STACK_H - 1, WHITE);
    if (stTopW >= 8) {
      display.drawPixel(x + stTopW / 2 - 2, y + 1, BLACK);
      display.drawPixel(x + stTopW / 2 + 2, y + 1, BLACK);
    }
  }
  if (cutActive) display.drawRect((int)cutX, (int)cutY, cutW, STACK_H - 1, WHITE);

  drawScores(stBest, stLevel, stState != GAME_READY);
  if (now - stPerfectAt < 600) drawCentered("perfect!", 12, 1);
  if (stState == GAME_READY) {
    drawCentered("STACK", 14, 1);
    drawCentered("tap to drop", 26, 1);
  }
  if (stState == GAME_OVER) drawGameOver(stNewBest, now);
  display.display();
}


// ---------------------------------------------------------------
//  Quotes - tap for the next one
// ---------------------------------------------------------------

// Splits text into lines of at most `width` letters, breaking at spaces.
// The lines go into qLines; returns how many there are.
int wrapText(const char* text, int width) {
  int n = 0;
  const char* p = text;
  while (*p && n < 6) {
    while (*p == ' ') p++;
    if (!*p) break;
    int take = strlen(p);
    if (take > width) {
      take = width;
      while (take > 0 && p[take] != ' ') take--;   // back up to the last space
      if (take == 0) take = width;                 // one very long word: just cut it
    }
    memcpy(qLines[n], p, take);
    qLines[n][take] = 0;
    n++;
    p += take;
  }
  return n;
}

void nextQuote() {
  do {
    quoteIndex = (quoteIndex + 1) % QUOTE_COUNT;
  } while (QUOTES[quoteIndex].text[0] == 0);   // skip empty ones
}

void startQuotes(unsigned long now) {
  mode = MODE_QUOTES;
  if (QUOTES[quoteIndex].text[0] == 0) nextQuote();
  lastFrameAt = 0;
}

void quotesInput(int ev, unsigned long now) {
  if (ev == BTN_LONG) openMenu(now);
  else if (ev == BTN_TAP) nextQuote();
}

void quotesDraw(unsigned long now) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print('"');
  int sz = lroundf(1.5f + 1.5f * wave(now, 1500, 0));
  if (sz > 0) drawSparkle(121, 5, sz, WHITE);

  int n = wrapText(QUOTES[quoteIndex].text, 20);
  int y = 8 + (40 - n * 10) / 2;              // centre the lines in the space above the author
  for (int i = 0; i < n; i++) drawCentered(qLines[i], y + i * 10, 1);

  char by[32];
  snprintf(by, sizeof by, "- %s", QUOTES[quoteIndex].author);
  display.setTextSize(1);
  display.setCursor(SCREEN_W - (int)strlen(by) * 6 - 2, 55);
  display.print(by);
  display.display();
}


// ---------------------------------------------------------------
//  Stopwatch - tap = start/pause, hold while paused = reset
// ---------------------------------------------------------------

unsigned long swTime(unsigned long now) {
  return swElapsed + (swRunning ? now - swStartAt : 0);
}

void startStopwatch(unsigned long now) {
  mode = MODE_STOPWATCH;
  lastFrameAt = 0;
}

void stopwatchInput(int ev, unsigned long now) {
  if (ev == BTN_TAP) {
    if (swRunning) {
      swElapsed += now - swStartAt;
      swRunning = false;
    } else {
      swStartAt = now;
      swRunning = true;
    }
  } else if (ev == BTN_LONG) {
    if (!swRunning && swElapsed > 0) swElapsed = 0;   // paused: hold to reset
    else openMenu(now);                               // a running stopwatch keeps going in the background
  }
}

void stopwatchDraw(unsigned long now) {
  unsigned long ms = swTime(now);
  unsigned long h = ms / 3600000UL, m = (ms / 60000UL) % 60, s = (ms / 1000UL) % 60, tenth = (ms / 100UL) % 10;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print("stopwatch");
  drawBlob(118, 7 - ((swRunning && (now / 150) % 2) ? 1 : 0), swRunning ? 1 : 0);

  bool show = swRunning || ms == 0 || (now % 1000) < 700;   // paused: the time blinks
  char buf[12];
  if (show) {
    if (h > 0) {
      snprintf(buf, sizeof buf, "%lu:%02lu:%02lu", h, m, s);
      drawCentered(buf, 22, 2);
    } else {
      snprintf(buf, sizeof buf, "%02lu:%02lu", m, s);
      display.setTextSize(3);
      display.setCursor(7, 18);
      display.print(buf);
      snprintf(buf, sizeof buf, ".%lu", tenth);
      display.setTextSize(2);
      display.setCursor(97, 26);
      display.print(buf);
    }
  }
  const char* hint = swRunning ? "tap:pause hold:menu" : (ms > 0 ? "tap:go hold:reset" : "tap:start hold:menu");
  drawCentered(hint, 56, 1);
  display.display();
}


// ---------------------------------------------------------------
//  Button, display detection, sleep
// ---------------------------------------------------------------

// Reports what the button just did:
//   BTN_DOWN - the moment it's pressed (games use this, so jumps feel instant)
//   BTN_TAP  - released before LONG_PRESS_MS (faces and menu use this)
//   BTN_LONG - held for LONG_PRESS_MS (opens the menu / chooses)
// The reading has to stay the same for DEBOUNCE_MS before we believe it,
// so the metal contacts "bouncing" is ignored.
int readButton(unsigned long now) {
  int reading = digitalRead(BUTTON_PIN);
  if (reading != buttonReading) {     // something changed (could be bounce) - restart the timer
    buttonReading = reading;
    buttonFlipAt = now;
  }
  if (now - buttonFlipAt >= DEBOUNCE_MS && reading != buttonStable) {
    buttonStable = reading;
    if (buttonStable == LOW) {        // LOW = pressed (because of INPUT_PULLUP)
      pressStartAt = now;
      longFired = false;
      return BTN_DOWN;
    }
    return longFired ? BTN_NONE : BTN_TAP;   // released
  }
  if (buttonStable == LOW && !longFired && now - pressStartAt >= LONG_PRESS_MS) {
    longFired = true;
    return BTN_LONG;
  }
  return BTN_NONE;
}

// Looks for the OLED at its two possible addresses. Returns 0 if not found.
uint8_t findOled() {
  const uint8_t candidates[] = { 0x3C, 0x3D };
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      LOG("OLED found at 0x%02X\n", a);
      return a;
    }
  }
  return 0;
}

void goToSleep() {
  LOG("Napping... press the button to wake up\n");
  setPositions(0, 0);
  display.clearDisplay();
  faceSleepy(millis());
  drawLabel("nap time");
  display.display();
  delay(1500);                                   // the only long pause, used once before sleeping
#if USE_SH1106
  display.oled_command(SH110X_DISPLAYOFF);       // OLED uses only a few microamps when off
#else
  display.ssd1306_command(SSD1306_DISPLAYOFF);
#endif

  esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  gpio_pullup_en((gpio_num_t)BUTTON_PIN);        // keep the pull-up on while sleeping
  gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
  esp_deep_sleep_start();                        // waking up restarts the sketch from setup()
}


// ---------------------------------------------------------------
//  setup() runs once at power on, loop() runs over and over
// ---------------------------------------------------------------

void setup() {
  setCpuFrequencyMhz(80);   // half speed is plenty for faces and saves battery
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Start with the real button state, so the press that wakes it up doesn't also skip a face
  buttonReading = buttonStable = digitalRead(BUTTON_PIN);
  longFired = (buttonStable == LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);    // fast I2C = less time spent sending each frame
  uint8_t addr = findOled();
  while (addr == 0) {   // keep looking, so it starts by itself once the wiring is fixed
    LOG("OLED not found at 0x3C or 0x3D - check the wiring!\n");
    delay(1000);
    addr = findOled();
  }
#if USE_SH1106
  display.begin(addr, true);
  display.setContrast(OLED_CONTRAST);
#else
  display.begin(SSD1306_SWITCHCAPVCC, addr, true, false);   // false = Wire is already started
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(OLED_CONTRAST);
#endif

  prefs.begin("keychain", false);
  runBest = prefs.getUShort("runBest", 0);
  rxBest = prefs.getUShort("rxBest", 0);
  flBest = prefs.getUShort("flBest", 0);
  stBest = prefs.getUShort("stBest", 0);
#if SAVE_LAST_FACE
  currentFace = prefs.getUChar("face", HAPPY);
  if (currentFace >= FACE_COUNT) currentFace = HAPPY;
#endif

  unsigned long now = millis();
  faceChangedAt = now;
  lastActivityAt = now;
  nextBlinkAt = now + 2000;
  LOG("Kawaii keychain ready! Face %d: %s\n", currentFace, FACE_NAMES[currentFace]);
}

void loop() {
  unsigned long now = millis();
  int ev = readButton(now);
  if (ev != BTN_NONE) lastActivityAt = now;

  switch (mode) {
    case MODE_FACES:
      if (ev == BTN_TAP) {
        currentFace = (currentFace + 1) % FACE_COUNT;   // after the last face, back to the first
        faceChangedAt = now;
        blinking = false;
        nextBlinkAt = now + 1200;                       // don't blink right as the new face pops in
#if SAVE_LAST_FACE
        prefs.putUChar("face", currentFace);
#endif
        LOG("Face %d: %s\n", currentFace, FACE_NAMES[currentFace]);
      } else if (ev == BTN_LONG) {
        menuIndex = 0;
        openMenu(now);
      }
      break;
    case MODE_MENU:     menuInput(ev, now);     break;
    case MODE_RUNNER:   runnerInput(ev, now);   break;
    case MODE_FLOAT:     floatInput(ev, now);     break;
    case MODE_STACK:     stackInput(ev, now);     break;
    case MODE_REACTION:  reactionInput(ev, now);  break;
    case MODE_QUOTES:    quotesInput(ev, now);    break;
    case MODE_STOPWATCH: stopwatchInput(ev, now); break;
  }

  bool fastGame = (mode == MODE_RUNNER || mode == MODE_FLOAT || mode == MODE_STACK);
  unsigned long frameMs = fastGame ? GAME_FRAME_MS : FRAME_MS;
  if (now - lastFrameAt >= frameMs) {               // time for a new frame?
    unsigned long dt = min(now - lastFrameAt, 60UL);  // cap it, so a slow frame can't teleport things
    lastFrameAt = now;
    switch (mode) {
      case MODE_FACES:    drawFrame(now);  break;
      case MODE_MENU:     menuDraw();      break;
      case MODE_RUNNER:   runnerUpdate(dt, now); runnerDraw(now); break;
      case MODE_FLOAT:     floatUpdate(dt, now); floatDraw(now); break;
      case MODE_STACK:     stackUpdate(dt, now); stackDraw(now); break;
      case MODE_REACTION:  reactionDraw(now);  break;
      case MODE_QUOTES:    quotesDraw(now);    break;
      case MODE_STOPWATCH: stopwatchDraw(now); break;
    }
  }

#if AUTO_SLEEP_MINUTES > 0
  // never nap while the stopwatch is running - deep sleep would lose the time
  if (!swRunning && now - lastActivityAt >= AUTO_SLEEP_MINUTES * 60000UL) goToSleep();
#endif

  delay(1);   // tiny rest so the CPU isn't spinning flat out
}
