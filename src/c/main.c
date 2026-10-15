#include <pebble.h>

#define MAX_GAMES 20

// Latest stats from the Frame (pushed by pkjs every few seconds)
static struct {
  int battery, charging, watts10, mins, bat_temp, health, volts100;
  int cpu, cpu_temp, mem, volume, muted;
  bool have;
  char game[64], status[48];
} s;

static Window *s_main, *s_menu_win, *s_bat_win, *s_games_win;

// idle.c
Layer *idle_create(GRect frame);
void idle_show(bool show);
void idle_set_mood(int charging, int battery, int cpu);

#define STATS_MS 60000  // how long stats stay up after opening the app, a flick or a button press
static bool s_idle = true;
static AppTimer *s_idle_timer;
static Layer *s_stats, *s_bat_layer;
static SimpleMenuLayer *s_menu, *s_games;

static char s_names_buf[MAX_GAMES * 32];
static SimpleMenuItem s_game_items[MAX_GAMES];
static SimpleMenuSection s_games_section;

// ---- Steam look -------------------------------------------------------------
// Steam's palette on Pebble's 64 colors. Its in-between slates (#1b2838 body, #2a475e panels) aren't
// in the palette, so they're 50% checkerboard dithers of the two nearest colors.
#define STEAM_TEXT GColorWhite
#define STEAM_SUBTEXT GColorLightGray      // #c7d5e0 / #8f98a0
#define STEAM_ACCENT GColorPictonBlue      // #66c0f4
#define STEAM_FOCUS GColorVividCerulean    // #1a9fff, focus + progress
#define STEAM_INGAME GColorInchworm        // friends list "in-game" green
#define STEAM_PLAY GColorKellyGreen        // Play button green
#define STEAM_ERROR GColorSunsetOrange     // #c15755-ish
#define HEADER_H 40                        // status bar (16) + title row

static void dither_rect(GContext *ctx, GRect r, GColor a, GColor b) {
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  for (int y = r.origin.y; y < r.origin.y + r.size.h; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int x0 = r.origin.x > row.min_x ? r.origin.x : row.min_x;
    int x1 = r.origin.x + r.size.w - 1 < row.max_x ? r.origin.x + r.size.w - 1 : row.max_x;
    for (int x = x0; x <= x1; x++) row.data[x] = ((x + y) & 1) ? a.argb : b.argb;
  }
  graphics_release_frame_buffer(ctx, fb);
}

static void steam_body(GContext *ctx, GRect r) { dither_rect(ctx, r, GColorBlack, GColorOxfordBlue); }
static void steam_panel(GContext *ctx, GRect r) { dither_rect(ctx, r, GColorDarkGray, GColorMidnightGreen); }

static void draw_text(GContext *ctx, const char *txt, const char *font, GRect r, GColor c, GTextAlignment a) {
  graphics_context_set_text_color(ctx, c);
  graphics_draw_text(ctx, txt, fonts_get_system_font(font), r, GTextOverflowModeTrailingEllipsis, a, NULL);
}

// Title row under the status bar with Steam's thin blue underline; returns where content starts
static int draw_header(GContext *ctx, GRect b, const char *title) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 16, b.size.w, HEADER_H - 16), 0, GCornerNone);
  draw_text(ctx, title, FONT_KEY_GOTHIC_18_BOLD, GRect(8, 15, b.size.w - 16, 22), STEAM_TEXT, GTextAlignmentLeft);

  bool online = s.have && !s.status[0];
  draw_text(ctx, online ? "ONLINE" : "OFFLINE", FONT_KEY_GOTHIC_14, GRect(0, 19, b.size.w - 18, 16),
            online ? STEAM_ACCENT : STEAM_SUBTEXT, GTextAlignmentRight);
  graphics_context_set_fill_color(ctx, online ? STEAM_ACCENT : GColorDarkGray);
  graphics_fill_circle(ctx, GPoint(b.size.w - 11, 28), 3);

  graphics_context_set_fill_color(ctx, STEAM_FOCUS);
  graphics_fill_rect(ctx, GRect(0, HEADER_H, b.size.w, 1), 0, GCornerNone);
  steam_body(ctx, GRect(0, HEADER_H + 1, b.size.w, b.size.h - HEADER_H - 1));
  return HEADER_H + 1;
}

static StatusBarLayer *steam_status_bar(Layer *root) {
  StatusBarLayer *bar = status_bar_layer_create();
  status_bar_layer_set_colors(bar, GColorBlack, STEAM_SUBTEXT);
  layer_add_child(root, status_bar_layer_get_layer(bar));
  return bar;
}

static void steam_menu_colors(SimpleMenuLayer *m) {
  MenuLayer *ml = simple_menu_layer_get_menu_layer(m);
  menu_layer_set_normal_colors(ml, GColorBlack, STEAM_TEXT);
  menu_layer_set_highlight_colors(ml, STEAM_FOCUS, STEAM_TEXT);
}

// ---- helpers ----------------------------------------------------------------

static void send_action(const char *action) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_cstring(it, MESSAGE_KEY_Action, action);
  app_message_outbox_send();
}

static void fmt_mins(char *buf, size_t n, int mins) {
  if (!mins) snprintf(buf, n, "—");
  else if (mins >= 60) snprintf(buf, n, "%dh%02dm", mins / 60, mins % 60);
  else snprintf(buf, n, "%dm", mins);
}

static GColor battery_color(void) {
  if (s.charging) return STEAM_PLAY;
  if (s.battery <= 15) return STEAM_ERROR;
  return STEAM_FOCUS;
}

static void render(void) {
  if (s_stats) layer_mark_dirty(s_stats);
  if (s_bat_layer) layer_mark_dirty(s_bat_layer);
}

// ---- stats screen -----------------------------------------------------------

static void tile(GContext *ctx, GRect r, const char *label, const char *value, GColor value_color) {
  steam_panel(ctx, r);
  draw_text(ctx, label, FONT_KEY_GOTHIC_14, GRect(r.origin.x + 5, r.origin.y + 1, r.size.w - 6, 16),
            STEAM_SUBTEXT, GTextAlignmentLeft);
  draw_text(ctx, value, FONT_KEY_GOTHIC_24_BOLD, GRect(r.origin.x + 5, r.origin.y + 14, r.size.w - 6, 28),
            value_color, GTextAlignmentLeft);
}

static void stats_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w, y = draw_header(ctx, b, "STEAM FRAME");
  char buf[32], buf2[16];

  // Battery: big number, power + time on the right, Steam download-style bar underneath
  if (s.have) snprintf(buf, sizeof(buf), "%d%%", s.battery);
  draw_text(ctx, s.have ? buf : "--%", FONT_KEY_BITHAM_42_BOLD, GRect(6, y - 2, 110, 50), STEAM_TEXT,
            GTextAlignmentLeft);
  if (s.have) {
    snprintf(buf, sizeof(buf), "%s%d.%d W", s.charging ? "+" : "−", s.watts10 / 10, s.watts10 % 10);
    draw_text(ctx, buf, FONT_KEY_GOTHIC_24_BOLD, GRect(110, y + 4, w - 118, 28), STEAM_TEXT, GTextAlignmentRight);
    fmt_mins(buf2, sizeof(buf2), s.mins);
    snprintf(buf, sizeof(buf), "%s %s", buf2, s.mins ? (s.charging ? "to full" : "left") : "");
    draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(100, y + 30, w - 108, 18), STEAM_SUBTEXT, GTextAlignmentRight);
  }
  GRect bar = GRect(8, y + 52, w - 16, 7);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bar, 0, GCornerNone);
  int fill = s.have ? bar.size.w * s.battery / 100 : 0;
  graphics_context_set_fill_color(ctx, battery_color());
  graphics_fill_rect(ctx, GRect(bar.origin.x, bar.origin.y, fill, bar.size.h), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, s.charging ? GColorInchworm : STEAM_ACCENT);  // Steam's top gloss
  graphics_fill_rect(ctx, GRect(bar.origin.x, bar.origin.y, fill, 2), 0, GCornerNone);

  // Three stat tiles
  int ty = y + 66, tw = (w - 32) / 3;
  if (s.have) snprintf(buf, sizeof(buf), "CPU %d°", s.cpu_temp);
  if (s.have) snprintf(buf2, sizeof(buf2), "%d%%", s.cpu);
  tile(ctx, GRect(8, ty, tw, 44), s.have ? buf : "CPU", s.have ? buf2 : "--", STEAM_TEXT);
  if (s.have) snprintf(buf2, sizeof(buf2), "%d%%", s.mem);
  tile(ctx, GRect(16 + tw, ty, tw, 44), "RAM", s.have ? buf2 : "--", STEAM_TEXT);
  if (s.have) snprintf(buf2, sizeof(buf2), "%d%%", s.volume);
  tile(ctx, GRect(24 + 2 * tw, ty, tw, 44), "VOLUME", s.muted ? "MUTE" : s.have ? buf2 : "--",
       s.muted ? STEAM_ERROR : STEAM_TEXT);

  // Now playing card, like the friends list: green bar + "NOW PLAYING" when in game
  GRect card = GRect(8, ty + 52, w - 16, b.size.h - ty - 58);
  steam_panel(ctx, card);
  bool playing = s.game[0] && !s.status[0];
  graphics_context_set_fill_color(ctx, s.status[0] ? STEAM_ERROR : playing ? STEAM_PLAY : GColorDarkGray);
  graphics_fill_rect(ctx, GRect(card.origin.x, card.origin.y, 3, card.size.h), 0, GCornerNone);
  GRect label = GRect(card.origin.x + 9, card.origin.y + 1, card.size.w - 12, 16);
  GRect body = GRect(card.origin.x + 9, card.origin.y + 15, card.size.w - 12, card.size.h - 16);
  if (s.status[0]) {
    draw_text(ctx, "STATUS", FONT_KEY_GOTHIC_14_BOLD, label, STEAM_ERROR, GTextAlignmentLeft);
    draw_text(ctx, s.status, FONT_KEY_GOTHIC_18, body, STEAM_TEXT, GTextAlignmentLeft);
  } else {
    draw_text(ctx, playing ? "NOW PLAYING" : "NOT IN GAME", FONT_KEY_GOTHIC_14_BOLD, label,
              playing ? STEAM_INGAME : STEAM_SUBTEXT, GTextAlignmentLeft);
    draw_text(ctx, playing ? s.game : s.have ? "Steam Frame idle" : "Connecting…", FONT_KEY_GOTHIC_24_BOLD,
              body, playing ? STEAM_TEXT : STEAM_SUBTEXT, GTextAlignmentLeft);
  }
}

// ---- games window -----------------------------------------------------------

static void game_selected(int index, void *ctx) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_int32(it, MESSAGE_KEY_Launch, index);
  app_message_outbox_send();
  vibes_short_pulse();
  window_stack_pop_all(false);
  window_stack_push(s_main, true);
}

static void games_load(Window *w) {
  // Split "name\nname\n..." in place into menu items
  int n = 0;
  char *p = s_names_buf;
  while (n < MAX_GAMES && *p) {
    s_game_items[n++] = (SimpleMenuItem){ .title = p, .callback = game_selected };
    char *nl = strchr(p, '\n');
    if (!nl) break;
    *nl = '\0';
    p = nl + 1;
  }
  s_games_section = (SimpleMenuSection){ .title = "LIBRARY", .items = s_game_items, .num_items = n };
  Layer *root = window_get_root_layer(w);
  s_games = simple_menu_layer_create(layer_get_bounds(root), w, &s_games_section, 1, NULL);
  steam_menu_colors(s_games);
  layer_add_child(root, simple_menu_layer_get_layer(s_games));
}

static void games_unload(Window *w) {
  simple_menu_layer_destroy(s_games);
}

// ---- battery window ---------------------------------------------------------

static StatusBarLayer *s_bat_bar;

static void bat_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int y = draw_header(ctx, b, "BATTERY") + 4;
  char v[7][20];
  snprintf(v[0], 20, "%d%%", s.battery);
  snprintf(v[1], 20, "%s", s.charging ? "Charging" : "On battery");
  snprintf(v[2], 20, "%d.%d W", s.watts10 / 10, s.watts10 % 10);
  fmt_mins(v[3], 20, s.mins);
  snprintf(v[4], 20, "%d.%02d V", s.volts100 / 100, s.volts100 % 100);
  snprintf(v[5], 20, "%d°C", s.bat_temp);
  snprintf(v[6], 20, "%d%%", s.health);
  const char *labels[7] = { "Charge", "State", "Power", s.charging ? "Time to full" : "Time left",
                            "Voltage", "Temperature", "Health" };
  for (int i = 0; i < 7; i++, y += 26) {
    if (i) {
      graphics_context_set_fill_color(ctx, GColorDarkGray);
      graphics_fill_rect(ctx, GRect(10, y - 2, b.size.w - 20, 1), 0, GCornerNone);
    }
    draw_text(ctx, labels[i], FONT_KEY_GOTHIC_18, GRect(10, y, 110, 22), STEAM_SUBTEXT, GTextAlignmentLeft);
    draw_text(ctx, s.have ? v[i] : "--", FONT_KEY_GOTHIC_18_BOLD, GRect(90, y, b.size.w - 100, 22),
              i == 0 ? battery_color() : STEAM_TEXT, GTextAlignmentRight);
  }
}

static void bat_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_bat_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_bat_layer, bat_update);
  layer_add_child(root, s_bat_layer);
  s_bat_bar = steam_status_bar(root);
}

static void bat_unload(Window *w) {
  status_bar_layer_destroy(s_bat_bar);
  layer_destroy(s_bat_layer);
  s_bat_layer = NULL;
}

// ---- menu window ------------------------------------------------------------

enum { M_MUTE, M_BATTERY, M_GAMES, M_QUIT, M_REFRESH, M_COUNT_QUICK };
enum { P_SLEEP, P_REBOOT, P_POWEROFF, P_COUNT };

static SimpleMenuItem s_quick_items[M_COUNT_QUICK], s_power_items[P_COUNT];
static SimpleMenuSection s_sections[2];
static const char *POWER_ACTIONS[P_COUNT] = { "sleep", "reboot", "poweroff" };
static int s_armed = -1;  // power item waiting for a confirming second press

static void quick_selected(int index, void *ctx) {
  switch (index) {
    case M_MUTE: send_action("mute"); window_stack_pop(true); break;
    case M_BATTERY: window_stack_push(s_bat_win, true); break;
    case M_GAMES:
      send_action("games");  // reply (GameNames) pushes the games window
      s_quick_items[M_GAMES].subtitle = "Loading…";
      layer_mark_dirty(simple_menu_layer_get_layer(s_menu));
      break;
    case M_QUIT: send_action("quit_game"); window_stack_pop(true); break;
    case M_REFRESH: send_action("refresh"); window_stack_pop(true); break;
  }
}

static void power_selected(int index, void *ctx) {
  if (s_armed != index) {
    if (s_armed >= 0) s_power_items[s_armed].subtitle = NULL;
    s_armed = index;
    s_power_items[index].subtitle = "Select again to confirm";
    layer_mark_dirty(simple_menu_layer_get_layer(s_menu));
    return;
  }
  send_action(POWER_ACTIONS[index]);
  vibes_double_pulse();
  window_stack_pop(true);
}

static void menu_load(Window *w) {
  s_armed = -1;
  s_quick_items[M_MUTE] = (SimpleMenuItem){ .title = s.muted ? "Unmute" : "Mute", .callback = quick_selected };
  s_quick_items[M_BATTERY] = (SimpleMenuItem){ .title = "Battery details", .callback = quick_selected };
  s_quick_items[M_GAMES] = (SimpleMenuItem){ .title = "Launch game…", .callback = quick_selected };
  s_quick_items[M_QUIT] = (SimpleMenuItem){ .title = "Quit game", .subtitle = s.game[0] ? s.game : NULL,
                                            .callback = quick_selected };
  s_quick_items[M_REFRESH] = (SimpleMenuItem){ .title = "Refresh", .callback = quick_selected };
  s_power_items[P_SLEEP] = (SimpleMenuItem){ .title = "Sleep", .callback = power_selected };
  s_power_items[P_REBOOT] = (SimpleMenuItem){ .title = "Restart", .callback = power_selected };
  s_power_items[P_POWEROFF] = (SimpleMenuItem){ .title = "Power off", .callback = power_selected };
  s_sections[0] = (SimpleMenuSection){ .title = "QUICK SETTINGS", .items = s_quick_items, .num_items = M_COUNT_QUICK };
  s_sections[1] = (SimpleMenuSection){ .title = "POWER", .items = s_power_items, .num_items = P_COUNT };

  Layer *root = window_get_root_layer(w);
  s_menu = simple_menu_layer_create(layer_get_bounds(root), w, s_sections, 2, NULL);
  steam_menu_colors(s_menu);
  layer_add_child(root, simple_menu_layer_get_layer(s_menu));
}

static void menu_unload(Window *w) {
  simple_menu_layer_destroy(s_menu);
}

// ---- main window ------------------------------------------------------------

static void go_idle(void *ctx) {
  s_idle_timer = NULL;
  s_idle = true;
  idle_show(true);
}

// Shows the stats; returns true if they were hidden, so the press only wakes
static bool wake(void) {
  bool was_idle = s_idle;
  s_idle = false;
  idle_show(false);
  if (!app_timer_reschedule(s_idle_timer, STATS_MS)) s_idle_timer = app_timer_register(STATS_MS, go_idle, NULL);
  return was_idle;
}

static void tap(AccelAxisType axis, int32_t direction) { wake(); }

static void up_click(ClickRecognizerRef r, void *ctx) { if (!wake()) send_action("vol_up"); }
static void down_click(ClickRecognizerRef r, void *ctx) { if (!wake()) send_action("vol_down"); }
static void select_click(ClickRecognizerRef r, void *ctx) { if (!wake()) window_stack_push(s_menu_win, true); }
static void select_long(ClickRecognizerRef r, void *ctx) { wake(); send_action("mute"); vibes_short_pulse(); }

static void click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 200, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 200, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

static void main_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_stats = layer_create(b);
  layer_set_update_proc(s_stats, stats_update);
  layer_add_child(root, s_stats);
  steam_status_bar(root);
  layer_add_child(root, idle_create(b));  // on top; dissolves away to reveal the stats
  wake();  // open on the stats; the sculpture takes over after STATS_MS
}

// ---- messaging --------------------------------------------------------------

static int get_int(DictionaryIterator *it, uint32_t key, int fallback) {
  Tuple *t = dict_find(it, key);
  return t ? (int)t->value->int32 : fallback;
}

static void inbox(DictionaryIterator *it, void *ctx) {
  Tuple *t;
  if ((t = dict_find(it, MESSAGE_KEY_GameNames))) {
    strncpy(s_names_buf, t->value->cstring, sizeof(s_names_buf) - 1);
    if (window_stack_get_top_window() == s_menu_win) window_stack_push(s_games_win, true);
    return;
  }
  if ((t = dict_find(it, MESSAGE_KEY_Status))) {
    strncpy(s.status, t->value->cstring, sizeof(s.status) - 1);
  }
  if ((t = dict_find(it, MESSAGE_KEY_Battery))) {
    s.have = true;
    s.battery = t->value->int32;
    s.charging = get_int(it, MESSAGE_KEY_Charging, 0);
    s.watts10 = get_int(it, MESSAGE_KEY_Watts, 0);
    s.mins = get_int(it, MESSAGE_KEY_Mins, 0);
    s.bat_temp = get_int(it, MESSAGE_KEY_BatTemp, 0);
    s.health = get_int(it, MESSAGE_KEY_Health, 0);
    s.volts100 = get_int(it, MESSAGE_KEY_Volts, 0);
    s.cpu = get_int(it, MESSAGE_KEY_Cpu, 0);
    s.cpu_temp = get_int(it, MESSAGE_KEY_CpuTemp, 0);
    s.mem = get_int(it, MESSAGE_KEY_Mem, 0);
    s.volume = get_int(it, MESSAGE_KEY_Volume, 0);
    s.muted = get_int(it, MESSAGE_KEY_Muted, 0);
    idle_set_mood(s.charging, s.battery, s.cpu);
    if ((t = dict_find(it, MESSAGE_KEY_Game))) strncpy(s.game, t->value->cstring, sizeof(s.game) - 1);
  }
  render();
}

static Window *make_window(WindowHandlers h, GColor bg) {
  Window *w = window_create();
  window_set_background_color(w, bg);
  window_set_window_handlers(w, h);
  return w;
}

int main(void) {
  s_main = make_window((WindowHandlers){ .load = main_load }, GColorBlack);
  window_set_click_config_provider(s_main, click_config);
  s_menu_win = make_window((WindowHandlers){ .load = menu_load, .unload = menu_unload }, GColorBlack);
  s_bat_win = make_window((WindowHandlers){ .load = bat_load, .unload = bat_unload }, GColorBlack);
  s_games_win = make_window((WindowHandlers){ .load = games_load, .unload = games_unload }, GColorBlack);

  app_message_register_inbox_received(inbox);
  app_message_open(1024, 64);
  accel_tap_service_subscribe(tap);
  window_stack_push(s_main, true);
  app_event_loop();
  // windows/layers are freed by the OS on app exit
}
