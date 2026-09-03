#include "deb/extract.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <string_view>

#include <archive.h>
#include <archive_entry.h>

#include "core/contracts.hpp"
#include "core/fs.hpp"

namespace abistudy::deb {
namespace {

struct ReadCloser {
  void operator()(archive *a) const noexcept {
    if (a)
      archive_read_free(a);
  }
};
struct WriteCloser {
  void operator()(archive *a) const noexcept {
    if (a)
      archive_write_free(a);
  }
};
using ReadArchive = std::unique_ptr<archive, ReadCloser>;
using WriteArchive = std::unique_ptr<archive, WriteCloser>;

/// @brief libarchive read callback that pulls the inner tarball's bytes out
///        of the current entry of the outer `ar` archive.
struct Nested {
  archive *outer;
  std::array<char, 1 << 16> buf;
};

la_ssize_t nested_read(archive * /*inner*/, void *client, const void **out) {
  auto *n = static_cast<Nested *>(client);
  const la_ssize_t got = archive_read_data(n->outer, n->buf.data(), n->buf.size());
  *out = n->buf.data();
  return got < 0 ? -1 : got;
}

/// @brief True if `p` is a relative path with no `..` component. An archive
///        member failing this can escape `dest` and is refused.
[[nodiscard]] bool safe_relative(std::string_view p) {
  if (p.empty() || p.front() == '/')
    return false;
  std::size_t start = 0;
  while (start <= p.size()) {
    const auto end = p.find('/', start);
    const auto seg =
      p.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (seg == "..")
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return true;
}

[[nodiscard]] std::string err_of(archive *a) {
  const char *s = archive_error_string(a);
  return s ? s : "unknown libarchive error";
}

/// @brief Copies one entry's data blocks from reader to disk writer.
Result<std::uint64_t> copy_data(archive *in, archive *out) {
  std::uint64_t total = 0;
  for (;;) {
    const void *buff = nullptr;
    size_t size = 0;
    la_int64_t offset = 0;
    const int r = archive_read_data_block(in, &buff, &size, &offset);
    if (r == ARCHIVE_EOF)
      return total;
    if (r < ARCHIVE_OK)
      return fail(ErrorCode::parse, "reading entry data: {}", err_of(in));
    if (archive_write_data_block(out, buff, size, offset) < ARCHIVE_OK)
      return fail(ErrorCode::io, "writing entry data: {}", err_of(out));
    total += size;
  }
}

} // namespace

Result<ExtractStats> extract_deb(
  const std::filesystem::path &deb, const std::filesystem::path &dest
) {
  ABISTUDY_EXPECTS(!dest.empty());
  ABISTUDY_TRY_VOID(fs::ensure_dir(dest));

  std::error_code fec;
  if (!std::filesystem::is_regular_file(deb, fec))
    return fail(ErrorCode::io, "cannot open '{}': no such file", deb.string());
  ReadArchive outer{archive_read_new()};
  archive_read_support_format_ar(outer.get());
  // libarchive bids on the format while opening: a readable file that is not
  // an `ar` archive fails HERE, and that is a parse error, not an I/O one.
  if (archive_read_open_filename(outer.get(), deb.c_str(), 1 << 16) != ARCHIVE_OK)
    return fail(ErrorCode::parse, "'{}' is not a .deb: {}", deb.string(), err_of(outer.get()));

  archive_entry *entry = nullptr;
  for (;;) {
    const int r = archive_read_next_header(outer.get(), &entry);
    if (r == ARCHIVE_EOF)
      return fail(ErrorCode::parse, "'{}' has no data.tar member", deb.string());
    if (r < ARCHIVE_OK)
      return fail(ErrorCode::parse, "'{}' is not a .deb: {}", deb.string(), err_of(outer.get()));
    if (std::string_view{archive_entry_pathname(entry)}.starts_with("data.tar"))
      break;
  }

  Nested nested{.outer = outer.get(), .buf = {}};
  ReadArchive inner{archive_read_new()};
  archive_read_support_filter_all(inner.get());
  archive_read_support_format_tar(inner.get());
  if (archive_read_open(inner.get(), &nested, nullptr, &nested_read, nullptr) != ARCHIVE_OK) {
    return fail(
      ErrorCode::parse, "'{}': cannot open data.tar: {}", deb.string(), err_of(inner.get())
    );
  }

  // Entries are rewritten to absolute paths under `dest`, so libarchive's
  // NOABSOLUTEPATHS flag cannot be used; the equivalent checks are done on
  // the entry's own path below. SECURE_SYMLINKS still refuses to write
  // through a symlink that an earlier entry planted; NODOTDOT still refuses
  // a `..` anywhere in the final path.
  WriteArchive disk{archive_write_disk_new()};
  archive_write_disk_set_options(
    disk.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                  ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_UNLINK
  );
  archive_write_disk_set_standard_lookup(disk.get());

  ExtractStats stats;
  for (;;) {
    const int r = archive_read_next_header(inner.get(), &entry);
    if (r == ARCHIVE_EOF)
      break;
    if (r < ARCHIVE_OK)
      return fail(ErrorCode::parse, "'{}': data.tar: {}", deb.string(), err_of(inner.get()));
    const auto type = archive_entry_filetype(entry);
    if (type == AE_IFCHR || type == AE_IFBLK || type == AE_IFIFO || type == AE_IFSOCK)
      continue;

    std::string_view rel{archive_entry_pathname(entry)};
    while (rel.starts_with("./"))
      rel.remove_prefix(2);
    if (rel.empty() || rel == ".")
      continue;
    if (!safe_relative(rel)) {
      ++stats.refused;
      continue;
    }
    const auto target = dest / rel;
    archive_entry_set_pathname(entry, target.c_str());
    if (const char *hl = archive_entry_hardlink(entry)) {
      std::string_view h{hl};
      while (h.starts_with("./"))
        h.remove_prefix(2);
      if (!safe_relative(h)) {
        ++stats.refused;
        continue;
      }
      archive_entry_set_hardlink(entry, (dest / h).c_str());
    }
    if (archive_write_header(disk.get(), entry) < ARCHIVE_OK) {
      // A refused symlink/hardlink is not fatal for our purposes; a
      // refused regular file is.
      if (type == AE_IFREG)
        return fail(ErrorCode::io, "cannot create '{}': {}", target.string(), err_of(disk.get()));
      continue;
    }
    if (type == AE_IFREG) {
      ABISTUDY_TRY(auto n, copy_data(inner.get(), disk.get()));
      stats.files += 1;
      stats.bytes += n;
    }
    archive_write_finish_entry(disk.get());
  }
  return stats;
}

Result<std::string> read_maybe_compressed(const std::filesystem::path &p) {
  ReadArchive a{archive_read_new()};
  archive_read_support_filter_all(a.get());
  archive_read_support_format_raw(a.get());
  archive_read_support_format_empty(a.get());
  if (archive_read_open_filename(a.get(), p.c_str(), 1 << 16) != ARCHIVE_OK)
    return fail(ErrorCode::io, "cannot open '{}': {}", p.string(), err_of(a.get()));
  archive_entry *e = nullptr; // NOLINT(misc-const-correctness): libarchive out-parameter
  const int r = archive_read_next_header(a.get(), &e);
  if (r == ARCHIVE_EOF)
    return std::string{};
  if (r < ARCHIVE_OK)
    return fail(ErrorCode::parse, "'{}': {}", p.string(), err_of(a.get()));
  std::string out;
  std::array<char, 1 << 16> buf{};
  for (;;) {
    const la_ssize_t n = archive_read_data(a.get(), buf.data(), buf.size());
    if (n == 0)
      break;
    if (n < 0)
      return fail(ErrorCode::parse, "'{}': decompression failed: {}", p.string(), err_of(a.get()));
    out.append(buf.data(), static_cast<std::size_t>(n));
  }
  return out;
}

} // namespace abistudy::deb
