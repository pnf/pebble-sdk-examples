#include "simple_analog.h"

#include "pebble.h"

#include "string.h"
#include "stdlib.h"

Layer *simple_bg_layer;

Layer *date_layer;
TextLayer *day_label;
char day_buffer[6];
TextLayer *num_label;
char num_buffer[4];
TextLayer *logo_label;
char logo_buffer[8];

static const char *BRAND = "BOFFO";

static GPath *minute_arrow;
static GPath *hour_arrow;
static GPath *tick_paths[NUM_CLOCK_TICKS];
Layer *hands_layer;
Window *window;


/*
  The plan:
  1. Winding level starts at 100 and decreases by UNWIND_RATE per hour, stopping at zero.
  2. Winding level increases 20 on each call to init, stopping at 100.
  3. At zero, the watch stops updating.
  4. Maintain skew counter, which changes at rate of (wlevel - 50)/50 * SKEW_RATE/hour
  5. Time will be t + skew-minutes
  6. At wind level of 100, skew is reset to zero.
  7. Store mday and t as of full wind.


w = w0 - u (t-t0)
s = s0 + \int_t0^t a (w - wz) dt'
  = s0 + a \int (w0 - u t' + u t0 - wz) dt'
  = s0 + a [w0 + u t0 - wz](t-t0) - 0.5 a u (t - t0)(t+t0)
  = s0 + a (t-t0) [w0 + u t0 - wz - 0.5 u t - 0.5 u t0]
  = s0 + a (t-t0) [w0 - wz - 0.5 u (t - t0)]
 */

//uint32_t BUTT_KEY = 1;
//static int butt = 0;

// Day of month when last fully wound
static const uint32_t DWOUND_KEY = 2;
static int dwound = -1;

// Time when last fully wound
static const uint32_t TWOUND_KEY = 3;
static long twound = -1;

// Winding level
static const uint32_t WLEVEL_KEY = 4;
static double w0;
static double w;

// Skew
static const uint32_t SKEW_KEY = 5;
static double s0;
static double s;

static const uint32_t T0_KEY = 6;
static long t0 = 0L;  // time when saved
static long t1 = 0L;  // time when updated internally

static void get_state() {
  if(persist_exists(TWOUND_KEY)) {
    APP_LOG(APP_LOG_LEVEL_DEBUG,"Loading state from storage.\n");
    persist_read_data(TWOUND_KEY,&twound,sizeof(twound));
    dwound = persist_read_int(DWOUND_KEY);
    persist_read_data(WLEVEL_KEY,&w0,sizeof(w0)); w=w0;
    persist_read_data(SKEW_KEY,&s0,sizeof(s0)); s=s0;
    persist_read_data(T0_KEY,&t0,sizeof(t0)); t1=t0;
  }
  else {
    APP_LOG(APP_LOG_LEVEL_DEBUG,"Initializing new state.\n");
    t0 = t1 = twound = time(NULL);
    dwound = -1;
    w = w0 = 100.;
    s = s0 = 0.;
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG,
	  "dwound=%d twound=%ld w=%d s=%d t0=%ld\n",
	  dwound,twound,(int)w0,(int)s0,t0);

}

static void save_state() {
  persist_write_int(DWOUND_KEY,dwound);
  persist_write_data(TWOUND_KEY,&twound,sizeof(twound));
  persist_write_data(WLEVEL_KEY,&w,sizeof(w));
  persist_write_data(SKEW_KEY,&s,sizeof(s));
  persist_write_data(T0_KEY,&t1,sizeof(t1));
}

static const int WIND_RATE =   20;   /* per click */
static const int UNWIND_RATE = 4;  /* per hour */
static const int SKEW_RATE =  2; /* minutes per day per 100 winds */
static const double W0 = 60.;
static const int JUMP_SEC = 7;

static void set_skew() {
  static const double u = UNWIND_RATE/3600.;  // units per second
  static const double a = SKEW_RATE/(60.*100.);   // seconds per day per unit wind
  t1 = time(NULL);
  w = w0 - u * (t1-t0);
  if(w<=0.0) {
    w = 0.0;
    t1 = w0/u + t0;  // time the clock stopped
  }
  s = s0 + a * (t1-t0) * (w0 - W0 - 0.5 * u * (t1-t0));

  APP_LOG(APP_LOG_LEVEL_DEBUG, "set_skew: t1=%ld w=%d s=%d\n",t1,(int)w,(int)s);
  
}

static void bg_update_proc(Layer *layer, GContext *ctx) {

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  graphics_context_set_fill_color(ctx, GColorWhite);
  for (int i = 0; i < NUM_CLOCK_TICKS; ++i) {
    gpath_draw_filled(ctx, tick_paths[i]);
  }
}

static void hands_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const GPoint center = grect_center_point(&bounds);
  const int16_t secondHandLength = bounds.size.w / 2;

  GPoint secondHand;

  time_t now = time(NULL) + s;
  struct tm *t = localtime(&now);

  int jsec = t->tm_sec + (rand() % JUMP_SEC);
  int32_t second_angle = TRIG_MAX_ANGLE * jsec / 60;
  secondHand.y = (int16_t)(-cos_lookup(second_angle) * (int32_t)secondHandLength / TRIG_MAX_RATIO) + center.y;
  secondHand.x = (int16_t)(sin_lookup(second_angle) * (int32_t)secondHandLength / TRIG_MAX_RATIO) + center.x;

  // second hand
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_line(ctx, secondHand, center);

  // minute/hour hand
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorBlack);

  gpath_rotate_to(minute_arrow, TRIG_MAX_ANGLE * t->tm_min / 60);
  gpath_draw_filled(ctx, minute_arrow);
  gpath_draw_outline(ctx, minute_arrow);

  gpath_rotate_to(hour_arrow, (TRIG_MAX_ANGLE * (((t->tm_hour % 12) * 6) + (t->tm_min / 10))) / (12 * 6));
  gpath_draw_filled(ctx, hour_arrow);
  gpath_draw_outline(ctx, hour_arrow);

  // dot in the middle
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(bounds.size.w / 2 - 1, bounds.size.h / 2 - 1, 3, 3), 0, GCornerNone);
}

static void date_update_proc(Layer *layer, GContext *ctx) {

  time_t now = time(NULL);
  // struct tm *t = localtime(&now);

  int d = (dwound-1) + (((now-twound)/(24*3600)) % 31) + 1;
  
  // strftime(day_buffer, sizeof(day_buffer), "%a", t);
  // snprintf(day_buffer,sizeof(day_buffer),"%d",(int)s);
  text_layer_set_text(day_label, day_buffer);

  /* strftime(num_buffer, sizeof(num_buffer), "%d", t); */
  snprintf(num_buffer,sizeof(num_buffer),"%d",d);
  day_buffer[0] = 'A' + rand() % ('Z'-'A');
  day_buffer[1] = 'a' + rand() % ('z'-'a');
  day_buffer[2] = 'a' + rand() % ('z'-'a');
  day_buffer[3] = 0;
  text_layer_set_text(num_label, num_buffer);

  snprintf(logo_buffer,sizeof(logo_buffer),"%s",BRAND);
  text_layer_set_text(logo_label,logo_buffer);
}

static void handle_tick(struct tm *tick_time, TimeUnits units_changed) {
  if(units_changed & MINUTE_UNIT) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Minute tick");
    set_skew();
  }
  if(units_changed & SECOND_UNIT &&
     (tick_time->tm_sec % JUMP_SEC)==0
     w>0.0) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Second tick");
    layer_mark_dirty(window_get_root_layer(window));
  }
}

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // init layers
  simple_bg_layer = layer_create(bounds);
  layer_set_update_proc(simple_bg_layer, bg_update_proc);
  layer_add_child(window_layer, simple_bg_layer);

  // init date layer -> a plain parent layer to create a date update proc
  date_layer = layer_create(bounds);
  layer_set_update_proc(date_layer, date_update_proc);
  layer_add_child(window_layer, date_layer);

  GFont norm18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont bold18 = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  // init logo
  logo_label = text_layer_create(GRect(48, 20, 54, 20));
  text_layer_set_text(logo_label, logo_buffer);
  text_layer_set_background_color(logo_label, GColorBlack);
  text_layer_set_text_color(logo_label, GColorWhite);
  text_layer_set_font(logo_label, bold18);

  // init day
  day_label = text_layer_create(GRect(46, 114, 27, 20));
  text_layer_set_text(day_label, day_buffer);
  text_layer_set_background_color(day_label, GColorBlack);
  text_layer_set_text_color(day_label, GColorWhite);
  text_layer_set_font(day_label, norm18);

  layer_add_child(date_layer, text_layer_get_layer(day_label));
  layer_add_child(date_layer, text_layer_get_layer(logo_label));

  // init num
  num_label = text_layer_create(GRect(73, 114, 18, 20));

  text_layer_set_text(num_label, num_buffer);
  text_layer_set_background_color(num_label, GColorBlack);
  text_layer_set_text_color(num_label, GColorWhite);
  text_layer_set_font(num_label, bold18);

  layer_add_child(date_layer, text_layer_get_layer(num_label));

  // init hands
  hands_layer = layer_create(bounds);
  layer_set_update_proc(hands_layer, hands_update_proc);
  layer_add_child(window_layer, hands_layer);
}

static void window_unload(Window *window) {
  layer_destroy(simple_bg_layer);
  layer_destroy(date_layer);
  text_layer_destroy(day_label);
  text_layer_destroy(num_label);
  text_layer_destroy(logo_label);
  layer_destroy(hands_layer);
}

static void init(void) {

  get_state();

  window = window_create();
  window_set_window_handlers(window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });

  day_buffer[0] = '\0';
  num_buffer[0] = '\0';

  // init hand paths
  minute_arrow = gpath_create(&MINUTE_HAND_POINTS);
  hour_arrow = gpath_create(&HOUR_HAND_POINTS);

  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  const GPoint center = grect_center_point(&bounds);
  gpath_move_to(minute_arrow, center);
  gpath_move_to(hour_arrow, center);

  // init clock face paths
  for (int i = 0; i < NUM_CLOCK_TICKS; ++i) {
    tick_paths[i] = gpath_create(&ANALOG_BG_POINTS[i]);
  }

  // Push the window onto the stack
  const bool animated = true;
  window_stack_push(window, animated);


  w = w + WIND_RATE;
  if(w>=100) {
    w = 100.;
    s = 0L;
    t0 = twound = time(NULL);
    struct tm *t = localtime(&twound);
    dwound = t->tm_mday;
  }
  
  tick_timer_service_subscribe(SECOND_UNIT|HOUR_UNIT|MINUTE_UNIT, handle_tick);
}

static void deinit(void) {

  save_state();
  
  gpath_destroy(minute_arrow);
  gpath_destroy(hour_arrow);

  for (int i = 0; i < NUM_CLOCK_TICKS; ++i) {
    gpath_destroy(tick_paths[i]);
  }

  tick_timer_service_unsubscribe();
  window_destroy(window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
