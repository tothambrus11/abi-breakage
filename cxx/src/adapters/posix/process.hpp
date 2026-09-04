#pragma once
// =============================================================================
// ProcessRunner adapter over posix_spawn / poll / wait4.
// =============================================================================

#include "ports/process_runner.hpp"

namespace abistudy::posix {

class PosixProcessRunner final : public ports::ProcessRunner {
public:
  [[nodiscard]] Result<ports::Completed> run(
    std::span<const std::string> argv, const ports::RunOptions &opt
  ) const override;
};

} // namespace abistudy::posix
