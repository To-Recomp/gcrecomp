// SPDX-License-Identifier: MIT
//
// A line-based reader for the TOML subset the settings files use: `[table]` headers, `key = value` lines (bare or quoted
// keys), `#` comments, single-line values (booleans, numbers, strings, arrays of strings). The runtime only reads these
// files; writing them is the settings overlay's and the launcher's job (To-Recomp/overlay). Dotted keys, inline tables
// and multi-line values are read as absent.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace gcrecomp::settings {

class TomlDoc {
public:
    TomlDoc() = default;
    explicit TomlDoc(const std::string& text);

    // The value as written, without the trailing comment; `table` is "" for the top level.
    std::optional<std::string> raw(const std::string& table, const std::string& key) const;

    std::optional<bool> get_bool(const std::string& table, const std::string& key) const;
    std::optional<double> get_number(const std::string& table, const std::string& key) const;
    std::optional<std::string> get_string(const std::string& table, const std::string& key) const;

private:
    struct Line {
        std::string text;  // without the line ending
    };
    std::vector<Line> lines_;
    int find(const std::string& table, const std::string& key) const;
};

// Reads a whole file; false when it cannot be opened.
bool read_text_file(const std::string& path, std::string& out);

}  // namespace gcrecomp::settings
