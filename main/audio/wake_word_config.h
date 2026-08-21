#pragma once

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

struct WakeWordCommandEntry {
    std::string command;
    std::string display;
};

inline constexpr size_t kMaxWakeWordCommands = 8;
inline constexpr size_t kMaxWakeWordCommandLen = 64;
inline constexpr size_t kMaxWakeWordDisplayLen = 32;

inline bool IsValidWakeWordPinyin(const std::string& command) {
    if (command.empty() || command.size() > kMaxWakeWordCommandLen) {
        return false;
    }
    if (command.front() == ' ' || command.back() == ' ') {
        return false;
    }
    for (size_t i = 0; i < command.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(command[i]);
        if (ch >= 'A' && ch <= 'Z') {
            return false;
        }
        if (ch == ' ') {
            if (i + 1 >= command.size() || command[i + 1] == ' ') {
                return false;
            }
            continue;
        }
        if (ch < 'a' || ch > 'z') {
            return false;
        }
    }
    return true;
}

inline bool IsValidWakeWordDisplay(const std::string& display) {
    if (display.empty() || display.size() > kMaxWakeWordDisplayLen) {
        return false;
    }
    for (unsigned char ch : display) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

inline bool ValidateWakeWordEntries(const std::vector<WakeWordCommandEntry>& entries,
                                    std::string* error) {
    if (entries.empty()) {
        if (error) {
            *error = "wake_word commands are required";
        }
        return false;
    }
    if (entries.size() > kMaxWakeWordCommands) {
        if (error) {
            *error = "too many wake_word commands (max 8)";
        }
        return false;
    }

    std::unordered_set<std::string> seen_commands;
    std::unordered_set<std::string> seen_displays;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.command.empty() || entry.display.empty()) {
            if (error) {
                *error = "wake_word command and display are required";
            }
            return false;
        }
        if (!IsValidWakeWordPinyin(entry.command)) {
            if (error) {
                *error =
                    "invalid wake_word pinyin at index " + std::to_string(i) +
                    ", use lowercase syllables separated by spaces";
            }
            return false;
        }
        if (!IsValidWakeWordDisplay(entry.display)) {
            if (error) {
                *error = "invalid wake_word display at index " + std::to_string(i);
            }
            return false;
        }
        if (!seen_commands.insert(entry.command).second) {
            if (error) {
                *error = "duplicate wake_word pinyin: " + entry.command;
            }
            return false;
        }
        if (!seen_displays.insert(entry.display).second) {
            if (error) {
                *error = "duplicate wake_word display: " + entry.display;
            }
            return false;
        }
    }
    return true;
}
