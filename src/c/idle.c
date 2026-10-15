// Idle scene: a tumbling icosahedron shaded only with 4x4 ordered (Bayer) dithering, lit from
// wherever the wrist is tilted. Every pixel is written straight to the frame buffer, so the same
// Bayer matrix also drives the dissolve between this scene and the stats underneath.
#include <pebble.h>

#define FRAME_MS 100  // ~10 fps; lower it if the animation shows up in watch battery life
#define RAMP_STEPS 6  // 7 colors per ramp, 16 dither levels between neighbours
#define MAX_SHADE (RAMP_STEPS * 16 - 1)

static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static const uint8_t RAMP_BLUE[] = { GColorBlackARGB8, GColorOxfordBlueARGB8, GColorCobaltBlueARGB8,
                                     GColorVividCeruleanARGB8, GColorPictonBlueARGB8, GColorCelesteARGB8,
                                     GColorWhiteARGB8 };
static const uint8_t RAMP_GREEN[] = { GColorBlackARGB8, GColorDarkGreenARGB8, GColorIslamicGreenARGB8,
                                      GColorMalachiteARGB8, GColorScreaminGreenARGB8, GColorMintGreenARGB8,
                                      GColorWhiteARGB8 };
static const uint8_t RAMP_RED[] = { GColorBlackARGB8, GColorBulgarianRoseARGB8, GColorDarkCandyAppleRedARGB8,
                                    GColorRedARGB8, GColorSunsetOrangeARGB8, GColorMelonARGB8, GColorWhiteARGB8 };

#define PHI 1.618034f
static const float VERTS[12][3] = {
  { -1, PHI, 0 }, { 1, PHI, 0 }, { -1, -PHI, 0 }, { 1, -PHI, 0 }, { 0, -1, PHI }, { 0, 1, PHI },
  { 0, -1, -PHI }, { 0, 1, -PHI }, { PHI, 0, -1 }, { PHI, 0, 1 }, { -PHI, 0, -1 }, { -PHI, 0, 1 },
};
static const uint8_t FACES[20][3] = {
  { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
  { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
  { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 },
};
#define CAM_DIST 3.6f
#define CAGE 1.1f  // half-size of the wire cube around the solid

static const int8_t CUBE_EDGES[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 },
                                          { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };

static Layer *s_layer;
static AppTimer *s_timer;
static const uint8_t *s_ramp = RAMP_BLUE;
static int32_t s_angle, s_speed = 150;
static int s_reveal, s_target;  // 0 = scene fully drawn, 16 = stats fully showing through
static float s_lx = -0.5f, s_ly = -0.6f;

static float my_sqrtf(float x) {  // Newton's method; inputs here are small and positive
  if (x <= 0) return 0;
  float r = x > 1 ? x : 1;
  for (int i = 0; i < 8; i++) r = 0.5f * (r + x / r);
  return r;
}

static float fsin(int32_t a) { return (float)sin_lookup(a) / TRIG_MAX_RATIO; }
static float fcos(int32_t a) { return (float)cos_lookup(a) / TRIG_MAX_RATIO; }

// ---- drawing -----------------------------------------------------------------

// Pixels whose Bayer rank is below s_reveal are left alone, so the layer below shows through
#define SHOWN(x, y) (BAYER[(y) & 3][(x) & 3] >= s_reveal)

static inline uint8_t shade_px(int shade, int x, int y) {
  return BAYER[y & 3][x & 3] < (shade & 15) ? s_ramp[(shade >> 4) + 1] : s_ramp[shade >> 4];
}

static void fill_tri(GBitmap *fb, int h, GPoint a, GPoint b, GPoint c, int shade) {
  GPoint t;
  if (b.y < a.y) { t = a; a = b; b = t; }
  if (c.y < a.y) { t = a; a = c; c = t; }
  if (c.y < b.y) { t = b; b = c; c = t; }
  if (c.y == a.y) return;
  for (int y = a.y < 0 ? 0 : a.y; y <= c.y && y < h; y++) {
    int xa = a.x + (c.x - a.x) * (y - a.y) / (c.y - a.y);
    int xb = y < b.y ? a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y)
           : c.y == b.y ? b.x : b.x + (c.x - b.x) * (y - b.y) / (c.y - b.y);
    if (xa > xb) { int s = xa; xa = xb; xb = s; }
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    if (xa < row.min_x) xa = row.min_x;
    if (xb > row.max_x) xb = row.max_x;
    for (int x = xa; x <= xb; x++) {
      if (SHOWN(x, y)) row.data[x] = shade_px(shade, x, y);
    }
  }
}

// Dotted line (every other pixel), so the cage reads as lighter than the solid
static void dotted_line(GBitmap *fb, int w, int h, GPoint a, GPoint b, uint8_t color) {
  int dx = abs(b.x - a.x), dy = -abs(b.y - a.y), sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (a.x >= 0 && a.x < w && a.y >= 0 && a.y < h && ((a.x + a.y) & 1) && SHOWN(a.x, a.y)) {
      gbitmap_get_data_row_info(fb, a.y).data[a.x] = color;
    }
    if (a.x == b.x && a.y == b.y) return;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; a.x += sx; }
    if (e2 <= dx) { err += dx; a.y += sy; }
  }
}

// Rotate by (yaw, pitch) and project; returns view-space z
static float project(float x, float y, float z, int32_t yaw, int32_t pitch, int cx, int cy, float focal,
                     GPoint *out, float *vx, float *vy) {
  float sy = fsin(yaw), cyw = fcos(yaw), sx = fsin(pitch), cxw = fcos(pitch);
  float x1 = x * cyw + z * sy, z1 = -x * sy + z * cyw;
  float y2 = y * cxw - z1 * sx, z2 = y * sx + z1 * cxw;
  float k = focal / (z2 + CAM_DIST);
  *out = GPoint(cx + (int)(x1 * k), cy + (int)(y2 * k));
  if (vx) { *vx = x1; *vy = y2; }
  return z2;
}

static void cage(GBitmap *fb, int w, int h, int cx, int cy, float focal, bool front) {
  GPoint p[8];
  float z[8];
  for (int i = 0; i < 8; i++) {
    z[i] = project(i & 1 ? CAGE : -CAGE, i & 2 ? CAGE : -CAGE, i & 4 ? CAGE : -CAGE,
                   -s_angle / 2, s_angle / 3, cx, cy, focal, &p[i], NULL, NULL);
  }
  for (int e = 0; e < 12; e++) {
    int a = CUBE_EDGES[e][0], b = CUBE_EDGES[e][1];
    if ((z[a] + z[b] < 0) == front) {
      dotted_line(fb, w, h, p[a], p[b], front ? s_ramp[5] : s_ramp[2]);
    }
  }
}

static void update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int w = bounds.size.w, h = bounds.size.h;
  int bob = fsin(s_angle * 2) * 6;
  int cx = w / 2, cy = h / 2 - 8 + bob, floor_y = h - 22;
  float focal = w * 0.9f;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  // Background: black fading to the ramp's darkest tint, plus a dithered shadow that tightens as
  // the solid rises
  int rx = 60 - bob * 2, ry = 8;
  for (int y = 0; y < h; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int g = y * y * 12 / (h * h), dy = y - floor_y;  // black at the top, faint tint at the bottom
    for (int x = row.min_x; x <= row.max_x; x++) {
      if (!SHOWN(x, y)) continue;
      int dx = x - cx;
      bool shadow = dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry;
      row.data[x] = shadow ? s_ramp[0] : shade_px(g, x, y);
    }
  }

  cage(fb, w, h, cx, cy, focal, false);  // back edges, behind the solid

  float v[12][3];
  GPoint p[12];
  for (int i = 0; i < 12; i++) {
    v[i][2] = project(VERTS[i][0] / 1.902f, VERTS[i][1] / 1.902f, VERTS[i][2] / 1.902f,
                      s_angle, s_angle * 7 / 10, cx, cy, focal, &p[i], &v[i][0], &v[i][1]);
  }

  float llen = 1.0f / my_sqrtf(s_lx * s_lx + s_ly * s_ly + 1.0f);
  for (int f = 0; f < 20; f++) {
    const uint8_t *fc = FACES[f];
    // Regular solid centred on the origin: the face centroid is along its outward normal
    float nx = v[fc[0]][0] + v[fc[1]][0] + v[fc[2]][0];
    float ny = v[fc[0]][1] + v[fc[1]][1] + v[fc[2]][1];
    float nz = v[fc[0]][2] + v[fc[1]][2] + v[fc[2]][2];
    // visible when the normal points at the camera at (0,0,-CAM_DIST)
    if (nx * (nx / 3) + ny * (ny / 3) + nz * (nz / 3 + CAM_DIST) >= 0) continue;
    float nlen = my_sqrtf(nx * nx + ny * ny + nz * nz);
    float lambert = (nx * s_lx + ny * s_ly - nz) / nlen * llen;
    int shade = 14 + (int)((lambert > 0 ? lambert : 0) * (MAX_SHADE - 14));
    fill_tri(fb, h, p[fc[0]], p[fc[1]], p[fc[2]], shade > MAX_SHADE ? MAX_SHADE : shade);
  }
  cage(fb, w, h, cx, cy, focal, true);
  graphics_release_frame_buffer(ctx, fb);
}

static void tick(void *ctx) {
  s_timer = NULL;
  s_angle = (s_angle + s_speed) % (TRIG_MAX_ANGLE * 10);  // wraps cleanly for both rotation axes
  if (s_reveal != s_target) s_reveal += s_reveal < s_target ? 2 : -2;

  // Light follows the wrist; low-pass so it glides
  AccelData a;
  if (accel_service_peek(&a) == 0) {
    s_lx += (-0.5f - a.x / 700.0f - s_lx) * 0.25f;
    s_ly += (-0.6f + a.y / 700.0f - s_ly) * 0.25f;
  }

  layer_set_hidden(s_layer, s_reveal >= 16);
  layer_mark_dirty(s_layer);
  if (s_reveal < 16 || s_target < 16) s_timer = app_timer_register(FRAME_MS, tick, NULL);
}

Layer *idle_create(GRect frame) {
  s_layer = layer_create(frame);
  layer_set_update_proc(s_layer, update);
  s_timer = app_timer_register(FRAME_MS, tick, NULL);
  return s_layer;
}

void idle_show(bool show) {
  s_target = show ? 0 : 16;
  if (!s_timer) s_timer = app_timer_register(FRAME_MS, tick, NULL);
}

void idle_set_mood(int charging, int battery, int cpu) {
  s_ramp = charging ? RAMP_GREEN : battery <= 15 ? RAMP_RED : RAMP_BLUE;
  s_speed = 150 + cpu * 8;  // busier Frame, faster spin
}
