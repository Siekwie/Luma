#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace luma::util {

/// Quote and escape a string as a JSON string literal. Control characters
/// are escaped; invalid UTF-8 bytes are replaced with U+FFFD so the output
/// is always valid JSON.
std::string json_quote(std::string_view text);

/// Minimal streaming JSON writer (no dependencies).
class JsonWriter {
public:
    /// With `indent` > 0 the output is pretty-printed.
    explicit JsonWriter(int indent = 0) : indent_(indent) {}

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);
    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) { return value(std::string_view(text)); }
    JsonWriter& value(const std::string& text) { return value(std::string_view(text)); }
    JsonWriter& value(bool flag);
    JsonWriter& value(std::int64_t number);
    JsonWriter& null();
    JsonWriter& string_array(const std::vector<std::string>& values);

    /// Shorthand for key(name).value(v).
    template <typename T>
    JsonWriter& field(std::string_view name, const T& v) {
        key(name);
        return value(v);
    }

    const std::string& str() const { return out_; }

private:
    void before_value();
    void newline();

    struct Frame {
        bool is_object = false;
        std::size_t count = 0;
    };

    int indent_ = 0;
    std::string out_;
    std::vector<Frame> stack_;
    bool after_key_ = false;
};

} // namespace luma::util
