#pragma once

#include <string>

#include "dtest/scheduler.hpp"

namespace dtest {

std::string xml_escape(const std::string& s);

// Keeps the last `max_lines` lines / `max_bytes` bytes of a stream (failures are usually at the end).
std::string tail(const std::string& s, size_t max_lines, size_t max_bytes);

// Human-readable per-task lines plus a one-line summary.
std::string render_text(const RunSummary& s);

// JUnit XML: one testcase per task; classname is the agent that ran the final attempt.
std::string render_junit(const std::string& suite_name, const RunSummary& s);

}  // namespace dtest
