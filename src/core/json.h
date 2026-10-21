#pragma once
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Small JSON reader for the config file. Strings are UTF-8.
namespace dx::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::shared_ptr<json::Array> array;
    std::shared_ptr<json::Object> object;

    [[nodiscard]] const Value* Find(std::string_view key) const;
    [[nodiscard]] double Num(std::string_view key, double def) const;
    [[nodiscard]] bool Bool(std::string_view key, bool def) const;
    [[nodiscard]] std::string Str(std::string_view key, std::string_view def) const;
};

[[nodiscard]] std::optional<Value> Parse(std::string_view text);
[[nodiscard]] std::string Quote(std::string_view s);
[[nodiscard]] std::string Number(double v);

}  // namespace dx::json
