#pragma once
// =============================================================================
// Minimal subprocess runner. The pipeline proper never shells out -- every
// analysis is in-process through libabigail and libclang. This exists for the
// calibration tests, which must compile thirty tiny libraries with g++.
// =============================================================================

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace abistudy::proc {

/// @brief Outcome of a finished subprocess.
struct Completed {
  int exit_code;          ///< Exit status; -signal if killed by a signal.
  std::string out;        ///< Everything written to stdout.
  std::string err;        ///< Everything written to stderr.
  bool timed_out = false; ///< True if killed because `timeout` elapsed.
};

/// @brief Options for run().
struct Options {
  std::optional<std::filesystem::path> cwd; ///< Working directory; inherit if absent.
  std::chrono::seconds timeout{600};        ///< Kill with SIGKILL after this.
};

/// @brief Runs `argv[0]` (searched on PATH) with `argv[1..]`, capturing output.
/// @pre   `argv` is non-empty and argv[0] is non-empty.
/// @post  Child has exited or been killed; no zombie remains.
/// @errors external_tool if the process could not be spawned. A non-zero exit
///         status is NOT an error here -- it is reported in Completed and the
///         caller decides.
[[nodiscard]] Result<Completed> run(std::span<const std::string> argv, const Options &opt = {});

} // namespace abistudy::proc
