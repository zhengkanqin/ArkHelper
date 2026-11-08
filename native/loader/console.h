#pragma once

#include <string>

namespace ark::loader {

// Writes UTF-16 text to a console, or UTF-8 when stdout is redirected.
void write_console(const std::wstring& text);

std::wstring utf8_to_wide(const std::string& text);

// Prints one `[HH:MM:SS] message` line; `format` follows swprintf conventions.
void print_status(const wchar_t* format, ...);

// Prompts and waits for a key press; returns immediately without an attached console.
void wait_for_key_press();

} // namespace ark::loader
