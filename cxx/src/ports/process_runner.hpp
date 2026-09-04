#pragma once
// =============================================================================
// Port: runs a subprocess to completion. The diff stage isolates each pair in
// a child (libabigail keeps per-process state and can exhaust memory), and
// the calibration tests compile fixtures with the system compiler.
// =============================================================================

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "core/error.hpp"

namespace abistudy::ports {

/// @brief Outcome of a finished subprocess.
struct Completed {
  int exit_code;                ///< Exit status; -signal if killed by a signal.
  std::string out;              ///< Everything written to stdout.
  std::string err;              ///< Everything written to stderr.
  bool timed_out = false;       ///< True if killed because the timeout elapsed.
  std::uint64_t max_rss_kb = 0; ///< Peak resident set of the child, if known.
};

struct RunOptions {
  std::optional<std::filesystem::path> cwd; ///< Working directory; inherit if absent.
  std::chrono::seconds timeout{600};        ///< Kill with SIGKILL after this.
};

class ProcessRunner {
public:
  virtual ~ProcessRunner() = default;
  ProcessRunner() = default;
  ProcessRunner(const ProcessRunner &) = delete;
  ProcessRunner &operator=(const ProcessRunner &) = delete;
  ProcessRunner(ProcessRunner &&) = delete;
  ProcessRunner &operator=(ProcessRunner &&) = delete;

  /// @brief Runs `argv[0]` (searched on PATH) with `argv[1..]`, capturing output.
  /// @pre   `argv` is non-empty and argv[0] is non-empty.
  /// @post  Child has exited or been killed; no zombie remains.
  /// @errors external_tool if the process could not be spawned. A non-zero
  ///         exit status is NOT an error here; the caller decides.
  [[nodiscard]] virtual Result<Completed> run(
    std::span<const std::string> argv, const RunOptions &opt
  ) const = 0;
};

} // namespace abistudy::ports
