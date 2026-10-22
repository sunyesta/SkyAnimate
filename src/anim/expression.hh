#pragma once

/* Drivers' expressions: a small, fast language of its own (Blender's "simple expressions"), compiled
 * once to a stack program, so a file full of drivers costs next to nothing per frame and never runs
 * Python.
 *
 *   numbers       1  0.5  2e-3
 *   names         variables, then `frame`, `pi`, `e`, `True`, `False` (a variable may take one of
 *                 their names; the caller resolves them, see Expression::names)
 *   operators     + - * / % ** (or ^), unary -, comparisons < <= > >= == != (1 or 0),
 *                 chained as Python's (a < b < c), `and` `or` (Python's: the deciding operand) `not`,
 *                 and Python's `a if test else b`
 *   functions     sin cos tan asin acos atan atan2 sqrt abs sign floor ceil round fmod pow exp log
 *                 min max clamp(x, lo, hi) lerp(a, b, t) smoothstep(lo, hi, x) radians degrees
 *
 * Values are in their own units: a layer's rotation in degrees, a bone's angle in radians. */

#include <string>
#include <vector>

namespace oanim {

struct Expression {
  /* The program: each op with its number or name index. */
  struct Op {
    uint8_t code = 0;
    double number = 0.0;
    int index = 0;
  };
  std::vector<Op> ops;
  /* The names it reads, in the order Op::index counts them. */
  std::vector<std::string> names;
};

/* Compiles `text`; false with `r_error` ("Unknown function 'sinn'", "Expected ')'"). */
bool expression_compile(const std::string &text, Expression *r_expression, std::string *r_error);
/* Runs it with `values[i]` for names[i]. NaN for a division by zero or the like is the caller's to
 * catch. */
double expression_run(const Expression &expression, const std::vector<double> &values);

}  // namespace oanim
