#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace esphome::tessa_electricity_se {

// A price formula the person types, in p, the sensor's price per kWh: numbers (with a decimal point or a comma), p,
// + - * /, brackets and a minus in front: "(p+0.01)*1.21", "0,25", "p-0.02". It is read once, into the order a small
// stack works it out in, and then costs nothing to work out for every slot. An empty formula is p itself.
class Formula {
 public:
  // Reads `text`; false when it is not a formula, which then stands for p as an empty one does.
  bool parse(const char *text) {
    n_ = 0;
    set_ = false;
    Reader r{this, text ? text : ""};
    r.skip();
    if (!*r.at) return true;
    r.sum();
    r.skip();
    if (!r.ok || *r.at) {
      n_ = 0;
      return false;
    }
    set_ = true;
    return true;
  }

  // Whether a formula was typed: false for an empty one and for one that is not a formula.
  bool set() const { return set_; }

  float operator()(float p) const {
    if (!set_) return p;
    float stack[MAX_STEPS];
    int k = 0;
    for (int i = 0; i < n_; i++) {
      const Step &s = steps_[i];
      switch (s.op) {
        case NUMBER: stack[k++] = s.value; break;
        case PRICE: stack[k++] = p; break;
        case NEGATE: stack[k - 1] = -stack[k - 1]; break;
        default: {
          const float b = stack[--k];
          float &a = stack[k - 1];
          a = s.op == ADD ? a + b : s.op == SUBTRACT ? a - b : s.op == MULTIPLY ? a * b : a / b;
        }
      }
    }
    return k == 1 ? stack[0] : NAN;
  }

 private:
  enum Op : uint8_t { NUMBER, PRICE, ADD, SUBTRACT, MULTIPLY, DIVIDE, NEGATE };
  struct Step {
    Op op;
    float value;
  };
  static constexpr int MAX_STEPS = 64;  // a text option holds 64 characters: never more steps than that
  static constexpr int MAX_DEPTH = 16;  // brackets and minus signs inside each other

  // Reads by recursive descent: a sum of products of factors, each factor a number, p, a minus or plus in front of a
  // factor, or a sum in brackets.
  struct Reader {
    Formula *f;
    const char *at;
    bool ok = true;
    int depth = 0;

    void skip() {
      while (*at == ' ') at++;
    }
    void emit(Op op, float value = 0) {
      if (f->n_ >= MAX_STEPS) {
        ok = false;
        return;
      }
      f->steps_[f->n_++] = {op, value};
    }
    void sum() {
      product();
      for (;;) {
        skip();
        const char c = *at;
        if (!ok || (c != '+' && c != '-')) return;
        at++;
        product();
        emit(c == '+' ? ADD : SUBTRACT);
      }
    }
    void product() {
      factor();
      for (;;) {
        skip();
        const char c = *at;
        if (!ok || (c != '*' && c != '/')) return;
        at++;
        factor();
        emit(c == '*' ? MULTIPLY : DIVIDE);
      }
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    void factor() {
      skip();
      if (!ok || ++depth > MAX_DEPTH) {
        ok = false;
        return;
      }
      const char c = *at;
      if (c == '-' || c == '+') {
        at++;
        factor();
        if (c == '-') emit(NEGATE);
      } else if (c == '(') {
        at++;
        sum();
        skip();
        if (*at == ')') at++;
        else ok = false;
      } else if (c == 'p' || c == 'P') {
        at++;
        emit(PRICE);
      } else if (digit(c) || ((c == '.' || c == ',') && digit(at[1]))) {
        // A decimal comma is a point: "0,01" as people in the Netherlands write it.
        char buf[24];
        int k = 0;
        bool point = false;
        while (k < static_cast<int>(sizeof(buf)) - 1 && (digit(*at) || (!point && (*at == '.' || *at == ',')))) {
          if (*at == '.' || *at == ',') {
            point = true;
            buf[k++] = '.';
          } else {
            buf[k++] = *at;
          }
          at++;
        }
        buf[k] = 0;
        emit(NUMBER, strtof(buf, nullptr));
      } else {
        ok = false;
      }
      depth--;
    }
  };

  Step steps_[MAX_STEPS]{};
  int n_ = 0;
  bool set_ = false;
};

}  // namespace esphome::tessa_electricity_se

