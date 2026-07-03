// ============================================================================
// LiteQuery — eval.cpp
// Scalar expression evaluator implementation.
// ============================================================================

#include "eval.h"

#include <cmath>
#include <cctype>

namespace lq {

using namespace ast;

namespace {

// ---- Numeric helpers -------------------------------------------------------

bool isNumeric(const Value& v) {
    return typeIsNumeric(v.typeId());
}

// Are both operands (non-null) integers? Then integer arithmetic preserves type.
bool bothIntegers(const Value& a, const Value& b) {
    return typeIsInteger(a.typeId()) && typeIsInteger(b.typeId());
}

// ---- Comparison ------------------------------------------------------------
// Returns -1/0/1, or 2 if either side is NULL (SQL "unknown").
int compareValues(const Value& a, const Value& b) {
    if (a.isNull() || b.isNull()) return 2;

    // Numeric cross-type comparison via double (int64 fits within the exact
    // range we care about for the MVP; decimal treated as double).
    if (isNumeric(a) && isNumeric(b)) {
        if (bothIntegers(a, b)) {
            int64_t x = a.toInt64(), y = b.toInt64();
            return (x < y) ? -1 : (x > y ? 1 : 0);
        }
        double x = a.toDouble(), y = b.toDouble();
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    // Boolean
    if (a.typeId() == TypeId::BOOLEAN && b.typeId() == TypeId::BOOLEAN) {
        bool x = a.getBool(), y = b.getBool();
        return (x == y) ? 0 : (!x ? -1 : 1);
    }

    // String
    if (a.typeId() == TypeId::VARCHAR && b.typeId() == TypeId::VARCHAR) {
        int c = a.getString().compare(b.getString());
        return (c < 0) ? -1 : (c > 0 ? 1 : 0);
    }

    throw EvalError("cannot compare values of incompatible types");
}

// ---- Boolean coercion for logical operators --------------------------------
// Yields 0 (false), 1 (true), or 2 (unknown/NULL).
int truthiness(const Value& v) {
    if (v.isNull()) return 2;
    if (v.typeId() == TypeId::BOOLEAN) return v.getBool() ? 1 : 0;
    if (typeIsInteger(v.typeId()))     return v.toInt64() != 0 ? 1 : 0;
    if (typeIsFloat(v.typeId()))       return v.toDouble() != 0.0 ? 1 : 0;
    throw EvalError("expression is not boolean-convertible");
}

Value boolValue(int t) {                      // 2 (unknown) → NULL
    return (t == 2) ? Value::null() : Value(t == 1);
}

// ---- Arithmetic ------------------------------------------------------------

Value evalArithmetic(BinaryOp op, const Value& l, const Value& r) {
    if (l.isNull() || r.isNull()) return Value::null();
    if (!isNumeric(l) || !isNumeric(r))
        throw EvalError("arithmetic on non-numeric value");

    if (bothIntegers(l, r) && op != BinaryOp::DIV) {
        int64_t a = l.toInt64(), b = r.toInt64();
        switch (op) {
            case BinaryOp::ADD: return Value(a + b);
            case BinaryOp::SUB: return Value(a - b);
            case BinaryOp::MUL: return Value(a * b);
            case BinaryOp::MOD: return b == 0 ? Value::null() : Value(a % b);
            default: break;
        }
    }
    double a = l.toDouble(), b = r.toDouble();
    switch (op) {
        case BinaryOp::ADD: return Value(a + b);
        case BinaryOp::SUB: return Value(a - b);
        case BinaryOp::MUL: return Value(a * b);
        case BinaryOp::DIV: return b == 0.0 ? Value::null() : Value(a / b);
        case BinaryOp::MOD: return b == 0.0 ? Value::null() : Value(std::fmod(a, b));
        case BinaryOp::POW: return Value(std::pow(a, b));
        default:            throw EvalError("unsupported arithmetic operator");
    }
}

}  // namespace

// ============================================================================
// LIKE matcher — classic O(n*m) backtracking with % (any run) and _ (one char)
// ============================================================================

bool likeMatch(const std::string& text, const std::string& pattern, bool ci) {
    auto norm = [ci](char c) {
        return ci ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : c;
    };

    size_t ti = 0, pi = 0;
    size_t star = std::string::npos, tstar = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() &&
            (pattern[pi] == '_' || norm(pattern[pi]) == norm(text[ti]))) {
            ++ti; ++pi;
        } else if (pi < pattern.size() && pattern[pi] == '%') {
            star  = pi++;
            tstar = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++tstar;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '%') ++pi;
    return pi == pattern.size();
}

// ============================================================================
// evaluate()
// ============================================================================

Value evaluate(const ast::ExprNode& expr, const Schema& schema, const Row& row) {
    return std::visit([&](const auto& e) -> Value {
        using T = std::decay_t<decltype(e)>;

        // ---- Literal -------------------------------------------------------
        if constexpr (std::is_same_v<T, Literal>) {
            return e.value;
        }

        // ---- Column reference ---------------------------------------------
        else if constexpr (std::is_same_v<T, ColumnRef>) {
            if (e.isStar())
                throw EvalError("'*' is not a scalar expression");
            int idx = -1;
            if (e.table) idx = schema.indexOf(*e.table + "." + e.column);
            if (idx < 0) idx = schema.indexOf(e.column);
            if (idx < 0) throw EvalError("unknown column: " + e.toString());
            return row[static_cast<size_t>(idx)];
        }

        // ---- Unary ---------------------------------------------------------
        else if constexpr (std::is_same_v<T, UnaryExpr>) {
            Value v = evaluate(*e.operand, schema, row);
            switch (e.op) {
                case UnaryOp::PLUS:   return v;
                case UnaryOp::NEGATE:
                    if (v.isNull()) return Value::null();
                    if (typeIsInteger(v.typeId())) return Value(-v.toInt64());
                    return Value(-v.toDouble());
                case UnaryOp::NOT: {
                    int t = truthiness(v);
                    return boolValue(t == 2 ? 2 : (t == 1 ? 0 : 1));
                }
            }
            return Value::null();
        }

        // ---- Binary --------------------------------------------------------
        else if constexpr (std::is_same_v<T, BinaryExpr>) {
            // Short-circuit logical operators with 3-valued logic.
            if (e.op == BinaryOp::AND) {
                int l = truthiness(evaluate(*e.left, schema, row));
                if (l == 0) return Value(false);           // false AND x = false
                int r = truthiness(evaluate(*e.right, schema, row));
                if (r == 0) return Value(false);
                if (l == 2 || r == 2) return Value::null();
                return Value(true);
            }
            if (e.op == BinaryOp::OR) {
                int l = truthiness(evaluate(*e.left, schema, row));
                if (l == 1) return Value(true);            // true OR x = true
                int r = truthiness(evaluate(*e.right, schema, row));
                if (r == 1) return Value(true);
                if (l == 2 || r == 2) return Value::null();
                return Value(false);
            }

            Value l = evaluate(*e.left, schema, row);
            Value r = evaluate(*e.right, schema, row);

            switch (e.op) {
                case BinaryOp::ADD: case BinaryOp::SUB: case BinaryOp::MUL:
                case BinaryOp::DIV: case BinaryOp::MOD: case BinaryOp::POW:
                    return evalArithmetic(e.op, l, r);

                case BinaryOp::EQ: case BinaryOp::NEQ:
                case BinaryOp::LT: case BinaryOp::LTE:
                case BinaryOp::GT: case BinaryOp::GTE: {
                    int c = compareValues(l, r);
                    if (c == 2) return Value::null();      // NULL comparison
                    switch (e.op) {
                        case BinaryOp::EQ:  return Value(c == 0);
                        case BinaryOp::NEQ: return Value(c != 0);
                        case BinaryOp::LT:  return Value(c < 0);
                        case BinaryOp::LTE: return Value(c <= 0);
                        case BinaryOp::GT:  return Value(c > 0);
                        case BinaryOp::GTE: return Value(c >= 0);
                        default: break;
                    }
                    return Value::null();
                }

                case BinaryOp::CONCAT: {
                    if (l.isNull() || r.isNull()) return Value::null();
                    auto str = [](const Value& v) {
                        return v.typeId() == TypeId::VARCHAR ? v.getString() : v.toString();
                    };
                    return Value(str(l) + str(r));
                }

                case BinaryOp::LIKE:
                case BinaryOp::ILIKE: {
                    if (l.isNull() || r.isNull()) return Value::null();
                    return Value(likeMatch(l.getString(), r.getString(),
                                           e.op == BinaryOp::ILIKE));
                }

                default:
                    throw EvalError("unsupported binary operator in evaluator");
            }
        }

        // ---- IS [NOT] NULL -------------------------------------------------
        else if constexpr (std::is_same_v<T, IsNullExpr>) {
            bool isNull = evaluate(*e.expr, schema, row).isNull();
            return Value(e.negated ? !isNull : isNull);
        }

        // ---- BETWEEN -------------------------------------------------------
        else if constexpr (std::is_same_v<T, BetweenExpr>) {
            Value v  = evaluate(*e.expr, schema, row);
            Value lo = evaluate(*e.low, schema, row);
            Value hi = evaluate(*e.high, schema, row);
            int cl = compareValues(v, lo);
            int ch = compareValues(v, hi);
            if (cl == 2 || ch == 2) return Value::null();
            bool in = (cl >= 0) && (ch <= 0);
            return Value(e.negated ? !in : in);
        }

        // ---- IN (value list) ----------------------------------------------
        else if constexpr (std::is_same_v<T, InExpr>) {
            Value v = evaluate(*e.expr, schema, row);
            if (v.isNull()) return Value::null();
            const auto* list = std::get_if<ExprList>(&e.values);
            if (!list) throw EvalError("IN (subquery) is not supported by the evaluator");
            bool sawNull = false;
            for (const auto& item : *list) {
                Value iv = evaluate(*item, schema, row);
                if (iv.isNull()) { sawNull = true; continue; }
                if (compareValues(v, iv) == 0)
                    return Value(!e.negated);
            }
            if (sawNull) return Value::null();   // SQL: x IN (…,NULL) is unknown if no match
            return Value(e.negated);
        }

        // ---- CASE ----------------------------------------------------------
        else if constexpr (std::is_same_v<T, CaseExpr>) {
            std::optional<Value> subject;
            if (e.subject) subject = evaluate(**e.subject, schema, row);
            for (const auto& w : e.whenClauses) {
                bool matched;
                if (subject) {
                    Value cond = evaluate(*w.condition, schema, row);
                    matched = (compareValues(*subject, cond) == 0);
                } else {
                    matched = (truthiness(evaluate(*w.condition, schema, row)) == 1);
                }
                if (matched) return evaluate(*w.result, schema, row);
            }
            if (e.elseExpr) return evaluate(**e.elseExpr, schema, row);
            return Value::null();
        }

        // ---- CAST ----------------------------------------------------------
        else if constexpr (std::is_same_v<T, CastExpr>) {
            Value v = evaluate(*e.expr, schema, row);
            if (v.isNull()) return Value::null();
            TypeId target = e.targetType.id;
            if (typeIsInteger(target)) return Value(static_cast<int64_t>(v.toDouble()));
            if (typeIsFloat(target))   return Value(v.toDouble());
            if (target == TypeId::VARCHAR) return Value(v.toString());
            if (target == TypeId::BOOLEAN) return Value(truthiness(v) == 1);
            return v;
        }

        // ---- Scalar function calls ----------------------------------------
        else if constexpr (std::is_same_v<T, FunctionCall>) {
            if (e.isAggregate())
                throw EvalError("aggregate '" + e.name +
                                "' cannot be evaluated as a scalar expression");
            // A small set of common scalar functions.
            const std::string& fn = e.name;
            auto arg = [&](size_t i) { return evaluate(*e.args[i], schema, row); };

            if (fn == "COALESCE") {
                for (size_t i = 0; i < e.args.size(); ++i) {
                    Value v = arg(i);
                    if (!v.isNull()) return v;
                }
                return Value::null();
            }
            if (fn == "ABS" && e.args.size() == 1) {
                Value v = arg(0);
                if (v.isNull()) return Value::null();
                if (typeIsInteger(v.typeId())) return Value(std::abs(v.toInt64()));
                return Value(std::fabs(v.toDouble()));
            }
            if (fn == "LENGTH" && e.args.size() == 1) {
                Value v = arg(0);
                if (v.isNull()) return Value::null();
                return Value(static_cast<int64_t>(v.getString().size()));
            }
            if (fn == "UPPER" && e.args.size() == 1) {
                Value v = arg(0);
                if (v.isNull()) return Value::null();
                std::string s = v.getString();
                for (auto& c : s) c = static_cast<char>(std::toupper((unsigned char)c));
                return Value(s);
            }
            if (fn == "LOWER" && e.args.size() == 1) {
                Value v = arg(0);
                if (v.isNull()) return Value::null();
                std::string s = v.getString();
                for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
                return Value(s);
            }
            if ((fn == "ROUND") && e.args.size() >= 1) {
                Value v = arg(0);
                if (v.isNull()) return Value::null();
                double d = v.toDouble();
                int nd = e.args.size() >= 2 ? static_cast<int>(arg(1).toInt64()) : 0;
                double f = std::pow(10.0, nd);
                return Value(std::round(d * f) / f);
            }
            throw EvalError("unknown scalar function: " + fn);
        }

        // ---- Unsupported node types ---------------------------------------
        else {
            throw EvalError("expression type not supported by the evaluator");
        }
    }, expr);
}

bool evaluatePredicate(const ast::ExprNode& expr, const Schema& schema, const Row& row) {
    Value v = evaluate(expr, schema, row);
    if (v.isNull()) return false;
    if (v.typeId() == TypeId::BOOLEAN) return v.getBool();
    if (typeIsInteger(v.typeId())) return v.toInt64() != 0;
    if (typeIsFloat(v.typeId()))   return v.toDouble() != 0.0;
    return false;
}

}  // namespace lq
