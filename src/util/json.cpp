#include "luma/util/json.hpp"

namespace luma::util {

namespace {

/// Length of the valid UTF-8 sequence starting at `i`, or 0 if invalid.
std::size_t utf8_sequence_length(std::string_view text, std::size_t i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    std::uint32_t code = 0;
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code = lead & 0x07;
    } else {
        return 0;
    }
    if (i + length > text.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        if ((byte(i + k) & 0xC0) != 0x80) {
            return 0;
        }
        code = (code << 6) | (byte(i + k) & 0x3F);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
        (length == 4 && (code < 0x10000 || code > 0x10FFFF)) || (code >= 0xD800 && code <= 0xDFFF)) {
        return 0;
    }
    return length;
}

} // namespace

std::string json_quote(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        switch (c) {
        case '"':
            out += "\\\"";
            ++i;
            continue;
        case '\\':
            out += "\\\\";
            ++i;
            continue;
        case '\b':
            out += "\\b";
            ++i;
            continue;
        case '\f':
            out += "\\f";
            ++i;
            continue;
        case '\n':
            out += "\\n";
            ++i;
            continue;
        case '\r':
            out += "\\r";
            ++i;
            continue;
        case '\t':
            out += "\\t";
            ++i;
            continue;
        default:
            break;
        }
        if (c < 0x20 || c == 0x7F) {
            out += "\\u00";
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
            ++i;
            continue;
        }
        const std::size_t length = utf8_sequence_length(text, i);
        if (length == 0) {
            out += "\\ufffd";
            ++i;
            continue;
        }
        out.append(text.substr(i, length));
        i += length;
    }
    out.push_back('"');
    return out;
}

void JsonWriter::newline() {
    if (indent_ <= 0) {
        return;
    }
    out_.push_back('\n');
    out_.append(stack_.size() * static_cast<std::size_t>(indent_), ' ');
}

void JsonWriter::before_value() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!stack_.empty()) {
        if (stack_.back().count++ > 0) {
            out_.push_back(',');
        }
        newline();
    }
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    out_.push_back('{');
    stack_.push_back(Frame{.is_object = true, .count = 0});
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    const bool empty = stack_.back().count == 0;
    stack_.pop_back();
    if (!empty) {
        newline();
    }
    out_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    out_.push_back('[');
    stack_.push_back(Frame{.is_object = false, .count = 0});
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    const bool empty = stack_.back().count == 0;
    stack_.pop_back();
    if (!empty) {
        newline();
    }
    out_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    before_value();
    out_ += json_quote(name);
    out_ += indent_ > 0 ? ": " : ":";
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    before_value();
    out_ += json_quote(text);
    return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
    before_value();
    out_ += flag ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t number) {
    before_value();
    out_ += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_value();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::string_array(const std::vector<std::string>& values) {
    begin_array();
    for (const auto& v : values) {
        value(v);
    }
    return end_array();
}

} // namespace luma::util
