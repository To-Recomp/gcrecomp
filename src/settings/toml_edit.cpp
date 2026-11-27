// SPDX-License-Identifier: MIT
#include "toml_edit.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gcrecomp::settings {

namespace {

enum class Kind { Blank, Comment, Table, KeyValue, Other };

struct Parsed {
    Kind kind = Kind::Other;
    std::string name;        // table name, or key
    size_t value_begin = 0;  // KeyValue: first character of the value
    size_t value_end = 0;    // KeyValue: one past the last character of the value (no trailing space or comment)
};

bool is_space(char c) { return c == ' ' || c == '\t'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Reads a basic ("...") or literal ('...') string starting at s[i]; leaves `end` one past the closing quote.
bool read_quoted(const std::string& s, size_t i, std::string& out, size_t& end) {
    const char q = s[i];
    out.clear();
    for (++i; i < s.size(); ++i) {
        const char c = s[i];
        if (c == q) {
            end = i + 1;
            return true;
        }
        if (q == '"' && c == '\\') {
            if (++i >= s.size()) return false;
            switch (s[i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: return false;
            }
        } else {
            out += c;
        }
    }
    return false;
}

Parsed parse_line(const std::string& line) {
    Parsed p;
    size_t i = 0;
    while (i < line.size() && is_space(line[i])) ++i;
    if (i == line.size()) {
        p.kind = Kind::Blank;
        return p;
    }
    if (line[i] == '#') {
        p.kind = Kind::Comment;
        return p;
    }
    if (line[i] == '[') {
        if (i + 1 < line.size() && line[i + 1] == '[') return p;  // array of tables: not used
        const size_t close = line.find(']', i);
        if (close == std::string::npos) return p;
        p.kind = Kind::Table;
        p.name = trim(line.substr(i + 1, close - i - 1));
        return p;
    }
    std::string key;
    size_t after_key = i;
    if (line[i] == '"' || line[i] == '\'') {
        if (!read_quoted(line, i, key, after_key)) return p;
    } else {
        const size_t eq = line.find('=', i);
        if (eq == std::string::npos) return p;
        key = trim(line.substr(i, eq - i));
        after_key = eq;
    }
    while (after_key < line.size() && is_space(line[after_key])) ++after_key;
    if (after_key >= line.size() || line[after_key] != '=' || key.empty()) return p;
    size_t v = after_key + 1;
    while (v < line.size() && is_space(line[v])) ++v;
    size_t e = v;  // the value ends at a comment that is outside quotes
    bool in_str = false;
    char quote = 0;
    for (; e < line.size(); ++e) {
        const char c = line[e];
        if (in_str) {
            if (quote == '"' && c == '\\') ++e;
            else if (c == quote) in_str = false;
        } else if (c == '"' || c == '\'') {
            in_str = true;
            quote = c;
        } else if (c == '#') {
            break;
        }
    }
    while (e > v && (is_space(line[e - 1]) || line[e - 1] == '\r')) --e;
    p.kind = Kind::KeyValue;
    p.name = std::move(key);
    p.value_begin = v;
    p.value_end = e;
    return p;
}

}  // namespace

TomlDoc::TomlDoc(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const size_t nl = text.find('\n', i);
        Line l;
        if (nl == std::string::npos) {
            l.text = text.substr(i);
            i = text.size();
        } else {
            l.text = text.substr(i, nl - i);
            if (!l.text.empty() && l.text.back() == '\r') l.text.pop_back();
            i = nl + 1;
        }
        lines_.push_back(std::move(l));
    }
}

int TomlDoc::find(const std::string& table, const std::string& key) const {
    std::string current;
    for (size_t i = 0; i < lines_.size(); ++i) {
        const Parsed p = parse_line(lines_[i].text);
        if (p.kind == Kind::Table) current = p.name;
        else if (p.kind == Kind::KeyValue && current == table && p.name == key) return int(i);
    }
    return -1;
}

std::optional<std::string> TomlDoc::raw(const std::string& table, const std::string& key) const {
    const int i = find(table, key);
    if (i < 0) return std::nullopt;
    const Parsed p = parse_line(lines_[size_t(i)].text);
    return lines_[size_t(i)].text.substr(p.value_begin, p.value_end - p.value_begin);
}

std::optional<bool> TomlDoc::get_bool(const std::string& table, const std::string& key) const {
    const auto r = raw(table, key);
    if (r == "true") return true;
    if (r == "false") return false;
    return std::nullopt;
}

std::optional<double> TomlDoc::get_number(const std::string& table, const std::string& key) const {
    const auto r = raw(table, key);
    if (!r || r->empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(r->c_str(), &end);
    if (*end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<std::string> TomlDoc::get_string(const std::string& table, const std::string& key) const {
    const auto r = raw(table, key);
    if (!r || r->empty() || (r->front() != '"' && r->front() != '\'')) return std::nullopt;
    std::string out;
    size_t end = 0;
    if (!read_quoted(*r, 0, out, end) || end != r->size()) return std::nullopt;
    return out;
}

bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace gcrecomp::settings
