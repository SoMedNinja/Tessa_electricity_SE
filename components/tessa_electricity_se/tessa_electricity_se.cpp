#include "tessa_electricity_se.h"
#include "formula.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome::tessa_electricity_se {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

constexpr int MAX_DAY = 100;    // a day's slots, at most: 100 quarters on the day the clock goes back
constexpr int MAX_SLOTS = 2 * MAX_DAY;  // today and tomorrow
constexpr int MAX_CELLS = 60;  // as many as the meter's bar has
constexpr int TILE_BARS = 200;  // keep all quarter-hour slots when today's and tomorrow's prices are present

// ---- The prices: what the sensor says, one price per slot (a quarter of an hour when the sensor has them, else an hour) ----
struct Prices {
  float slot[MAX_SLOTS] = {};  // the delivery price, the tariff's formula applied
  float feed[MAX_SLOTS] = {};  // the feed-in price, when the tariff has one
  int today = 0;     // today's slots: 24 hours or 96 quarters, 23/25 or 92/100 on the days the clock changes; 0: no graph
  int tomorrow = 0;  // tomorrow's, in today's resolution, after today's; 0 until the sensor has them
  float current = 0, feed_current = 0;  // the price of now: today's list at now, else the sensor's state
  bool has_current = false;
  float state_current = 0, state_feed = 0;  // the sensor's state through the tariff
  bool state_ok = false;
  bool has_feed = false;  // the tariff has a feed-in price: `feed` and `feed_current` hold it
  bool chosen = false;  // the tile has a sensor
  char unit[16] = "";
  float lo = 0, hi = 0, avg = 0;  // of today
  int lo_slot = 0, hi_slot = 0;   // the slots of today they are in
  int count() const { return today + tomorrow; }
};

// A list of a day's prices into `out`, one slot each: 24 hours or 96 quarters, and 23/25 hours or 92/100 quarters on
// the days the clock changes. A list finer than quarters is joined to at most MAX_DAY slots. Returns the slots, 0 for
// no list.
// `n` prices of the list `a` from its price `from` on, as read_day reads a whole one.
static int read_span(JsonArrayConst a, int from, int n, float scale, float *out) {
  if (n < 23 || from < 0) return 0;
  const int join = (n + MAX_DAY - 1) / MAX_DAY;
  const int slots = (n + join - 1) / join;
  float sum[MAX_DAY] = {};
  int seen[MAX_DAY] = {};
  int k = 0;
  for (JsonVariantConst v : a) {
    const int i = k++ - from;
    if (i < 0) continue;
    if (i >= n) break;
    if (v.is<float>()) {
      sum[i / join] += v.as<float>() * scale;
      seen[i / join]++;
    }
  }
  for (int s = 0; s < slots; s++) out[s] = seen[s] ? sum[s] / seen[s] : NAN;
  return slots;
}

static int read_day(JsonVariantConst list, float scale, float *out) {
  JsonArrayConst a = list.as<JsonArrayConst>();
  return read_span(a, 0, static_cast<int>(a.size()), scale, out);
}

// A day of `n` slots in quarters or in hours, so that tomorrow has today's: an hour becomes four quarters, four quarters
// one hour. Returns its slots now.
static int resolve(float *day, int n, bool quarters) {
  if ((n >= 90) == quarters) return n;
  if (quarters) {
    if (n * 4 > MAX_DAY) return 0;
    for (int s = n * 4 - 1; s >= 0; s--) day[s] = day[s / 4];
    return n * 4;
  }
  const int hours = (n + 3) / 4;
  for (int h = 0; h < hours; h++) {
    float sum = 0;
    int seen = 0;
    for (int s = h * 4; s < std::min(n, h * 4 + 4); s++) {
      if (std::isnan(day[s])) continue;
      sum += day[s];
      seen++;
    }
    day[h] = seen ? sum / seen : NAN;
  }
  return hours;
}

static uint32_t midnight_of(uint32_t epoch);

// The EPEX Spot sensor (HACS `epex_spot`) keeps one list of yesterday, today and, from the afternoon, tomorrow, each
// slot with its start. Today's slots begin at the screen's midnight and tomorrow's at the next: both are cut out of the
// list by the start of its first slot and the length of a slot (an hour, or a quarter when the sensor has them). False
// while the clock is not set or the list does not reach today. After Nekromonger's tessera-strompreis.
static bool read_epex(JsonObjectConst a, float scale, Prices &p) {
  JsonArrayConst list = a["prices"].as<JsonArrayConst>();
  const int n = static_cast<int>(list.size());
  const uint32_t first = a["first"].as<uint32_t>(), second = a["second"].as<uint32_t>(), now = tessera::epoch();
  if (n < 2 || !first || !now) return false;
  const uint32_t step = second > first && second - first <= 3600 ? second - first : 3600;
  const uint32_t mid = midnight_of(now), next = midnight_of(mid + 26 * 3600), after = midnight_of(next + 26 * 3600);
  if (mid < first) return false;
  const int from = static_cast<int>((mid - first) / step), today = static_cast<int>((next - mid) / step);
  if (from + today > n) return false;
  p.today = read_span(list, from, today, scale, p.slot);
  if (!p.today) return false;
  const int to = from + today, tomorrow = std::min(static_cast<int>((after - next) / step), n - to);
  if (tomorrow > 0) p.tomorrow = resolve(p.slot + p.today, read_span(list, to, tomorrow, scale, p.slot + p.today), p.today >= 90);
  return true;
}

// Delivery applies tax and an additional charge to the selected price sensor. Optional sales display applies its own
// additional amount to that same sensor; this Tessera tile-option API does not support a second entity selector.
struct Tariff {
  Formula delivery, feed_in;
  float sales_additional = 0;
  bool feed = false;
  bool error = false;  // invalid text: show a clear prompt instead of a silently wrong price
};

// Parse a free-text tariff amount: at most two whole digits and no more decimal digits than the display precision.
// A comma or point may be used as the decimal mark. Empty text means zero; costs may be negative, tax may not.
static bool read_amount(JsonObjectConst options, const char *id, int decimals, bool negative, float &out) {
  const char *text = options[id] | "0";
  while (*text == ' ' || *text == '\t') text++;
  if (!*text) { out = 0; return true; }
  char value[16];
  size_t n = 0, whole = 0, fraction = 0;
  bool point = false, digit = false;
  if (*text == '-' || *text == '+') {
    if (*text == '-' && !negative) return false;
    value[n++] = *text++;
  }
  for (; *text; text++) {
    const char c = *text;
    if (c >= '0' && c <= '9') {
      if (point) { if (++fraction > decimals) return false; }
      else if (++whole > 2) return false;
      value[n++] = c;
      digit = true;
    } else if ((c == '.' || c == ',') && !point) {
      point = true;
      value[n++] = '.';
    } else return false;
    if (n >= sizeof(value) - 1) return false;
  }
  if (!digit || whole == 0 || (point && fraction == 0)) return false;
  value[n] = 0;
  char *end = nullptr;
  out = std::strtof(value, &end);
  return end != value && *end == 0 && std::isfinite(out);
}

static Tariff tariff_of(JsonObjectConst options) {
  Tariff t;
  const int decimals = std::clamp<int>(static_cast<int>(std::lround(options["fixed"] | 2.0f)), 0, 4);
  float tax = 0, delivery_additional = 0;
  const bool valid_tax = read_amount(options, "tax", decimals, false, tax);
  const bool valid_delivery = read_amount(options, "delivery_additional", decimals, true, delivery_additional);
  const bool valid_sales = read_amount(options, "sales_additional", decimals, true, t.sales_additional);
  char expression[64];
  snprintf(expression, sizeof(expression), "p*%.6g+%.6g", 1.0f + tax / 100.0f, delivery_additional);
  const bool delivery = t.delivery.parse(expression);
  snprintf(expression, sizeof(expression), "p+%.6g", t.sales_additional);
  const bool sales = t.feed_in.parse(expression);
  t.feed = sales && (options["show_sales"] | false);
  t.error = !valid_tax || !valid_delivery || !valid_sales || !delivery || !sales;
  return t;
}

static void read_prices(JsonObjectConst data, bool cents, const Tariff &tariff, Prices &p) {
  p.today = p.tomorrow = 0;
  p.has_current = p.has_feed = p.state_ok = false;
  p.chosen = !data["state"].isNull();
  if (!p.chosen || tariff.error) return;
  // A price per MWh is made one per kWh, so the units, the decimals and the graph's labels read as for a sensor per kWh.
  snprintf(p.unit, sizeof(p.unit), "%s", data["attributes"]["unit_of_measurement"] | "");
  float scale = 1.0f;  // to the sensor's currency per kWh, which the tariff's formula takes
  const float shown = cents ? 100.0f : 1.0f;
  if (char *slash = strchr(p.unit, '/')) {
    if (strcmp(slash + 1, "MWh") == 0) {
      scale /= 1000.0f;
      snprintf(slash + 1, sizeof(p.unit) - (slash + 1 - p.unit), "kWh");
    }
  }
  const char *state = data["state"] | "";
  char *end = nullptr;
  const float v = strtof(state, &end);
  if (end != state && *end == 0) {
    p.state_current = p.current = tariff.delivery(v * scale) * shown;
    p.state_feed = p.feed_current = tariff.feed_in(v * scale) * shown;
    p.state_ok = p.has_current = true;
    p.has_feed = tariff.feed;
  }
  JsonObjectConst a = data["attributes"];
  if (!a["prices"].isNull()) {
    read_epex(a, scale, p);
  } else {
    p.today = read_day(a["today"], scale, p.slot);
    if (p.today) p.tomorrow = resolve(p.slot + p.today, read_day(a["tomorrow"], scale, p.slot + p.today), p.today >= 90);
  }
  for (int s = 0; s < p.count(); s++) {
    const float kwh = p.slot[s];
    p.slot[s] = std::isnan(kwh) ? NAN : tariff.delivery(kwh) * shown;
    p.feed[s] = std::isnan(kwh) ? NAN : tariff.feed_in(kwh) * shown;
  }
  if (p.today && tariff.feed) p.has_feed = true;
  float lo = INFINITY, hi = -INFINITY, sum = 0;
  int n = 0;
  p.lo_slot = p.hi_slot = 0;
  for (int s = 0; s < p.today; s++) {
    if (std::isnan(p.slot[s])) continue;
    if (p.slot[s] < lo) { lo = p.slot[s]; p.lo_slot = s; }
    if (p.slot[s] > hi) { hi = p.slot[s]; p.hi_slot = s; }
    sum += p.slot[s];
    n++;
  }
  p.lo = n ? lo : 0;
  p.hi = n ? hi : 0;
  p.avg = n ? sum / n : 0;
}

static const Prices NO_PRICES{};

// Rank a price within its own delivery day. This keeps tomorrow's classification independent of today's prices.
static float day_percentile(const Prices &p, int slot, float price) {
  if (slot < 0 || slot >= p.count() || std::isnan(price)) return 0.5f;
  const int first = slot < p.today ? 0 : p.today;
  const int last = slot < p.today ? p.today : p.count();
  int count = 0, below = 0;
  for (int i = first; i < last; i++) {
    if (std::isnan(p.slot[i])) continue;
    count++;
    if (p.slot[i] < price) below++;
  }
  return count ? static_cast<float>(below) / count : 0.5f;
}

// Green for the cheapest 30%, red for the priciest 30%, and a continuous green-to-amber-to-red ramp between them.
static lv_color_t price_color(float percentile) {
  const lv_color_t green = lv_color_hex(0x31B879);
  const lv_color_t amber = lv_color_hex(0xE8B64B);
  const lv_color_t red = lv_color_hex(0xE45B62);
  if (percentile <= 0.30f) return green;
  if (percentile >= 0.70f) return red;
  if (percentile <= 0.50f)
    return lv_color_mix(green, amber, static_cast<lv_opa_t>((percentile - 0.30f) * 1275));
  return lv_color_mix(amber, red, static_cast<lv_opa_t>((percentile - 0.50f) * 1275));
}

static std::string number(float v, int decimals) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

// "EUR/kWh" as "€/kWh", or "ct/kWh" when the prices are in cents.
static std::string unit_text(const std::string &unit, bool cents) {
  const size_t slash = unit.find('/');
  const std::string per = slash == std::string::npos ? "kWh" : unit.substr(slash + 1);
  std::string money = slash == std::string::npos ? unit : unit.substr(0, slash);
  if (cents) {
    money = "ct";
  } else if (money == "EUR" && ui::covers("\u20ac", Font::BODY)) {
    money = "\u20ac";
  } else if (money == "USD") {
    money = "$";
  }
  return money.empty() ? per : money + "/" + per;
}

static std::string unit_text(const Prices &p, bool cents) { return unit_text(std::string(p.unit), cents); }

// The moment the day of `epoch` began on the screen's clock: its midnight, also on a day the clock changes, when the
// time of day is an hour off the time since midnight.
static uint32_t midnight_of(uint32_t epoch) {
  uint32_t m = epoch;
  for (int i = 0; i < 3; i++) {
    const tessera::LocalTime t = tessera::local_time(m);
    const uint32_t since = t.hour * 3600 + t.minute * 60 + t.second;
    if (since == 0) break;
    // Past the clock's change, a step back of the time of day lands just before midnight: then forward to it.
    m = m != epoch && since >= 12 * 3600 ? m + (86400 - since) : m - since;
  }
  return m;
}

// When each slot is: today's from its midnight, tomorrow's from the next, each as long as its day divided by its
// slots (an hour or a quarter, also on the days of 23 and 25 hours).
struct Day {
  uint32_t start = 0, next = 0;  // today's midnight and tomorrow's; 0 while the clock is not set
  uint32_t step = 3600, next_step = 3600;
  int today = 0;
  bool known() const { return start != 0; }
  // When slot s begins; at(count()) is when the last one ends.
  uint32_t at(int s) const {
    return s < today ? start + static_cast<uint32_t>(s) * step : next + static_cast<uint32_t>(s - today) * next_step;
  }
  // The slot of the moment `epoch`, -1 while the clock is not set.
  int slot(uint32_t epoch) const {
    if (!known() || !today) return -1;
    if (epoch < next) return static_cast<int>((epoch - start) / step);
    return today + static_cast<int>((epoch - next) / next_step);
  }
};

static Day day_of(const Prices &p, uint32_t epoch);

// The price of now: today's or tomorrow's slot of the moment `epoch` where the clock is set and the list has it, else
// the sensor's state. The state is not always the price of now: ENTSO-e keeps its lists on the sensor of the day's
// average, whose state is that average. The list is also what the bar of now shows, so the two agree.
static void price_now(Prices &p, uint32_t epoch) {
  const int s = day_of(p, epoch).slot(epoch);
  if (s >= 0 && s < p.count() && !std::isnan(p.slot[s])) {
    p.current = p.slot[s];
    p.feed_current = p.feed[s];
    p.has_current = true;
  } else {
    p.current = p.state_current;
    p.feed_current = p.state_feed;
    p.has_current = p.state_ok;
  }
}

static Day day_of(const Prices &p, uint32_t epoch) {
  Day d;
  d.today = p.today;
  if (!epoch || !p.today) return d;
  d.start = midnight_of(epoch);
  d.next = midnight_of(d.start + 26 * 3600);
  d.step = (d.next - d.start) / p.today;
  d.next_step = p.tomorrow ? (midnight_of(d.next + 26 * 3600) - d.next) / p.tomorrow : d.step;
  return d;
}

// The slots of the 24 hours from the slot of now, as far as the prices go: from `from` to before `to`. All of them
// while the clock is not set.
static void window_of(const Prices &p, const Day &d, uint32_t epoch, int &from, int &to) {
  const int all = p.count(), at = d.slot(epoch);
  from = 0;
  to = all;
  if (at >= 0 && at < all) {
    from = at;
    to = std::clamp(d.slot(d.at(at) + 24 * 3600), from + 1, all);
  }
}

// With only the next 24 hours shown: their lowest, highest and average price in place of today's.
static void range_ahead(Prices &p, const Day &d, uint32_t epoch) {
  int from = 0, to = 0;
  window_of(p, d, epoch, from, to);
  float lo = INFINITY, hi = -INFINITY, sum = 0;
  int n = 0;
  for (int s = from; s < to; s++) {
    if (std::isnan(p.slot[s])) continue;
    if (p.slot[s] < lo) { lo = p.slot[s]; p.lo_slot = s; }
    if (p.slot[s] > hi) { hi = p.slot[s]; p.hi_slot = s; }
    sum += p.slot[s];
    n++;
  }
  if (!n) return;
  p.lo = lo;
  p.hi = hi;
  p.avg = sum / n;
}

// ---- The graph, in the look of Tessera's history graph: price labels on the left, hairlines across, the bars, and
// clock times under them with a small mark each ----
constexpr int MAX_TICKS = 7;   // lines across, at most
constexpr int MAX_TIMES = 24;  // times under the graph, at most
// The lines across the graph at its price labels: none, solid or dotted.
enum class Grid : uint8_t { NONE, SOLID, DOTTED };

static Grid grid_of(const char *option) {
  return strcmp(option, "solid") == 0 ? Grid::SOLID : strcmp(option, "dotted") == 0 ? Grid::DOTTED : Grid::NONE;
}

constexpr int MAX_RUNS = 24;   // lines of one colour the stepped line is drawn in, at most; the last takes the rest

class Chart {
 public:
  // The graph takes the rectangle x, y, w, h: its plot, the price labels left of it and the times under it.
  // `bars` is how many bars it may draw, `times` how many times it may write under the bars.
  // `line` draws a stepped line in place of the bars, with the bars as a faint fill under it when `fill`.
  // `ahead` draws only the 24 hours from the slot of now, as far as the prices go, in place of today and tomorrow.
  void create(lv_obj_t *parent, int x, int y, int w, int h, int bars, int times, bool cents, bool line = false,
              bool fill = true, Grid grid = Grid::NONE, bool ahead = false) {
    cents_ = cents;
    ahead_ = ahead;
    grid_style_ = grid;
    line_ = line;
    fill_ = fill;
    bars_n_ = std::min(bars, MAX_SLOTS);
    times_n_ = std::min(times, MAX_TIMES);
    const bool big = ui::large();
    label_h_ = ui::line_height(Font::BODY);
    const int side = ui::px(big ? 8 : 5), under = ui::px(big ? 8 : 4);
    label_w_ = ui::text_width(cents ? "-000" : "-0.00", Font::BODY);
    x_ = x;
    ax_ = x + label_w_ + side;
    aw_ = std::max(10, w - label_w_ - side);
    ay_ = y;
    ah_ = std::max(10, h - label_h_ - under);
    time_y_ = ay_ + ah_ + under;
    mark_y_ = ay_ + ah_ + ui::px(big ? 1 : 1);
    mark_h_ = ui::px(big ? 5 : 3);
    for (int i = 0; i < MAX_TICKS; i++) {
      // A dotted line is a line with dashes of a dot; a solid one a block a pixel high.
      if (grid_style_ == Grid::DOTTED) {
        grid_[i] = step_line(parent, 1);
        lv_obj_set_style_line_dash_width(grid_[i], std::max(1, ui::px(big ? 2 : 1)), 0);
        lv_obj_set_style_line_dash_gap(grid_[i], std::max(2, ui::px(big ? 4 : 3)), 0);
      } else {
        grid_[i] = hairline(parent, theme::GRID);
      }
      price_[i] = ui::label(parent, Font::BODY, theme::SUBTLE);
      lv_obj_set_style_text_align(price_[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
    }
    divider_ = hairline(parent, theme::TICK);
    for (int i = 0; i < bars_n_; i++) {
      bars_[i] = ui::block(parent, theme::TRACK);
      lv_obj_set_style_radius(bars_[i], line_ ? 0 : ui::px(1), 0);
      lv_obj_add_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (line_) {
      // Over the fill: the feed-in price as a thin line, then the delivery price in runs of one colour, then the marks
      // of now and of a tapped slot. The points stay in PSRAM for as long as the lines show them.
      const int thick = std::max(2, ui::px(big ? 3 : 2));
      feed_line_ = step_line(parent, std::max(1, thick / 2));
      for (int i = 0; i < MAX_RUNS; i++) runs_[i] = step_line(parent, thick);
      points_.resize(2 * bars_n_ + MAX_RUNS);
      feed_points_.resize(2 * bars_n_ + 1);
      mark_ = hairline(parent, theme::INK);
      const int d = std::max(5, ui::px(big ? 9 : 7));
      for (lv_obj_t **dot : {&now_dot_, &sel_dot_, &feed_dot_}) {
        *dot = hairline(parent, theme::INK);
        lv_obj_set_size(*dot, d, d);
        lv_obj_set_style_radius(*dot, LV_RADIUS_CIRCLE, 0);
      }
    }
    for (int i = 0; i < times_n_; i++) {
      marks_[i] = hairline(parent, theme::TICK);
      time_[i] = ui::label(parent, Font::BODY, theme::SUBTLE);
      lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Where the second day starts, for a label above the graph; 0 without a second day.
  int midnight_x() const { return mid_x_; }
  int left() const { return ax_; }

  // The bar under a finger at x, y (the chart's own pixels), or -1.
  int bar_at(int x, int y) const {
    if (bars_now_ <= 0 || y < ay_ - label_h_ || y > ay_ + ah_ + label_h_ || x < ax_ || x >= ax_ + aw_) return -1;
    const int i = static_cast<int>(static_cast<long long>(x - ax_) * bars_now_ / aw_);
    return i >= 0 && i < bars_now_ && !std::isnan(value_[i]) ? i : -1;
  }
  float bar_value(int i) const { return value_[i]; }
  // The feed-in price bar i stands on; 0 without one.
  float bar_feed(int i) const { return base_[i]; }
  // The first and the one past the last slot (of all the days) that bar i stands for.
  int bar_from(int i) const { return from_ + i * group_; }
  int bar_to(int i, int n) const { return std::min(n, from_ + (i + 1) * group_); }
  void select(int i) { sel_ = i; }

  // The past is grey, the bar of now the accent, the bars to come green, amber or red by their place in the range of
  // the delivery price. With a feed-in price each bar stands on it instead of on zero: it runs from what a kWh brings
  // to what it costs.
  void update(const Prices &p, const Day &d, uint32_t epoch) {
    // The slots drawn: all of them, or the 24 hours from now (`from` to `to`); `now` and the bars count from `from`.
    const int all = p.count(), at = d.slot(epoch);
    int from = 0, to = all;
    if (ahead_) window_of(p, d, epoch, from, to);
    from_ = from;
    const int n = to - from;
    const int now = at < 0 ? -1 : at - from;
    const int group = n > bars_n_ ? (n + bars_n_ - 1) / bars_n_ : 1;
    const int bars = n ? (n + group - 1) / group : 0;
    float v[MAX_SLOTS];
    float lo = 0, hi = 0, mn = INFINITY, mx = -INFINITY;
    for (int b = 0; b < bars; b++) {
      float sum = 0;
      int seen = 0;
      for (int s = from + b * group; s < std::min(to, from + (b + 1) * group); s++) {
        if (std::isnan(p.slot[s])) continue;
        sum += p.slot[s];
        seen++;
      }
      v[b] = seen ? sum / seen : NAN;
      base_[b] = 0;
      if (std::isnan(v[b])) continue;
      if (p.has_feed) {
        float feed = 0;
        int known = 0;
        for (int s = from + b * group; s < std::min(to, from + (b + 1) * group); s++) {
          if (std::isnan(p.feed[s])) continue;
          feed += p.feed[s];
          known++;
        }
        base_[b] = known ? feed / known : 0;
      }
      lo = std::min({lo, v[b], base_[b]});
      hi = std::max({hi, v[b], base_[b]});
      mn = std::min(mn, v[b]);
      mx = std::max(mx, v[b]);
    }
    mid_x_ = 0;
    group_ = group;
    bars_now_ = bars;
    for (int b = 0; b < bars; b++) value_[b] = v[b];
    if (sel_ >= bars) sel_ = -1;
    if (!bars) {
      for (int i = 0; i < bars_n_; i++) lv_obj_add_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
      for (int i = 0; i < MAX_TICKS; i++) {
        lv_obj_add_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
      }
      for (int i = 0; i < times_n_; i++) {
        lv_obj_add_flag(marks_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_add_flag(divider_, LV_OBJ_FLAG_HIDDEN);
      hide_line(0);
      return;
    }

    // The lines across: a round step (1, 2 or 5 times a power of ten) that gives a few of them, from below the
    // lowest bar to above the highest.
    const float least = cents_ ? 1.0f : 0.01f;
    const int want = std::clamp(ah_ / (label_h_ * 2), 2, 5);
    const float raw = std::max(hi - lo, least) / want;
    const float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    float step = 10 * mag;
    for (float m : {1.0f, 2.0f, 5.0f})
      if (m * mag >= raw) { step = m * mag; break; }
    step = std::max(step, least);
    float bottom = 0, top = 0;
    int lines = 0;
    for (;;) {
      bottom = std::floor(lo / step + 0.0001f) * step;
      top = std::ceil(hi / step - 0.0001f) * step;
      if (top <= bottom) top = bottom + step;
      lines = static_cast<int>(std::lround((top - bottom) / step)) + 1;
      if (lines <= MAX_TICKS) break;
      step *= 2;
    }
    const float range = top - bottom;
    for (int i = 0; i < MAX_TICKS; i++) {
      if (i >= lines) {
        lv_obj_add_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const float value = bottom + step * i;
      const int ty = ay_ + static_cast<int>(std::lround(ah_ * (top - value) / range));
      const int gy = std::min(ty, ay_ + ah_ - 1);
      if (grid_style_ == Grid::DOTTED) {
        grid_points_[i][0] = point(ax_, gy);
        grid_points_[i][1] = point(ax_ + aw_, gy);
        lv_line_set_points(grid_[i], grid_points_[i], 2);
        lv_obj_set_style_line_color(grid_[i], ui::color(theme::TICK), 0);
        lv_obj_clear_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
      } else if (grid_style_ == Grid::SOLID) {
        lv_obj_set_size(grid_[i], aw_, 1);
        lv_obj_set_pos(grid_[i], ax_, gy);
        ui::set_fill(grid_[i], Tone::NORMAL, theme::GRID);
        lv_obj_clear_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_clear_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(price_[i], label_w_ + 2, label_h_);
      lv_obj_set_pos(price_[i], x_, std::max(ay_ - label_h_ / 2, ty - label_h_ / 2));
      ui::set_text(price_[i], number(std::fabs(value) < step / 100 ? 0.0f : value, cents_ ? 0 : 2));
      ui::set_color(price_[i], theme::SUBTLE);
    }

    // The bars, from the line of zero or from the feed-in price.
    // The bars share the whole width of the plot, also when it does not divide by their number.
    const int slot = aw_ / bars;
    const int gap = line_ ? 0 : slot >= 6 ? 2 : (slot >= 3 ? 1 : 0);  // a fill is one area
    const auto edge_x = [&](int i) { return static_cast<int>(static_cast<long long>(aw_) * i / bars); };
    const auto y_of = [&](float value) { return ay_ + static_cast<int>(std::lround(ah_ * (top - value) / range)); };
    const int now_bar = now < 0 ? -1 : now / group;
    for (int i = 0; i < bars_n_; i++) {
      lv_obj_t *bar = bars_[i];
      if (i >= bars || std::isnan(v[i]) || (line_ && !fill_)) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
      const int high = y_of(std::max(v[i], base_[i])), low = y_of(std::min(v[i], base_[i]));
      const int len = std::max(2, low - high);
      lv_obj_set_size(bar, std::max(1, edge_x(i + 1) - edge_x(i) - gap), len);
      lv_obj_set_pos(bar, ax_ + edge_x(i), v[i] >= base_[i] ? low - len : high);
      if (i == sel_ && !line_) {
        ui::set_fill(bar, Tone::NORMAL, theme::INK);
      } else if (i < now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::TRACK);
      } else if (i == now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::ACCENT);
      } else {
        lv_obj_set_style_bg_color(bar, price_color(day_percentile(p, from + i * group, v[i])), 0);
      }
      // Under a stepped line the bars are its fill: faint, the past a little less so as its grey is light already.
      if (line_) lv_obj_set_style_bg_opa(bar, i < now_bar ? LV_OPA_50 : LV_OPA_20, 0);
    }
    if (line_) draw_line(p, bars, v, now_bar, edge_x, y_of);

    // The line between the days.
    if (p.tomorrow > 0 && p.today > from && p.today < to) {
      mid_x_ = ax_ + edge_x((p.today - from) / group);
      lv_obj_clear_flag(divider_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(divider_, 1, ah_);
      lv_obj_set_pos(divider_, mid_x_, ay_);
      ui::set_fill(divider_, Tone::NORMAL, theme::TICK);
    } else {
      lv_obj_add_flag(divider_, LV_OBJ_FLAG_HIDDEN);
    }

    // The times under the bars: the clock hours that fit, a mark over each, as the history graph writes them. Each
    // stands where its slot begins, so a day of 23 or 25 hours has its hours where they are.
    for (int i = 0; i < times_n_; i++) {
      lv_obj_add_flag(marks_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!d.known()) return;
    const int hours = std::max(1, static_cast<int>((d.at(to) - d.at(from) + 1800) / 3600));
    const int between = ui::px(ui::large() ? 10 : 6);
    const int sample = ui::text_width(tessera::clock_text(d.start + 8 * 3600), Font::BODY);
    const float per_hour = static_cast<float>(aw_) / hours;
    int every = 12;
    for (int h : {1, 2, 3, 4, 6, 12})
      if (hours / h <= times_n_ && (sample + between) <= h * per_hour) { every = h; break; }
    int last = ax_ - 1000, k = 0;
    for (int s = from; s < to && k < times_n_; s++) {
      // Only a slot that begins on a whole hour, and of those the hours of the clock every `every`.
      const uint32_t when = d.at(s), day = s < d.today ? d.start : d.next;
      if ((when - day) % 3600 != 0) continue;
      const tessera::LocalTime t = tessera::local_time(when);
      if (t.minute != 0 || t.hour % every != 0) continue;
      const std::string words = tessera::clock_text(when);
      const int tw = ui::text_width(words, Font::BODY);
      const int cx = ax_ + static_cast<int>(static_cast<long long>(aw_) * (s - from) / n);
      const int left = std::max(ax_ - ui::px(ui::large() ? 8 : 4), cx - tw / 2);
      if (left <= last + between || left + tw > ax_ + aw_) continue;
      lv_obj_clear_flag(marks_[k], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(marks_[k], 1, mark_h_);
      lv_obj_set_pos(marks_[k], cx, mark_y_);
      ui::set_fill(marks_[k], Tone::NORMAL, theme::TICK);
      lv_obj_clear_flag(time_[k], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(time_[k], tw + 2, label_h_);
      lv_obj_set_pos(time_[k], left, time_y_);
      ui::set_text(time_[k], words);
      ui::set_color(time_[k], theme::SUBTLE);
      last = left + tw;
      k++;
    }
  }

 private:
  // A line of `width` pixels that takes no taps: the tile's and the card's on_tap find the bar.
  static lv_obj_t *step_line(lv_obj_t *parent, int width) {
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    return line;
  }

  static lv_point_precise_t point(int x, int y) {
    lv_point_precise_t q;
    q.x = x;
    q.y = y;
    return q;
  }

  // The stepped line's runs from `first` on and its marks out of sight; 0 hides all of it.
  void hide_line(int first) {
    if (!line_) return;
    for (int i = first; i < MAX_RUNS; i++) lv_obj_add_flag(runs_[i], LV_OBJ_FLAG_HIDDEN);
    if (first == 0) {
      lv_obj_add_flag(feed_line_, LV_OBJ_FLAG_HIDDEN);
      for (lv_obj_t *part : {mark_, now_dot_, sel_dot_, feed_dot_}) lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // A dot of the line centred on x, y.
  static void put_dot(lv_obj_t *dot, int x, int y, lv_color_t color) {
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(dot, x - lv_obj_get_width(dot) / 2, y - lv_obj_get_height(dot) / 2);
    lv_obj_set_style_bg_color(dot, color, 0);
  }

  // The stepped line: each bar's price a flat step across its slot, and a rise or drop to the next. The delivery
  // price is drawn in runs of one colour, as the bars are coloured: the past grey, now the accent, then green, amber or
  // red by its place in the range. The feed-in price, when there is one, is one thin line under it. A gap in the prices
  // breaks the line.
  template <class EdgeX, class YOf>
  void draw_line(const Prices &p, int bars, const float *v, int now_bar, EdgeX edge_x, YOf y_of) {
    const auto colour = [&](int i) {
      if (i < now_bar) return 0;
      if (i == now_bar) return 1;
      const float at = day_percentile(p, from_ + i * group_, v[i]);
      return 2 + static_cast<int>(at * 15.0f);
    };
    const auto color_of = [](int c) {
      switch (c) {
        case 0: return ui::color(theme::SUBTLE);
        case 1: return ui::color(theme::ACCENT);
        default: return price_color(static_cast<float>(c - 2) / 15.0f);
      }
    };
    int run = 0, k = 0, i = 0;
    while (i < bars && run < MAX_RUNS) {
      if (std::isnan(v[i])) {
        i++;
        continue;
      }
      const int first = k, c = colour(i);
      points_[k++] = point(ax_ + edge_x(i), y_of(v[i]));
      int j = i;
      for (; j < bars && !std::isnan(v[j]) && (colour(j) == c || run == MAX_RUNS - 1); j++) {
        points_[k++] = point(ax_ + edge_x(j + 1), y_of(v[j]));
        if (j + 1 < bars && !std::isnan(v[j + 1])) points_[k++] = point(ax_ + edge_x(j + 1), y_of(v[j + 1]));
      }
      lv_line_set_points(runs_[run], points_.data() + first, k - first);
      lv_obj_set_style_line_color(runs_[run], color_of(c), 0);
      lv_obj_clear_flag(runs_[run], LV_OBJ_FLAG_HIDDEN);
      run++;
      i = j;
    }
    hide_line(run);
    if (p.has_feed) {
      int n = 0;
      for (int b = 0; b < bars; b++) {
        if (std::isnan(v[b])) continue;
        feed_points_[n++] = point(ax_ + edge_x(b), y_of(base_[b]));
        feed_points_[n++] = point(ax_ + edge_x(b + 1), y_of(base_[b]));
      }
      lv_line_set_points(feed_line_, feed_points_.data(), n);
      lv_obj_set_style_line_color(feed_line_, ui::color(theme::MUTED), 0);
      lv_obj_clear_flag(feed_line_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(feed_line_, LV_OBJ_FLAG_HIDDEN);
    }
    const auto middle = [&](int b) { return ax_ + (edge_x(b) + edge_x(b + 1)) / 2; };
    if (now_bar >= 0 && now_bar < bars && !std::isnan(v[now_bar]) && now_bar != sel_)
      put_dot(now_dot_, middle(now_bar), y_of(v[now_bar]), ui::color(theme::ACCENT));
    else
      lv_obj_add_flag(now_dot_, LV_OBJ_FLAG_HIDDEN);
    // A tapped slot: a line down through it, and a dot on each price.
    if (sel_ >= 0 && sel_ < bars && !std::isnan(v[sel_])) {
      lv_obj_clear_flag(mark_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(mark_, 1, ah_);
      lv_obj_set_pos(mark_, middle(sel_), ay_);
      ui::set_fill(mark_, Tone::NORMAL, theme::INK);
      put_dot(sel_dot_, middle(sel_), y_of(v[sel_]), ui::color(theme::INK));
      if (p.has_feed) put_dot(feed_dot_, middle(sel_), y_of(base_[sel_]), ui::color(theme::MUTED));
      else lv_obj_add_flag(feed_dot_, LV_OBJ_FLAG_HIDDEN);
    } else {
      for (lv_obj_t *part : {mark_, sel_dot_, feed_dot_}) lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
  }

  static lv_obj_t *hairline(lv_obj_t *parent, theme::Role role) {
    lv_obj_t *line = ui::block(parent, role);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_size(line, 1, 1);
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    return line;
  }

  lv_obj_t *bars_[MAX_SLOTS]{}, *grid_[MAX_TICKS]{}, *price_[MAX_TICKS]{}, *marks_[MAX_TIMES]{}, *time_[MAX_TIMES]{};
  lv_obj_t *divider_{};
  // The stepped line: its runs, the feed-in line, the mark through a tapped slot and the dots, and their points.
  lv_obj_t *runs_[MAX_RUNS]{}, *feed_line_{}, *mark_{}, *now_dot_{}, *sel_dot_{}, *feed_dot_{};
  tessera::List<lv_point_precise_t> points_, feed_points_;
  bool line_ = false, fill_ = true, ahead_ = false;
  Grid grid_style_ = Grid::NONE;
  lv_point_precise_t grid_points_[MAX_TICKS][2]{};
  bool cents_ = false;
  int x_ = 0, ax_ = 0, ay_ = 0, aw_ = 1, ah_ = 1, label_w_ = 0, label_h_ = 0, time_y_ = 0, mark_y_ = 0, mark_h_ = 0;
  int bars_n_ = 0, times_n_ = 0, mid_x_ = 0;
  int group_ = 1, bars_now_ = 0, sel_ = -1, from_ = 0;  // from_: the slot of the first bar
  float value_[MAX_SLOTS]{}, base_[MAX_SLOTS]{};  // each bar's delivery price, and the price it stands on
};

// What a card takes over from the tile that opened it: the tile's options and its last prices.
struct Shared {
  bool cents = false;
  bool line = false, fill = true;  // the graph: a stepped line in place of bars, with a fill under it
  bool ahead = false;              // only the 24 hours from now
  Grid grid = Grid::NONE;
  int decimals = 3;
  Tariff tariff;
  Prices prices;
  // The built-in Nord Pool integration exposes summary sensors; its action supplies the missing interval series.
  std::string nordpool_config_entry;
  char market_area[4] = "", market_currency[8] = "";
  float market_today[MAX_DAY]{}, market_tomorrow[MAX_DAY]{};
  int market_today_count = 0, market_tomorrow_count = 0;
  uint32_t market_today_start = 0, market_tomorrow_start = 0;
  uint32_t market_today_request = 0, market_tomorrow_request = 0;
  uint32_t market_requested_ms = 0, market_requested_day = 0, market_generation = 0;
  char market_requested_area[4] = "", market_requested_currency[8] = "";
};

static bool market_area_for(const std::string &entity, char out[4]) {
  for (size_t i = 0; i + 3 < entity.size(); i++) {
    if (entity[i] == '_' && entity[i + 1] == 's' && entity[i + 2] == 'e' && entity[i + 3] >= '1' && entity[i + 3] <= '5') {
      snprintf(out, 4, "SE%c", entity[i + 3]);
      return true;
    }
  }
  out[0] = 0;
  return false;
}

static void market_currency_for(JsonObjectConst data, char out[8]) {
  const char *unit = data["attributes"]["unit_of_measurement"] | "";
  const char *slash = strchr(unit, '/');
  size_t n = slash ? static_cast<size_t>(slash - unit) : 0;
  if (!n || n >= 8) { out[0] = 0; return; }
  std::memcpy(out, unit, n);
  out[n] = 0;
}

static uint32_t ask_market_day(const tessera::Plugin *plugin, const Shared &shared, const char *date) {
  JsonDocument request;
  request["ask"] = "call_service:nordpool.get_prices_for_date";
  JsonObject data = request["data"].to<JsonObject>();
  data["config_entry"] = shared.nordpool_config_entry;
  data["date"] = date;
  JsonArray areas = data["areas"].to<JsonArray>();
  areas.add(shared.market_area);
  if (shared.market_currency[0]) data["currency"] = shared.market_currency;
  return tessera::send(plugin, request.as<JsonObjectConst>());
}

static bool same_local_day(uint32_t a, uint32_t b) {
  if (!a || !b) return false;
  const tessera::LocalTime x = tessera::local_time(a), y = tessera::local_time(b);
  return x.year == y.year && x.month == y.month && x.day == y.day;
}

static void market_date(uint32_t epoch, char out[12]) {
  const tessera::LocalTime t = tessera::local_time(epoch);
  snprintf(out, 12, "%04d-%02d-%02d", t.year, t.month, t.day);
}

static void apply_market_prices(const Shared &shared, uint32_t epoch, const Tariff &tariff, bool cents, Prices &p) {
  if (shared.market_today_count && same_local_day(shared.market_today_start, epoch) && p.today == 0) {
    p.today = shared.market_today_count;
    const float shown = cents ? 100.0f : 1.0f;
    for (int i = 0; i < p.today; i++) {
      p.slot[i] = tariff.delivery(shared.market_today[i]) * shown;
      p.feed[i] = tariff.feed_in(shared.market_today[i]) * shown;
    }
  }
  const uint32_t tomorrow = midnight_of(epoch) + 26 * 3600;
  const bool quarters = p.today >= 90 ||
                        (shared.market_today_count >= 90 && same_local_day(shared.market_today_start, epoch)) ||
                        (shared.market_tomorrow_count >= 90 && same_local_day(shared.market_tomorrow_start, tomorrow));
  // During Nord Pool's MTU transition, today can still be hourly while tomorrow is already quarter-hourly.
  // Keep both days at the finer resolution by repeating each hourly value across its four quarter slots.
  if (quarters && p.today > 0 && p.today < 90) {
    for (int i = p.today - 1; i >= 0; i--) {
      for (int q = 0; q < 4; q++) {
        p.slot[i * 4 + q] = p.slot[i];
        p.feed[i * 4 + q] = p.feed[i];
      }
    }
    p.today *= 4;
  }
  if (shared.market_tomorrow_count && same_local_day(shared.market_tomorrow_start, tomorrow) && p.today > 0 && p.tomorrow == 0) {
    float day[MAX_DAY], sales[MAX_DAY];
    const float shown = cents ? 100.0f : 1.0f;
    const int n = std::min(shared.market_tomorrow_count, MAX_DAY);
    for (int i = 0; i < n; i++) {
      day[i] = tariff.delivery(shared.market_tomorrow[i]) * shown;
      sales[i] = tariff.feed_in(shared.market_tomorrow[i]) * shown;
    }
    const int resolved = resolve(day, n, quarters);
    const int sales_resolved = resolve(sales, n, quarters);
    p.tomorrow = std::min(std::min(resolved, sales_resolved), MAX_DAY);
    for (int i = 0; i < p.tomorrow; i++) {
      p.slot[p.today + i] = day[i];
      p.feed[p.today + i] = sales[i];
    }
  }
  p.has_feed = tariff.feed && p.state_ok;
  float lo = INFINITY, hi = -INFINITY, sum = 0;
  int count = 0;
  p.lo_slot = p.hi_slot = 0;
  for (int i = 0; i < p.today; i++) {
    const float value = p.slot[i];
    if (std::isnan(value)) continue;
    if (value < lo) { lo = value; p.lo_slot = i; }
    if (value > hi) { hi = value; p.hi_slot = i; }
    sum += value;
    count++;
  }
  p.lo = count ? lo : 0;
  p.hi = count ? hi : 0;
  p.avg = count ? sum / count : 0;
}

// What the top bar shows: the prices of one tile on the screen, with its formula, unit and decimals. The first tile
// that has a sensor gives them, and keeps giving them while it lives; the prices stay when it goes (the CYD makes a
// new tile when its page comes back), until the next tile gives its own.
struct BarSource {
  const void *tile = nullptr;  // the tile that gives them; nullptr: none now
  bool set = false;            // a tile gave prices
  bool cents = false, error = false;
  int decimals = 3;
  Prices prices;
};

class PriceTile : public tessera::Tile {
 public:
  PriceTile(const tessera::Plugin *plugin, Shared *shared, BarSource *bar) : plugin_(plugin), shared_(shared), bar_(bar) {}
  ~PriceTile() override {
    if (bar_->tile == this) bar_->tile = nullptr;
  }

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    entity_ = c.entity;
    cents_ = strcmp(c.options["unit"] | "eur", "cents") == 0;
    decimals_ = std::clamp<int>(static_cast<int>(std::lround(c.options["fixed"] | 2.0f)), 0, 4);
    tariff_ = tariff_of(c.options);
    line_ = strcmp(c.options["graph"] | "bars", "line") == 0;
    fill_ = c.options["fill"] | true;
    grid_ = grid_of(c.options["grid"] | "none");
    ahead_ = c.options["ahead"] | false;
    head_ = ui::line_height(Font::HEADLINE);
    const int gap = ui::px(6);
    const int axis = ui::line_height(Font::BODY) + ui::px(ui::large() ? 8 : 4);
    chart_on_ = c.columns >= 2 && c.rows >= 2 && height_ - head_ - gap >= axis + ui::px(36);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    name_text_ = c.name ? c.name : "";
    name_ = ui::label(c.parent, Font::TITLE, theme::MUTED);
    feed_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(feed_, LV_TEXT_ALIGN_RIGHT, 0);
    if (chart_on_) {
      chart_.create(c.parent, 0, head_ + gap, width_, height_ - head_ - gap, TILE_BARS, 12, cents_, line_, fill_, grid_, ahead_);
      return;
    }
    // The meter's bar of cells (Tessera's sensor meter): narrow cells along the bottom of the tile, lit up to where
    // the price stands between the day's lowest and highest.
    cells_h_ = std::clamp(height_ * 20 / 100, ui::px(10), ui::px(26));
    const int cell = std::clamp(cells_h_ / 12, std::max(2, ui::px(3)), std::max(2, ui::px(5)));
    cell_gap_ = std::max(1, cell * 3 / 5);
    cells_n_ = std::clamp((width_ + cell_gap_) / (cell + cell_gap_), 10, MAX_CELLS);
    for (int i = 0; i < cells_n_; i++) {
      cells_[i] = ui::block(c.parent, theme::TRACK);
      lv_obj_set_style_radius(cells_[i], 0, 0);
      const int a = static_cast<int>(static_cast<long>(width_) * i / cells_n_);
      const int e = static_cast<int>(static_cast<long>(width_) * (i + 1) / cells_n_) - cell_gap_;
      lv_obj_set_size(cells_[i], std::max(1, e - a), cells_h_);
      lv_obj_set_pos(cells_[i], a, height_ - cells_h_);
    }
  }

  void on_state(JsonObjectConst data) override {
    read_prices(data, cents_, tariff_, prices_);
    market_area_for(entity_, shared_->market_area);
    market_currency_for(data, shared_->market_currency);
    maybe_request_market();
    apply_market_prices(*shared_, tessera::epoch(), tariff_, cents_, prices_);
    if (prices_.chosen && (bar_->tile == nullptr || bar_->tile == this)) {
      bar_->tile = this;
      bar_->set = true;
      bar_->cents = cents_;
      bar_->error = tariff_.error;
      bar_->decimals = decimals_;
      bar_->prices = prices_;
    }
  }

  void on_tick(uint32_t epoch) override {
    maybe_request_market();
    if (market_generation_ != shared_->market_generation) {
      apply_market_prices(*shared_, epoch, tariff_, cents_, prices_);
      market_generation_ = shared_->market_generation;
      if (prices_.chosen && (bar_->tile == nullptr || bar_->tile == this)) {
        bar_->tile = this;
        bar_->set = true;
        bar_->cents = cents_;
        bar_->error = tariff_.error;
        bar_->decimals = decimals_;
        bar_->prices = prices_;
      }
    }
    price_now(prices_, epoch);
    std::string big, small;
    if (!prices_.chosen) {
      big = plugin_->text("choose");
    } else if (tariff_.error) {
      big = plugin_->text("formula_error");
    } else if (!prices_.has_current) {
      big = plugin_->text("unavailable");
    } else {
      big = number(prices_.current, decimals_);
      small = unit_text(prices_, cents_);
    }
    const bool ok = prices_.chosen && prices_.has_current;
    std::string name = name_text_;
    // A tapped bar shows its own price, and its time in place of the name, for a few seconds.
    if (sel_ >= 0 && (!ok || !chart_on_ || epoch >= sel_until_)) { sel_ = -1; chart_.select(-1); }
    const Day day = day_of(prices_, epoch);
    if (ahead_) range_ahead(prices_, day, epoch);
    if (sel_ >= 0) {
      big = number(chart_.bar_value(sel_), decimals_);
      name = slot_times(day);
      // With a feed-in price, the bar's own after its time: "18:00–18:15 · Feed-in 0.250", where it fits beside
      // the price (at most as large as the headline face the row takes).
      if (prices_.has_feed) {
        const std::string with = name + " \u00b7 " +
                                 tessera::fill(plugin_->text("feed_in"), "value", number(chart_.bar_feed(sel_), decimals_));
        const int room = width_ - ui::text_width(small.empty() ? big : big + " " + small, Font::HEADLINE) - ui::px(8);
        if (ui::text_width(with, Font::BODY) <= room) name = with;
      }
    }
    const int color_slot = sel_ >= 0 ? chart_.bar_from(sel_) : day.slot(epoch);
    if (ok && color_slot >= 0 && (sel_ >= 0 || color_slot < prices_.count()))
      lv_obj_set_style_text_color(value_, price_color(day_percentile(prices_, color_slot,
                                      sel_ >= 0 ? chart_.bar_value(sel_) : prices_.current)), 0);
    else
      ui::set_color(value_, theme::INK);
    // A message ("Choose a price sensor") that does not fit beside the name takes the whole row.
    if (!ok && ui::text_width(big, Font::BODY_LARGE) > width_ * 60 / 100) name.clear();
    if (chart_on_) {
      // The name at the left and the price with its unit at the right, over the graph.
      place_row(big, small, name, head_);
      ui::set_text(feed_, "");
      chart_.update(ok ? prices_ : NO_PRICES, day, epoch);
    } else {
      // The meter's short card: the name at the left and the price with its unit at the right, in one row, the
      // cells under them. The feed-in price, when there is one, has a line of its own under the price where the
      // tile has room for it.
      int room = height_ - cells_h_ - ui::px(6);
      const int fh = ui::line_height(Font::BODY);
      const bool feed = ok && prices_.has_feed && room - fh >= ui::line_height(Font::BODY_LARGE);
      if (feed) room -= fh;
      place_row(big, small, name, room);
      ui::set_text(feed_, feed ? feed_text(prices_.feed_current, width_) : "");
      lv_obj_set_size(feed_, std::max(1, width_ - 2), fh);
      lv_obj_set_pos(feed_, 0, room);
      ui::set_color(feed_, theme::MUTED);
      // Each cell wears the colour of its place in the range, as the graph's bars do; the cells past the price are
      // a dim tint of theirs.
      const bool known = ok && prices_.today > 0 && prices_.hi > prices_.lo;
      const float share = known ? std::clamp((prices_.current - prices_.lo) / (prices_.hi - prices_.lo), 0.0f, 1.0f) : 0.0f;
      const int lit = static_cast<int>(std::lround(share * cells_n_));
      for (int i = 0; i < cells_n_; i++) {
        const float at = (i + 0.5f) / cells_n_;
        if (!known) {
          ui::set_fill(cells_[i], Tone::NORMAL, theme::TRACK);
          lv_obj_set_style_bg_opa(cells_[i], LV_OPA_COVER, 0);
          continue;
        }
        lv_obj_set_style_bg_color(cells_[i], price_color(at), 0);
        lv_obj_set_style_bg_opa(cells_[i], i < lit ? LV_OPA_COVER : LV_OPA_30, 0);
      }
    }
    last_epoch_ = epoch;
  }

  void on_theme() override {
    ui::set_color(value_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    on_tick(last_epoch_);
  }

  void on_tap() override {
    if (chart_on_ && prices_.today > 0) {
      const tessera::Point p = tessera::tap_point();
      const int hit = p.x >= 0 ? chart_.bar_at(p.x, p.y) : -1;
      // A tap on a bar shows it; a tap on the one shown, or beside the bars, opens the card.
      if (hit >= 0 && hit != sel_) {
        sel_ = hit;
        sel_until_ = last_epoch_ + 8;
        chart_.select(hit);
        on_tick(last_epoch_);
        return;
      }
    }
    shared_->cents = cents_;
    shared_->decimals = decimals_;
    shared_->tariff = tariff_;
    shared_->line = line_;
    shared_->fill = fill_;
    shared_->grid = grid_;
    shared_->ahead = ahead_;
    shared_->prices = prices_;
    tessera::open_card(plugin_->plugin_id(), "prices", entity_, tile_);
  }

 private:
  void maybe_request_market() {
    if (shared_->nordpool_config_entry.empty() || !shared_->market_area[0] || !tessera::epoch()) return;
    const uint32_t now_ms = millis();
    const uint32_t now = tessera::epoch();
    const uint32_t day_start = midnight_of(now);
    if (shared_->market_requested_ms && now_ms - shared_->market_requested_ms < 3600000 &&
        shared_->market_requested_day == day_start &&
        std::strcmp(shared_->market_requested_area, shared_->market_area) == 0 &&
        std::strcmp(shared_->market_requested_currency, shared_->market_currency) == 0) return;
    char today[12], tomorrow[12];
    market_date(day_start, today);
    market_date(day_start + 26 * 3600, tomorrow);
    shared_->market_today_request = ask_market_day(plugin_, *shared_, today);
    shared_->market_tomorrow_request = ask_market_day(plugin_, *shared_, tomorrow);
    shared_->market_requested_ms = now_ms;
    shared_->market_requested_day = day_start;
    std::snprintf(shared_->market_requested_area, sizeof(shared_->market_requested_area), "%s", shared_->market_area);
    std::snprintf(shared_->market_requested_currency, sizeof(shared_->market_requested_currency), "%s", shared_->market_currency);
  }

  // The name at the left and the price with its unit at the right in one row, `room` high; without a name the price
  // has the whole row.
  void place_row(const std::string &big, const std::string &small, const std::string &name, int room) {
    const std::string words = small.empty() ? big : big + " " + small;
    // The name keeps the tile-label face as the meter bar writes it; the price takes the largest face that leaves the
    // name its room, and only a name that fits no face steps down.
    const int apart = ui::px(8), most = name.empty() ? width_ - 2 : width_ * 60 / 100;
    Font face = Font::BODY, nface = Font::BODY;
    bool settled = false;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY}) {
      if (ui::text_width(words, f) > most || ui::line_height(f) > room) continue;
      if (name.empty() || ui::text_width(name, Font::TITLE) <= width_ - ui::text_width(words, f) - apart) {
        face = f;
        settled = true;
        break;
      }
    }
    if (!settled)
      for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY})
        if (ui::text_width(words, f) <= most && ui::line_height(f) <= room) { face = f; break; }
    const int vh = ui::line_height(face);
    const int vw = std::min(most, ui::text_width(words, face));
    for (Font f : {Font::TITLE, Font::BODY_LARGE, Font::BODY})
      if (ui::text_width(name, f) <= width_ - vw - apart) { nface = f; break; }
    ui::set_font(value_, face);
    ui::set_text(value_, words);
    ui::set_text(unit_, "");
    lv_obj_set_size(value_, std::max(1, vw + 2), vh);
    lv_obj_set_pos(value_, width_ - vw - 2, (room - vh) / 2);
    ui::set_font(name_, nface);
    ui::set_text(name_, name);
    const int nh = ui::line_height(nface);
    lv_obj_set_size(name_, std::max(1, width_ - vw - ui::px(8)), nh);
    lv_obj_set_pos(name_, 0, (room - vh) / 2 + (vh - nh) * 8 / 10);
    ui::set_color(name_, theme::MUTED);
  }

  // "18:00–18:15" of the shown bar.
  std::string slot_times(const Day &day) const {
    if (!day.known()) return "";
    const std::string a = tessera::clock_text(day.at(chart_.bar_from(sel_)));
    const std::string b = tessera::clock_text(day.at(chart_.bar_to(sel_, prices_.count())));
    return a + (ui::covers("\u2013", Font::BODY) ? "\u2013" : "-") + b;
  }

  // "Feed-in 0.120 €/kWh"; without the unit when that does not fit in `room`.
  std::string feed_text(float value, int room) const {
    const std::string n = number(value, decimals_), unit = unit_text(prices_, cents_);
    const std::string both = tessera::fill(plugin_->text("feed_in"), "value", unit.empty() ? n : n + " " + unit);
    return ui::text_width(both, Font::BODY) <= room ? both : tessera::fill(plugin_->text("feed_in"), "value", n);
  }

  const tessera::Plugin *plugin_;
  Shared *shared_;
  BarSource *bar_;
  int sel_ = -1;
  uint32_t sel_until_ = 0;
  std::string entity_, name_text_;
  Prices prices_;
  Chart chart_;
  Tariff tariff_;
  bool cents_ = false, chart_on_ = false, line_ = false, fill_ = true, ahead_ = false;
  Grid grid_ = Grid::NONE;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, tile_ = -1;
  int cells_n_ = 0, cells_h_ = 0, cell_gap_ = 1;
  uint32_t market_generation_ = 0;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *name_{}, *feed_{}, *cells_[MAX_CELLS]{};
};

// ---- The card: the price now, the day's lowest, highest and average, and the graph of today and tomorrow ----
class PriceCard : public tessera::Card {
 public:
  PriceCard(const tessera::Plugin *plugin, Shared *shared) : plugin_(plugin), shared_(shared) {}

  void open(const tessera::CardContext &c) override {
    width_ = c.width;
    height_ = c.height;
    prices_ = shared_->prices;
    cents_ = shared_->cents;
    decimals_ = shared_->decimals;
    tariff_ = shared_->tariff;
    ahead_ = shared_->ahead;
    const int gap = ui::px(8);
    const int text_h = ui::line_height(Font::BODY_LARGE);
    row_h_ = std::max(ui::line_height(Font::VALUE), 2 * text_h);
    // The feed-in price of now, when the tariff has one: a line under the header's row, at the left under the price.
    head_ = tariff_.feed ? row_h_ + text_h : row_h_;
    feed_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_size(feed_, width_, text_h);
    lv_obj_set_pos(feed_, 0, row_h_);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    // The header as the history card has it: the price at the left, the average in the middle, and at the right the
    // highest and the lowest with the time slot they are in.
    // The words are the size of the history card's: the font of the words on a key.
    left_w_ = width_ * 32 / 100;
    right_x_ = width_ * 54 / 100;
    avg_cap_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_size(avg_cap_, right_x_ - left_w_, text_h);
    lv_obj_set_style_text_align(avg_cap_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(avg_cap_, left_w_, (row_h_ - 2 * text_h) / 2);
    avg_ = ui::label(c.parent, Font::BODY_LARGE, theme::INK);
    lv_obj_set_size(avg_, right_x_ - left_w_, text_h);
    lv_obj_set_style_text_align(avg_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(avg_, left_w_, (row_h_ - 2 * text_h) / 2 + text_h);
    for (int i = 0; i < 2; i++) {
      extreme_[i] = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
      lv_obj_set_size(extreme_[i], width_ - right_x_, text_h);
      lv_obj_set_style_text_align(extreme_[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(extreme_[i], right_x_, (row_h_ - 2 * text_h) / 2 + i * text_h);
    }
    int y = head_ + gap;
    // The graph on a card of its own, as the history graph sits on one: a day label row, then the plot.
    const bool big = ui::large();
    const int edge = ui::px(big ? 14 : 8), days_h = ui::line_height(Font::BODY);
    const int card_h = std::max(ui::px(60), height_ - y - ui::px(4));
    surface_ = ui::block(c.parent, theme::CARD);
    lv_obj_set_size(surface_, width_, card_h);
    lv_obj_set_pos(surface_, 0, y);
    lv_obj_set_style_radius(surface_, ui::px(big ? 16 : 10), 0);
    lv_obj_set_style_border_width(surface_, 1, 0);
    const int top = y + ui::px(big ? 8 : 5);
    today_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_size(today_, width_ / 2, days_h);
    tomorrow_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_size(tomorrow_, width_ / 2, days_h);
    lv_obj_set_pos(today_, edge, top);
    const int plot_y = top + days_h + ui::px(big ? 8 : 4);
    chart_.create(c.parent, edge, plot_y, width_ - 2 * edge, y + card_h - ui::px(big ? 10 : 6) - plot_y, MAX_SLOTS, MAX_TIMES,
                  cents_, shared_->line, shared_->fill, shared_->grid, shared_->ahead);
    on_tick(0);
  }

  void on_state(JsonObjectConst data) override {
    read_prices(data, cents_, tariff_, prices_);
    apply_market_prices(*shared_, tessera::epoch(), tariff_, cents_, prices_);
    on_tick(last_epoch_);
  }

  void on_tick(uint32_t epoch) override {
    last_epoch_ = epoch;
    apply_market_prices(*shared_, epoch, tariff_, cents_, prices_);
    price_now(prices_, epoch);
    const bool ok = prices_.chosen && prices_.has_current;
    const bool graph = ok && prices_.today > 0;
    const Day day = day_of(prices_, epoch);
    const int gap = ui::px(8);
    // A tapped bar shows its own price and time slot for a few seconds, then the card is as it was.
    if (sel_ >= 0 && (!ok || prices_.today == 0 || epoch >= sel_until_)) { sel_ = -1; chart_.select(-1); }
    std::string big = ok                  ? number(prices_.current, decimals_)
                      : !prices_.chosen   ? plugin_->text("choose")
                      : tariff_.error     ? plugin_->text("formula_error")
                                          : plugin_->text("unavailable");
    const std::string small = ok ? unit_text(prices_, cents_) : "";
    if (graph && sel_ >= 0) big = number(chart_.bar_value(sel_), decimals_);
    // A message in place of the price has the whole header: the average, the highest and the lowest are empty then.
    const int room = ok ? left_w_ : width_;
    Font face = Font::BODY;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY})
      if (ui::text_width(big, f) + (ok ? gap + ui::text_width(small, Font::BODY) : 0) <= room) { face = f; break; }
    ui::set_font(value_, face);
    ui::set_text(value_, big);
    const int color_slot = graph && sel_ >= 0 ? chart_.bar_from(sel_) : day.slot(epoch);
    if (ok && color_slot >= 0 && color_slot < prices_.count())
      lv_obj_set_style_text_color(value_, price_color(day_percentile(prices_, color_slot,
                                      graph && sel_ >= 0 ? chart_.bar_value(sel_) : prices_.current)), 0);
    else
      ui::set_color(value_, theme::INK);
    ui::set_text(unit_, small);
    const int vh = ui::line_height(face), sh = ui::line_height(Font::BODY);
    const int vw = std::min(room, ui::text_width(big, face));
    lv_obj_set_size(value_, std::max(1, vw + 2), vh);
    lv_obj_set_pos(value_, 0, (row_h_ - vh) / 2);
    lv_obj_set_size(unit_, std::max(1, left_w_ - vw - gap), sh);
    lv_obj_set_pos(unit_, vw + gap, (row_h_ - vh) / 2 + vh - sh - ui::px(2));
    if (ahead_) range_ahead(prices_, day, epoch);
    // The feed-in price of now under the price; a tapped bar's own, with its time slot behind it, while it shows.
    std::string feed;
    if (ok && prices_.has_feed) {
      const bool tapped = graph && sel_ >= 0;
      feed = tessera::fill(plugin_->text("feed_in"), "value",
                           with_unit(tapped ? chart_.bar_feed(sel_) : prices_.feed_current, width_));
      if (tapped && day.known()) {
        const std::string with = feed + " \u00b7 " + tessera::clock_text(day.at(chart_.bar_from(sel_))) +
                                 (ui::covers("\u2013", Font::BODY_LARGE) ? "\u2013" : "-") +
                                 tessera::clock_text(day.at(chart_.bar_to(sel_, prices_.count())));
        if (ui::text_width(with, Font::BODY_LARGE) <= width_) feed = with;
      }
    }
    ui::set_text(feed_, feed);
    ui::set_color(feed_, theme::MUTED);
    if (graph && sel_ >= 0) {
      // Keep the selected slot's time range visible beneath its price in the card headline.
      ui::set_text(avg_cap_, slot_times(day));
      ui::set_text(avg_, with_unit(chart_.bar_value(sel_), right_x_ - left_w_));
    } else {
      ui::set_text(avg_cap_, graph ? plugin_->text("average") : "");
      ui::set_text(avg_, graph ? with_unit(prices_.avg, right_x_ - left_w_) : "");
    }
    ui::set_text(extreme_[0], graph ? extreme_text("high", true, day, epoch) : "");
    ui::set_text(extreme_[1], graph ? extreme_text("low", false, day, epoch) : "");
    chart_.update(graph ? prices_ : NO_PRICES, day, epoch);
    ui::set_fill(surface_, Tone::NORMAL, theme::CARD);
    lv_obj_set_style_border_color(surface_, ui::color(theme::LINE), 0);
    ui::set_text(today_, plugin_->text(graph ? "today" : "no_graph"));
    ui::set_color(today_, theme::SUBTLE);
    const bool second = graph && prices_.tomorrow > 0 && chart_.midnight_x() > 0;
    ui::set_text(tomorrow_, second ? plugin_->text("tomorrow") : "");
    ui::set_color(tomorrow_, theme::SUBTLE);
    if (second) {
      const int tx = chart_.midnight_x() + ui::px(ui::large() ? 8 : 4);
      lv_obj_set_pos(tomorrow_, tx, lv_obj_get_y(today_));
      lv_obj_set_width(tomorrow_, std::max(1, width_ - tx));
    }
  }

  // A tap on a bar shows it; a tap on the one shown, or elsewhere, puts the card back.
  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    const int hit = p.x >= 0 && prices_.today > 0 ? chart_.bar_at(p.x, p.y) : -1;
    sel_ = hit >= 0 && hit != sel_ ? hit : -1;
    sel_until_ = last_epoch_ + 8;
    chart_.select(sel_);
    on_tick(last_epoch_);
  }

  void on_theme() override {
    ui::set_color(value_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    ui::set_color(feed_, theme::MUTED);
    ui::set_color(avg_cap_, theme::MUTED);
    ui::set_color(avg_, theme::INK);
    ui::set_color(extreme_[0], theme::MUTED);
    ui::set_color(extreme_[1], theme::MUTED);
    on_tick(last_epoch_);
  }

 private:
  // "18:00–18:15" of the shown bar; only the start when it does not fit in the middle.
  std::string slot_times(const Day &day) const {
    if (!day.known()) return "";
    const std::string a = tessera::clock_text(day.at(chart_.bar_from(sel_)));
    const std::string b = tessera::clock_text(day.at(chart_.bar_to(sel_, prices_.count())));
    const std::string both = a + (ui::covers("\u2013", Font::BODY_LARGE) ? "\u2013" : "-") + b;
    return ui::text_width(both, Font::BODY_LARGE) <= right_x_ - left_w_ ? both : a;
  }

  // "High 0.381 · 18:00–18:15": the price and the time slot it is in; only the start when the whole does not fit.
  // The price with its unit behind it; the price alone when both do not fit in `room`.
  std::string with_unit(float value, int room) const {
    const std::string n = number(value, decimals_), unit = unit_text(prices_, cents_);
    const std::string both = unit.empty() ? n : n + " " + unit;
    return ui::text_width(both, Font::BODY_LARGE) <= room ? both : n;
  }

  // The highest or lowest price from the slot of now on (today's rest and tomorrow, or the next 24 hours); the clock
  // not set: all of today.
  bool extreme(bool high, int now, const Day &day, uint32_t epoch, float &value, int &slot) const {
    int from = 0, to = prices_.count();
    if (ahead_) window_of(prices_, day, epoch, from, to);
    bool found = false;
    for (int s = std::max(now, 0); s < to; s++) {
      const float v = prices_.slot[s];
      if (std::isnan(v) || (found && (high ? v <= value : v >= value))) continue;
      value = v;
      slot = s;
      found = true;
    }
    return found;
  }

  std::string extreme_text(const char *key, bool high, const Day &day, uint32_t epoch) const {
    float value = 0;
    int slot = 0;
    const int now = day.slot(epoch);
    if (!extreme(high, now, day, epoch, value, slot)) return "";
    std::string words = tessera::fill(plugin_->text(key), "value", with_unit(value, width_));
    if (ui::text_width(words, Font::BODY_LARGE) > width_ - right_x_)
      words = tessera::fill(plugin_->text(key), "value", number(value, decimals_));
    if (!day.known()) return words;
    const std::string when = slot >= prices_.today ? std::string(plugin_->text("tomorrow")) + " " : std::string();
    const std::string from = when + tessera::clock_text(day.at(slot));
    const std::string dash = ui::covers("\u2013", Font::BODY_LARGE) ? "\u2013" : "-";
    const std::string both = words + " \u00b7 " + from + dash + tessera::clock_text(day.at(slot + 1));
    if (ui::text_width(both, Font::BODY_LARGE) <= width_ - right_x_) return both;
    const std::string start = words + " \u00b7 " + from;
    return ui::text_width(start, Font::BODY_LARGE) <= width_ - right_x_ ? start : words;
  }

  const tessera::Plugin *plugin_;
  Shared *shared_;
  Prices prices_;
  Chart chart_;
  Tariff tariff_;
  bool cents_ = false, ahead_ = false;
  int sel_ = -1;
  uint32_t sel_until_ = 0;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, row_h_ = 0, left_w_ = 0, right_x_ = 0;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *feed_{}, *avg_cap_{}, *avg_{}, *extreme_[2]{}, *today_{}, *tomorrow_{}, *surface_{};
};

void ElectricityPrice::on_message(JsonObjectConst message) {
  const uint32_t re = message["re"] | 0u;
  if (!re) return;
  const bool today = re == shared_->market_today_request;
  const bool tomorrow = re == shared_->market_tomorrow_request;
  if (!today && !tomorrow) return;
  if (!(message["ok"] | false)) {
    if (today) shared_->market_today_request = 0;
    if (tomorrow) shared_->market_tomorrow_request = 0;
    return;
  }
  JsonObjectConst result = message["result"];
  JsonArrayConst prices = result["prices"].as<JsonArrayConst>();
  const uint32_t start = result["start"] | 0u;
  if (!prices.size() || !start) return;
  if (today) {
    shared_->market_today_count = read_span(prices, 0, static_cast<int>(prices.size()), 0.001f, shared_->market_today);
    shared_->market_today_start = start;
    shared_->market_today_request = 0;
  } else {
    shared_->market_tomorrow_count = read_span(prices, 0, static_cast<int>(prices.size()), 0.001f, shared_->market_tomorrow);
    shared_->market_tomorrow_start = start;
    shared_->market_tomorrow_request = 0;
  }
  shared_->market_generation++;
}

void ElectricityPrice::setup() {
  shared_ = new Shared();
  shared_->nordpool_config_entry = nordpool_config_entry_;
  bar_ = new BarSource();
  // A tile per integration: the inspector lists only sensors that have the attributes of that one.
  add_tile("nordpool", [this]() { return new PriceTile(this, shared_, bar_); });
  add_tile("entsoe", [this]() { return new PriceTile(this, shared_, bar_); });
  add_tile("gespot", [this]() { return new PriceTile(this, shared_, bar_); });
  add_tile("epex", [this]() { return new PriceTile(this, shared_, bar_); });
  add_card("prices", [this]() { return new PriceCard(this, shared_); }, true);
  add_bar_item("price", [this]() { return bar(); });
}

// The price now in the top bar, as a tile shows it: the prices of the tile that gives them (BarSource), its formula,
// unit and decimals, at the slot of now. Nothing shows until a tile has a price; a formula with a typo says so.
tessera::BarItem ElectricityPrice::bar() {
  tessera::BarItem item;
  if (!bar_->set) return item;
  if (bar_->error) {
    item.shown = true;
    item.icon = 0xF0241;  // flash
    item.text = text("formula_error");
    return item;
  }
  Prices &p = bar_->prices;
  price_now(p, tessera::epoch());
  if (!p.has_current) return item;
  item.shown = true;
  item.icon = 0xF0241;
  item.text = number(p.current, bar_->decimals) + " " + unit_text(p, bar_->cents);
  return item;
}

}  // namespace esphome::tessa_electricity_se

