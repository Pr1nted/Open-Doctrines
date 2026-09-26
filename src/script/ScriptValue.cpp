// ScriptValue's conversions, kept apart from ScriptEngine.cpp on purpose.
//
// They used to live there, which quietly made every user of a ScriptValue a
// user of Game.h and raylib -- so the expression parser could not be linked,
// or tested, without a windowing library. They depend on nothing but the
// standard library, and now say so.
#include "../ScriptEngine.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

std::string ScriptValue::asString() const {
    switch (type) {
        case INT: return std::to_string(intVal);
        case FLOAT: { char buf[32]; snprintf(buf, sizeof(buf), "%.2f", floatVal); return buf; }
        case STRING: return strVal;
        case BOOL: return boolVal ? "true" : "false";
        case MOD:  return strVal;          // the id, which is what a person reads
        default: return "";
    }
}

long long ScriptValue::asInt() const {
    switch (type) {
        case INT: return intVal;
        // CLAMPED, not cast. Converting a double that does not fit into a
        // long long is undefined behaviour, and a map script can produce one
        // with a single multiplication -- `set x = 1e300 % 2` was enough. A
        // NaN is nothing, and anything past the ends is the end it went past.
        case FLOAT: {
            if (std::isnan(floatVal)) return 0;
            constexpr double lo = -9223372036854775808.0;   // exactly LLONG_MIN
            constexpr double hi =  9223372036854775808.0;   // LLONG_MAX + 1
            if (floatVal <= lo) return std::numeric_limits<long long>::min();
            if (floatVal >= hi) return std::numeric_limits<long long>::max();
            return (long long)floatVal;
        }
        case BOOL: return boolVal ? 1 : 0;
        case MOD:  return boolVal ? 1 : 0;
        case STRING: { try { return std::stoll(strVal); } catch (...) { return 0; } }
        default: return 0;
    }
}

double ScriptValue::asFloat() const {
    switch (type) {
        case INT: return (double)intVal;
        case FLOAT: return floatVal;
        case BOOL: return boolVal ? 1.0 : 0.0;
        case MOD:  return boolVal ? 1.0 : 0.0;
        case STRING: { try { return std::stod(strVal); } catch (...) { return 0; } }
        default: return 0;
    }
}

bool ScriptValue::asBool() const {
    switch (type) {
        case INT: return intVal != 0;
        case FLOAT: return floatVal != 0.0;
        case BOOL: return boolVal;
        // A mod is "true" when it is installed and enabled.
        case MOD:  return boolVal;
        case STRING: return strVal == "true" || strVal == "1";
        default: return false;
    }
}
