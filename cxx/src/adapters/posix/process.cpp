#include "adapters/posix/process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/contracts.hpp"
#include "core/fs.hpp"

namespace abistudy::posix {
namespace {

/// @brief Drains `fd` into `dst` without blocking; returns false at EOF/error.
bool drain(int fd, std::string &dst) {
  std::array<char, std::size_t{1} << 14U> buf{};
  for (;;) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n > 0) {
      dst.append(buf.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0)
      return false;
    if (errno == EINTR)
      continue;
    return errno == EAGAIN || errno == EWOULDBLOCK;
  }
}

} // namespace

Result<ports::Completed> PosixProcessRunner::run(
  std::span<const std::string> argv, const ports::RunOptions &opt
) const {
  ABISTUDY_EXPECTS(!argv.empty() && !argv.front().empty());

  std::array<int, 2> out_pipe{};
  std::array<int, 2> err_pipe{};
  if (::pipe2(out_pipe.data(), O_CLOEXEC) != 0 || ::pipe2(err_pipe.data(), O_CLOEXEC) != 0)
    return fail(ErrorCode::external_tool, "pipe: {}", fs::errno_text(errno));

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
  if (opt.cwd)
    posix_spawn_file_actions_addchdir_np(&fa, opt.cwd->c_str());

  std::vector<char *> cargv;
  cargv.reserve(argv.size() + 1);
  // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast): posix_spawn takes char* const[] and never
  // writes through it.
  for (const auto &a : argv) {
    cargv.push_back(const_cast<char *>(a.c_str()));
  }
  // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
  cargv.push_back(nullptr);

  pid_t pid = 0;
  const int rc = ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (rc != 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    return fail(
      ErrorCode::external_tool, "cannot spawn '{}': {}", argv.front(), fs::errno_text(rc)
    );
  }
  ::fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  ::fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

  ports::Completed done{};
  const auto deadline = std::chrono::steady_clock::now() + opt.timeout;
  bool out_open = true;
  bool err_open = true;
  while (out_open || err_open) {
    std::array<pollfd, 2> fds{
      {{.fd = out_pipe[0], .events = POLLIN, .revents = 0},
       {.fd = err_pipe[0], .events = POLLIN, .revents = 0}}
    };
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()
    );
    if (left.count() <= 0) {
      ::kill(pid, SIGKILL);
      done.timed_out = true;
      break;
    }
    const int pr = ::poll(fds.data(), 2, static_cast<int>(std::min<long long>(left.count(), 1000)));
    if (pr < 0 && errno != EINTR)
      break;
    constexpr auto readable = static_cast<unsigned>(POLLIN) | static_cast<unsigned>(POLLHUP);
    if (out_open && (static_cast<unsigned>(fds[0].revents) & readable) != 0)
      out_open = drain(out_pipe[0], done.out);
    if (err_open && (static_cast<unsigned>(fds[1].revents) & readable) != 0)
      err_open = drain(err_pipe[0], done.err);
  }
  ::close(out_pipe[0]);
  ::close(err_pipe[0]);

  int status = 0;
  rusage usage{};
  while (::wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): glibc declares ru_maxrss in a union
  done.max_rss_kb = static_cast<std::uint64_t>(usage.ru_maxrss);
  if (WIFEXITED(status)) {
    done.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    done.exit_code = -WTERMSIG(status);
  } else {
    done.exit_code = -1;
  }
  return done;
}

} // namespace abistudy::posix
