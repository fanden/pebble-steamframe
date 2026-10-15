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
static TextLayer *s_pct, *s_power, *s_details, *s_game, *s_bat_text;
static SimpleMenuLayer *s_menu, *s_games;

static char s_pct_buf[8], s_power_buf[48], s_details_buf[96], s_bat_buf[200];
static char s_names_buf[MAX_GAMES * 32];
static SimpleMenuItem s_game_items[MAX_GAMES];
static SimpleMenuSection s_games_section;

// ---- helpers ----------------------------------------------------------------

static void send_action(const char *action) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_cstring(it, MESSAGE_KEY_Action, action);
  app_message_outbox_send();
}

static void fmt_mins(char *buf, size_t n, int mins) {
  if (mins >= 60) snprintf(buf, n, "%dh%02dm", mins / 60, mins % 60);
  else snprintf(buf, n, "%dm", mins);
}

static GColor battery_color(void) {
  if (s.charging) return GColorMalachite;
  if (s.battery <= 15) return GColorRed;
  if (s.battery <= 35) return GColorChromeYellow;
  return GColorWhite;
}

static void render(void) {
  if (!s.have) {
    text_layer_set_text(s_pct, "--%");
    text_layer_set_text(s_power, "");
    text_layer_set_text(s_details, "Connecting…");
    text_layer_set_text(s_game, s.status);
    return;
  }

  snprintf(s_pct_buf, sizeof(s_pct_buf), "%d%%", s.battery);
  text_layer_set_text(s_pct, s_pct_buf);
  text_layer_set_text_color(s_pct, battery_color());

  char t[16] = "";
  if (s.mins) fmt_mins(t, sizeof(t), s.mins);
  snprintf(s_power_buf, sizeof(s_power_buf), "%s %d.%dW%s%s%s",
           s.charging ? "+" : "-", s.watts10 / 10, s.watts10 % 10,
           s.mins ? " · " : "", t, s.mins ? (s.charging ? " to full" : " left") : "");
  text_layer_set_text(s_power, s_power_buf);

  snprintf(s_details_buf, sizeof(s_details_buf), "CPU %d%%  %d°C\nRAM %d%%\nVol %d%%%s",
           s.cpu, s.cpu_temp, s.mem, s.volume, s.muted ? " (muted)" : "");
  text_layer_set_text(s_details, s_details_buf);

  // An error/status line wins over the game title
  text_layer_set_text(s_game, s.status[0] ? s.status : (s.game[0] ? s.game : "No game running"));

  if (s_bat_text) {
    char t2[16] = "—";
    if (s.mins) fmt_mins(t2, sizeof(t2), s.mins);
    snprintf(s_bat_buf, sizeof(s_bat_buf),
             "Charge   %d%%\nState    %s\nPower    %d.%d W\n%s %s\nVoltage  %d.%02d V\nTemp     %d°C\nHealth   %d%%",
             s.battery, s.charging ? "Charging" : "On battery", s.watts10 / 10, s.watts10 % 10,
             s.charging ? "To full " : "Left    ", t2, s.volts100 / 100, s.volts100 % 100,
             s.bat_temp, s.health);
    text_layer_set_text(s_bat_text, s_bat_buf);
  }
}

static TextLayer *text(Layer *root, GRect r, const char *font, GTextAlignment align) {
  TextLayer *t = text_layer_create(r);
  text_layer_set_background_color(t, GColorClear);
  text_layer_set_text_color(t, GColorWhite);
  text_layer_set_font(t, fonts_get_system_font(font));
  text_layer_set_text_alignment(t, align);
  layer_add_child(root, text_layer_get_layer(t));
  return t;
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
  s_games_section = (SimpleMenuSection){ .title = "Launch game", .items = s_game_items, .num_items = n };
  Layer *root = window_get_root_layer(w);
  s_games = simple_menu_layer_create(layer_get_bounds(root), w, &s_games_section, 1, NULL);
  layer_add_child(root, simple_menu_layer_get_layer(s_games));
}

static void games_unload(Window *w) {
  simple_menu_layer_destroy(s_games);
}

// ---- battery window ---------------------------------------------------------

static TextLayer *s_bat_title;

static void bat_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_bat_title = text(root, GRect(0, 4, b.size.w, 30), FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter);
  text_layer_set_text(s_bat_title, "Battery");
  s_bat_text = text(root, GRect(10, 38, b.size.w - 20, b.size.h - 40), FONT_KEY_GOTHIC_24, GTextAlignmentLeft);
  render();
}

static void bat_unload(Window *w) {
  text_layer_destroy(s_bat_title);
  text_layer_destroy(s_bat_text);
  s_bat_text = NULL;  // render() skips the battery window while it's closed
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
  s_sections[0] = (SimpleMenuSection){ .title = "Quick settings", .items = s_quick_items, .num_items = M_COUNT_QUICK };
  s_sections[1] = (SimpleMenuSection){ .title = "Power", .items = s_power_items, .num_items = P_COUNT };

  Layer *root = window_get_root_layer(w);
  s_menu = simple_menu_layer_create(layer_get_bounds(root), w, s_sections, 2, NULL);
  layer_add_child(root, simple_menu_layer_get_layer(s_menu));
}

static void menu_unload(Window *w) {
  simple_menu_layer_destroy(s_menu);
}

// ---- main window ------------------------------------------------------------

static void up_click(ClickRecognizerRef r, void *ctx) { send_action("vol_up"); }
static void down_click(ClickRecognizerRef r, void *ctx) { send_action("vol_down"); }
static void select_click(ClickRecognizerRef r, void *ctx) { window_stack_push(s_menu_win, true); }
static void select_long(ClickRecognizerRef r, void *ctx) { send_action("mute"); vibes_short_pulse(); }

static void click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 200, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 200, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

static void main_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  int wd = b.size.w;

  StatusBarLayer *bar = status_bar_layer_create();
  status_bar_layer_set_colors(bar, GColorOxfordBlue, GColorWhite);
  layer_add_child(root, status_bar_layer_get_layer(bar));

  text_layer_set_text(text(root, GRect(0, 18, wd, 24), FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentCenter),
                      "STEAM FRAME");
  s_pct = text(root, GRect(0, 36, wd, 50), FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter);
  s_power = text(root, GRect(0, 88, wd, 28), FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter);
  s_details = text(root, GRect(12, 118, wd - 24, 72), FONT_KEY_GOTHIC_18, GTextAlignmentLeft);
  s_game = text(root, GRect(6, b.size.h - 34, wd - 12, 30), FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentCenter);
  text_layer_set_text_color(s_game, GColorPictonBlue);
  render();
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
  s_main = make_window((WindowHandlers){ .load = main_load }, GColorOxfordBlue);
  window_set_click_config_provider(s_main, click_config);
  s_menu_win = make_window((WindowHandlers){ .load = menu_load, .unload = menu_unload }, GColorWhite);
  s_bat_win = make_window((WindowHandlers){ .load = bat_load, .unload = bat_unload }, GColorOxfordBlue);
  s_games_win = make_window((WindowHandlers){ .load = games_load, .unload = games_unload }, GColorWhite);

  app_message_register_inbox_received(inbox);
  app_message_open(1024, 64);
  window_stack_push(s_main, true);
  app_event_loop();
  // windows/layers are freed by the OS on app exit
}
