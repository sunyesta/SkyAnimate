#include "anim/expression.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace oanim {

namespace {

enum Code : uint8_t {
  NUM,
  VAR,
  ADD,
  SUB,
  MUL,
  DIV,
  MOD,
  POW,
  NEG,
  LT,
  LE,
  GT,
  GE,
  EQ,
  NE,
  AND,
  OR,
  NOT,
  /* a, test, b on the stack: test ? a : b. */
  SELECT,
  /* A function: `index` is its number, `number` how many arguments. */
  CALL,
};

enum Func : int {
  F_SIN,
  F_COS,
  F_TAN,
  F_ASIN,
  F_ACOS,
  F_ATAN,
  F_ATAN2,
  F_SQRT,
  F_ABS,
  F_SIGN,
  F_FLOOR,
  F_CEIL,
  F_ROUND,
  F_FMOD,
  F_POW,
  F_EXP,
  F_LOG,
  F_MIN,
  F_MAX,
  F_CLAMP,
  F_LERP,
  F_SMOOTHSTEP,
  F_RADIANS,
  F_DEGREES,
};

struct FuncInfo {
  const char *name;
  Func func;
  /* Arguments: exactly `args`, or at least 1 when -1 (min, max). */
  int args;
};

constexpr FuncInfo FUNCS[] = {
    {"sin", F_SIN, 1},         {"cos", F_COS, 1},     {"tan", F_TAN, 1},       {"asin", F_ASIN, 1},
    {"acos", F_ACOS, 1},       {"atan", F_ATAN, 1},   {"atan2", F_ATAN2, 2},   {"sqrt", F_SQRT, 1},
    {"abs", F_ABS, 1},         {"sign", F_SIGN, 1},   {"floor", F_FLOOR, 1},   {"ceil", F_CEIL, 1},
    {"round", F_ROUND, 1},     {"fmod", F_FMOD, 2},   {"pow", F_POW, 2},       {"exp", F_EXP, 1},
    {"log", F_LOG, 1},         {"min", F_MIN, -1},    {"max", F_MAX, -1},      {"clamp", F_CLAMP, 3},
    {"lerp", F_LERP, 3},       {"smoothstep", F_SMOOTHSTEP, 3}, {"radians", F_RADIANS, 1},
    {"degrees", F_DEGREES, 1},
};

struct Parser {
  const std::string &text;
  size_t at = 0;
  Expression &out;
  std::string error;

  void space()
  {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) {
      at++;
    }
  }
  bool fail(const std::string &message)
  {
    if (error.empty()) {
      error = message;
    }
    return false;
  }
  /* The operator (or keyword) `s` next, taken when it is. */
  bool eat(const char *s)
  {
    space();
    const size_t n = std::char_traits<char>::length(s);
    if (text.compare(at, n, s) != 0) {
      return false;
    }
    /* A keyword isn't the start of a longer name ("order"). */
    if (std::isalpha(static_cast<unsigned char>(s[0])) && at + n < text.size() &&
        (std::isalnum(static_cast<unsigned char>(text[at + n])) || text[at + n] == '_'))
    {
      return false;
    }
    at += n;
    return true;
  }
  void emit(uint8_t code, double number = 0.0, int index = 0) { out.ops.push_back({code, number, index}); }

  bool ternary()
  {
    if (!or_expr()) {
      return false;
    }
    if (eat("if")) {
      if (!or_expr()) {
        return false;
      }
      if (!eat("else")) {
        return fail("Expected 'else'");
      }
      if (!ternary()) {
        return false;
      }
      emit(SELECT);
    }
    return true;
  }
  bool or_expr()
  {
    if (!and_expr()) {
      return false;
    }
    while (eat("or")) {
      if (!and_expr()) {
        return false;
      }
      emit(OR);
    }
    return true;
  }
  bool and_expr()
  {
    if (!not_expr()) {
      return false;
    }
    while (eat("and")) {
      if (!not_expr()) {
        return false;
      }
      emit(AND);
    }
    return true;
  }
  bool not_expr()
  {
    if (eat("not")) {
      if (!not_expr()) {
        return false;
      }
      emit(NOT);
      return true;
    }
    return comparison();
  }
  bool comparison()
  {
    if (!sum()) {
      return false;
    }
    for (;;) {
      uint8_t code;
      if (eat("<=")) {
        code = LE;
      }
      else if (eat(">=")) {
        code = GE;
      }
      else if (eat("==")) {
        code = EQ;
      }
      else if (eat("!=")) {
        code = NE;
      }
      else if (eat("<")) {
        code = LT;
      }
      else if (eat(">")) {
        code = GT;
      }
      else {
        return true;
      }
      if (!sum()) {
        return false;
      }
      emit(code);
    }
  }
  bool sum()
  {
    if (!term()) {
      return false;
    }
    for (;;) {
      uint8_t code;
      if (eat("+")) {
        code = ADD;
      }
      else if (eat("-")) {
        code = SUB;
      }
      else {
        return true;
      }
      if (!term()) {
        return false;
      }
      emit(code);
    }
  }
  bool term()
  {
    if (!unary()) {
      return false;
    }
    for (;;) {
      uint8_t code;
      space();
      if (text.compare(at, 2, "**") == 0) {
        return true;
      }
      if (eat("*")) {
        code = MUL;
      }
      else if (eat("/")) {
        code = DIV;
      }
      else if (eat("%")) {
        code = MOD;
      }
      else {
        return true;
      }
      if (!unary()) {
        return false;
      }
      emit(code);
    }
  }
  bool unary()
  {
    if (eat("-")) {
      if (!unary()) {
        return false;
      }
      emit(NEG);
      return true;
    }
    if (eat("+")) {
      return unary();
    }
    return power();
  }
  bool power()
  {
    if (!primary()) {
      return false;
    }
    if (eat("**") || eat("^")) {
      /* Right to left, and binding tighter than a minus on its left: -2**2 is -4. */
      if (!unary()) {
        return false;
      }
      emit(POW);
    }
    return true;
  }
  bool primary()
  {
    space();
    if (at >= text.size()) {
      return fail("The expression ends too early");
    }
    const char c = text[at];
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      char *end = nullptr;
      const double v = std::strtod(text.c_str() + at, &end);
      if (end == text.c_str() + at) {
        return fail("Can't read a number at '" + text.substr(at, 8) + "'");
      }
      at = size_t(end - text.c_str());
      emit(NUM, v);
      return true;
    }
    if (eat("(")) {
      if (!ternary()) {
        return false;
      }
      if (!eat(")")) {
        return fail("Expected ')'");
      }
      return true;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const size_t start = at;
      while (at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_')) {
        at++;
      }
      const std::string name = text.substr(start, at - start);
      space();
      if (at < text.size() && text[at] == '(') {
        at++;
        const FuncInfo *info = nullptr;
        for (const FuncInfo &f : FUNCS) {
          if (name == f.name) {
            info = &f;
          }
        }
        if (!info) {
          return fail("Unknown function '" + name + "'");
        }
        int args = 0;
        if (!eat(")")) {
          do {
            if (!ternary()) {
              return false;
            }
            args++;
          } while (eat(","));
          if (!eat(")")) {
            return fail("Expected ')' after " + name + "'s arguments");
          }
        }
        if ((info->args >= 0 && args != info->args) || (info->args < 0 && args < 1)) {
          return fail(name + " takes " + (info->args < 0 ? std::string("1 or more") : std::to_string(info->args)) +
                      " argument" + (info->args == 1 ? "" : "s"));
        }
        emit(CALL, double(args), int(info->func));
        return true;
      }
      if (name == "pi") {
        emit(NUM, 3.14159265358979323846);
        return true;
      }
      if (name == "e") {
        emit(NUM, 2.71828182845904523536);
        return true;
      }
      if (name == "True" || name == "False") {
        emit(NUM, name == "True" ? 1.0 : 0.0);
        return true;
      }
      const auto it = std::find(out.names.begin(), out.names.end(), name);
      const int index = int(it - out.names.begin());
      if (it == out.names.end()) {
        out.names.push_back(name);
      }
      emit(VAR, 0.0, index);
      return true;
    }
    return fail(std::string("Unexpected '") + c + "'");
  }
};

}  // namespace

bool expression_compile(const std::string &text, Expression *r_expression, std::string *r_error)
{
  Expression e;
  Parser p{text, 0, e, {}};
  /* "#frame/10": the # that types an expression into a number field (Blender's), written here too. */
  p.space();
  if (p.at < text.size() && text[p.at] == '#') {
    p.at++;
  }
  bool ok = p.ternary();
  p.space();
  if (ok && p.at < text.size()) {
    ok = p.fail("Unexpected '" + text.substr(p.at, 8) + "'");
  }
  if (!ok) {
    *r_error = p.error.empty() ? "Can't read the expression" : p.error;
    return false;
  }
  *r_expression = std::move(e);
  r_error->clear();
  return true;
}

double expression_run(const Expression &expression, const std::vector<double> &values)
{
  double stack[64];
  int top = 0;
  const auto pop = [&]() { return top > 0 ? stack[--top] : 0.0; };
  const auto push = [&](double v) {
    if (top < 64) {
      stack[top++] = v;
    }
  };
  for (const Expression::Op &op : expression.ops) {
    switch (op.code) {
      case NUM: push(op.number); break;
      case VAR: push(op.index < int(values.size()) ? values[size_t(op.index)] : 0.0); break;
      case NEG: push(-pop()); break;
      case NOT: push(pop() == 0.0 ? 1.0 : 0.0); break;
      case SELECT: {
        const double b = pop(), test = pop(), a = pop();
        push(test != 0.0 ? a : b);
        break;
      }
      case CALL: {
        const int n = int(op.number);
        double args[8] = {0};
        double many_min = 0.0, many_max = 0.0;
        for (int i = n - 1; i >= 0; i--) {
          const double v = pop();
          if (i < 8) {
            args[i] = v;
          }
          many_min = i == n - 1 ? v : std::min(many_min, v);
          many_max = i == n - 1 ? v : std::max(many_max, v);
        }
        const double a = args[0], b = args[1], c = args[2];
        double r = 0.0;
        switch (Func(op.index)) {
          case F_SIN: r = std::sin(a); break;
          case F_COS: r = std::cos(a); break;
          case F_TAN: r = std::tan(a); break;
          case F_ASIN: r = std::asin(a); break;
          case F_ACOS: r = std::acos(a); break;
          case F_ATAN: r = std::atan(a); break;
          case F_ATAN2: r = std::atan2(a, b); break;
          case F_SQRT: r = std::sqrt(a); break;
          case F_ABS: r = std::abs(a); break;
          case F_SIGN: r = a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0); break;
          case F_FLOOR: r = std::floor(a); break;
          case F_CEIL: r = std::ceil(a); break;
          case F_ROUND: r = std::round(a); break;
          case F_FMOD: r = std::fmod(a, b); break;
          case F_POW: r = std::pow(a, b); break;
          case F_EXP: r = std::exp(a); break;
          case F_LOG: r = std::log(a); break;
          case F_MIN: r = many_min; break;
          case F_MAX: r = many_max; break;
          case F_CLAMP: r = std::clamp(a, std::min(b, c), std::max(b, c)); break;
          case F_LERP: r = a + (b - a) * c; break;
          case F_SMOOTHSTEP: {
            const double t = b != a ? std::clamp((c - a) / (b - a), 0.0, 1.0) : (c >= b ? 1.0 : 0.0);
            r = t * t * (3.0 - 2.0 * t);
            break;
          }
          case F_RADIANS: r = a * 3.14159265358979323846 / 180.0; break;
          case F_DEGREES: r = a * 180.0 / 3.14159265358979323846; break;
        }
        push(r);
        break;
      }
      default: {
        const double b = pop(), a = pop();
        double r = 0.0;
        switch (op.code) {
          case ADD: r = a + b; break;
          case SUB: r = a - b; break;
          case MUL: r = a * b; break;
          case DIV: r = a / b; break;
          /* Python's: the result takes the divisor's sign. */
          case MOD: r = b != 0.0 ? a - std::floor(a / b) * b : std::nan(""); break;
          case POW: r = std::pow(a, b); break;
          case LT: r = a < b; break;
          case LE: r = a <= b; break;
          case GT: r = a > b; break;
          case GE: r = a >= b; break;
          case EQ: r = a == b; break;
          case NE: r = a != b; break;
          case AND: r = (a != 0.0 && b != 0.0) ? 1.0 : 0.0; break;
          case OR: r = (a != 0.0 || b != 0.0) ? 1.0 : 0.0; break;
          default: break;
        }
        push(r);
        break;
      }
    }
  }
  return top > 0 ? stack[top - 1] : 0.0;
}

}  // namespace oanim
