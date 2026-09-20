#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace vmtest {

// Decodes standard base64 (as returned by the QEMU guest agent). Whitespace and
// unknown characters are ignored; decoding stops at the first '='.
std::string base64_decode(const std::string& in);

// Escapes the five XML special characters.
std::string xml_escape(const std::string& in);

// Runs a program (no shell) and returns its exit status, or -1 if it could not
// be started or was killed by a signal.
int run_cmd(const std::vector<std::string>& argv);

// Polls `pred` every `interval` until it returns true or `timeout` elapses.
bool wait_until(std::chrono::milliseconds timeout, std::chrono::milliseconds interval,
                const std::function<bool()>& pred);

}  // namespace vmtest
