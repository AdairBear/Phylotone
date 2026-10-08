// Minimal JSON reader and string escaper for the assistant core.
//
// It exists so the core needs no JSON library. It handles what tool arguments
// and provider payloads need: objects, arrays, strings, numbers, booleans and
// null. It is not a general-purpose JSON library.

#pragma once

#include <string>
#include <vector>

namespace phylo::json
{

struct Value
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> items;      // Array elements
    std::vector<std::string> keys; // Object keys, parallel to `values`
    std::vector<Value> values;     // Object values, parallel to `keys`

    // Object member lookup. Returns nullptr if missing or not an object.
    const Value* get(const std::string& key) const;

    bool isString() const noexcept { return type == Type::String; }
    bool isNumber() const noexcept { return type == Type::Number; }
    bool isObject() const noexcept { return type == Type::Object; }
};

// Parses a complete JSON document. On failure returns false and sets error.
bool parse(const std::string& text, Value& out, std::string& error);

// Returns s as a quoted JSON string literal, with escapes applied.
std::string quote(const std::string& s);

} // namespace phylo::json
