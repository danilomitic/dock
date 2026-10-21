#include "core/json.h"

#include <charconv>
#include <cstdio>

namespace dx::json {
namespace {

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool Document(Value& v) {
        if (!ParseValue(v)) return false;
        SkipWs();
        return i_ == s_.size();
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    int depth_ = 0;

    void SkipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool Lit(std::string_view w) {
        if (s_.substr(i_, w.size()) == w) {
            i_ += w.size();
            return true;
        }
        return false;
    }
    bool Hex4(unsigned& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            switch (s_[i_++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        unsigned lo = 0;
                        if (!Lit("\\u") || !Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool ParseNumber(double& out) {
        const size_t start = i_;
        while (i_ < s_.size() && (std::string_view("+-0123456789.eE").find(s_[i_]) != std::string_view::npos)) ++i_;
        if (start == i_) return false;
        const auto res = std::from_chars(s_.data() + start, s_.data() + i_, out);
        return res.ec == std::errc() && res.ptr == s_.data() + i_;
    }
    bool ParseValue(Value& v) {
        SkipWs();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            if (++depth_ > 64) return false;
            ++i_;
            v.type = Value::Type::Object;
            v.object = std::make_shared<Object>();
            SkipWs();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                --depth_;
                return true;
            }
            for (;;) {
                SkipWs();
                std::string key;
                if (!String(key)) return false;
                SkipWs();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                Value child;
                if (!ParseValue(child)) return false;
                v.object->emplace_back(std::move(key), std::move(child));
                SkipWs();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; --depth_; return true; }
                return false;
            }
        }
        if (c == '[') {
            if (++depth_ > 64) return false;
            ++i_;
            v.type = Value::Type::Array;
            v.array = std::make_shared<Array>();
            SkipWs();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                --depth_;
                return true;
            }
            for (;;) {
                Value child;
                if (!ParseValue(child)) return false;
                v.array->push_back(std::move(child));
                SkipWs();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; --depth_; return true; }
                return false;
            }
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return String(v.string);
        }
        if (Lit("true")) { v.type = Value::Type::Bool; v.boolean = true; return true; }
        if (Lit("false")) { v.type = Value::Type::Bool; v.boolean = false; return true; }
        if (Lit("null")) { v.type = Value::Type::Null; return true; }
        v.type = Value::Type::Number;
        return ParseNumber(v.number);
    }
};

}  // namespace

const Value* Value::Find(std::string_view key) const {
    if (type != Type::Object || !object) return nullptr;
    for (const auto& [k, v] : *object)
        if (k == key) return &v;
    return nullptr;
}

double Value::Num(std::string_view key, double def) const {
    const Value* v = Find(key);
    return (v && v->type == Type::Number) ? v->number : def;
}

bool Value::Bool(std::string_view key, bool def) const {
    const Value* v = Find(key);
    return (v && v->type == Type::Bool) ? v->boolean : def;
}

std::string Value::Str(std::string_view key, std::string_view def) const {
    const Value* v = Find(key);
    return (v && v->type == Type::String) ? v->string : std::string(def);
}

std::optional<Value> Parse(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.remove_prefix(3);  // UTF-8 BOM
    Value v;
    Parser p(text);
    if (!p.Document(v)) return std::nullopt;
    return v;
}

std::string Quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string Number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

}  // namespace dx::json
