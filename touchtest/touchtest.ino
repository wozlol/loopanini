// Touch test: shows raw touch coordinates, the max y ever seen, and a
// 5 screen navigation using the left and right halves of a bottom band.
// Board: M5CoreS3, USB CDC On Boot Enabled is fine for this test.
#include <M5Unified.h>

static const char *names[5] = {"1 MIXER", "2 AMY", "3 LOOPER", "4 STUTTER", "5 CONFIG"};
static int screen = 2;
static int maxX = 0, maxY = 0;
static int bandTop = 216;  // start of the nav band, edit after seeing the readout

static void draw() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(2);
  d.setCursor(8, 8);
  d.print(names[screen]);
  d.setTextSize(1);
  d.setCursor(8, 40);
  d.printf("display %dx%d  band starts y=%d", d.width(), d.height(), bandTop);
  d.setCursor(8, 56);
  d.printf("max touch x=%d y=%d", maxX, maxY);
  d.fillRect(0, bandTop, d.width() / 2, d.height() - bandTop, 0x0320);
  d.fillRect(d.width() / 2, bandTop, d.width() / 2, d.height() - bandTop, 0x0019);
  d.setCursor(8, bandTop + 4);
  d.print("< prev");
  d.setCursor(d.width() - 50, bandTop + 4);
  d.print("next >");
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  draw();
}

void loop() {
  M5.update();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (t.x > maxX) maxX = t.x;
    if (t.y > maxY) maxY = t.y;
    Serial.printf("touch x=%d y=%d (max x=%d y=%d)\n", t.x, t.y, maxX, maxY);
    if (t.y >= bandTop) {
      screen = (t.x < M5.Display.width() / 2) ? (screen + 4) % 5 : (screen + 1) % 5;
    }
    draw();
    M5.Display.fillCircle(t.x, t.y, 4, TFT_YELLOW);
  }
  delay(10);
}
