#include "core/fs.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "core/contracts.hpp"

namespace abistudy::fs {

std::string errno_text(int err) { return std::system_category().message(err); }

Result<std::string> read_file(const stdfs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return fail(ErrorCode::io, "cannot open '{}' for reading: {}", p.string(), errno_text(errno));
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad())
    return fail(ErrorCode::io, "read error on '{}'", p.string());
  return std::move(ss).str();
}

Result<void> write_file_atomic(const stdfs::path &p, std::string_view content) {
  std::error_code ec;
  if (p.has_parent_path()) {
    stdfs::create_directories(p.parent_path(), ec);
    if (ec)
      return fail(ErrorCode::io, "cannot create '{}': {}", p.parent_path().string(), ec.message());
  }
  std::string tmpl = (p.parent_path() / (p.filename().string() + ".tmp.XXXXXX")).string();
  const int fd = ::mkstemp(tmpl.data());
  if (fd < 0)
    return fail(ErrorCode::io, "mkstemp near '{}': {}", p.string(), errno_text(errno));

  std::size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      const int e = errno;
      ::close(fd);
      ::unlink(tmpl.c_str());
      return fail(ErrorCode::io, "write to '{}': {}", tmpl, errno_text(e));
    }
    off += static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    const int e = errno;
    ::unlink(tmpl.c_str());
    return fail(ErrorCode::io, "fsync/close '{}': {}", tmpl, errno_text(e));
  }
  if (::rename(tmpl.c_str(), p.c_str()) != 0) {
    const int e = errno;
    ::unlink(tmpl.c_str());
    return fail(ErrorCode::io, "rename '{}' -> '{}': {}", tmpl, p.string(), errno_text(e));
  }
  return {};
}

Result<void> ensure_dir(const stdfs::path &p) {
  std::error_code ec;
  stdfs::create_directories(p, ec);
  if (ec)
    return fail(ErrorCode::io, "cannot create directory '{}': {}", p.string(), ec.message());
  if (!stdfs::is_directory(p, ec))
    return fail(ErrorCode::io, "'{}' exists but is not a directory", p.string());
  return {};
}

std::uintmax_t remove_all_noexcept(const stdfs::path &p) noexcept {
  std::error_code ec;
  const auto n = stdfs::remove_all(p, ec);
  return ec ? 0 : n;
}

bool is_under(const stdfs::path &root, const stdfs::path &p) {
  ABISTUDY_EXPECTS(!root.empty());
  std::error_code ec;
  const auto r = stdfs::weakly_canonical(root, ec).lexically_normal();
  if (ec)
    return false;
  const auto q = stdfs::weakly_canonical(p, ec).lexically_normal();
  if (ec)
    return false;
  const auto rs = r.string();
  const auto qs = q.string();
  if (qs.size() < rs.size() || !qs.starts_with(rs))
    return false;
  return qs.size() == rs.size() || qs[rs.size()] == '/' || rs.back() == '/';
}

std::uintmax_t tree_size(const stdfs::path &p) noexcept {
  std::error_code ec;
  std::uintmax_t total = 0;
  for (auto it = stdfs::recursive_directory_iterator(p, ec);
       !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec))
      total += it->file_size(ec);
  }
  return total;
}

Result<TempDir> TempDir::create(const stdfs::path &base, std::string_view prefix) {
  ABISTUDY_TRY_VOID(ensure_dir(base));
  std::string tmpl = (base / (std::string{prefix} + "XXXXXX")).string();
  if (::mkdtemp(tmpl.data()) == nullptr)
    return fail(ErrorCode::io, "mkdtemp under '{}': {}", base.string(), errno_text(errno));
  return TempDir{stdfs::path{tmpl}};
}

TempDir &TempDir::operator=(TempDir &&o) noexcept {
  if (this != &o) {
    if (!path_.empty())
      remove_all_noexcept(path_);
    path_ = std::move(o.path_);
    o.path_.clear();
  }
  return *this;
}

TempDir::~TempDir() {
  if (!path_.empty())
    remove_all_noexcept(path_);
}

Result<LockFile> LockFile::acquire(const stdfs::path &p) {
  if (p.has_parent_path())
    ABISTUDY_TRY_VOID(ensure_dir(p.parent_path()));
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644); // NOLINT(*-vararg)
  if (fd < 0)
    return fail(ErrorCode::io, "cannot open lock file '{}': {}", p.string(), errno_text(errno));
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK)
      return fail(
        ErrorCode::io, "'{}' is locked: another abistudy stage owns this scratch tree", p.string()
      );
    return fail(ErrorCode::io, "cannot lock '{}': {}", p.string(), errno_text(err));
  }
  return LockFile{fd};
}

LockFile &LockFile::operator=(LockFile &&o) noexcept {
  if (this != &o) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = o.fd_;
    o.fd_ = -1;
  }
  return *this;
}

LockFile::~LockFile() {
  if (fd_ >= 0)
    ::close(fd_); // releases the flock
}

} // namespace abistudy::fs
