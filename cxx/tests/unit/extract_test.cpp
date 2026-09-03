// Builds a real .deb in memory with libarchive (ar wrapper around a
// zstd/xz-compressed data.tar), extracts it with deb::extract_deb, and checks
// that ordinary members land where the pipeline expects them while members
// that could escape the destination are refused.
#include "adapters/libarchive/extract.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <filesystem>
#include <print>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "core/fs.hpp"

using namespace abistudy;
namespace stdfs = std::filesystem;

namespace {

void add_regular(archive *a, const std::string &name, const std::string &content) {
  archive_entry *e = archive_entry_new();
  archive_entry_set_pathname(e, name.c_str());
  archive_entry_set_size(e, static_cast<la_int64_t>(content.size()));
  archive_entry_set_filetype(e, AE_IFREG);
  archive_entry_set_perm(e, 0644);
  archive_write_header(a, e);
  archive_write_data(a, content.data(), content.size());
  archive_entry_free(e);
}

/// @brief A tar.xz with the given (path, content) members, as bytes.
std::string make_data_tar(const std::vector<std::pair<std::string, std::string>> &files) {
  std::string buf(std::size_t{1} << 20U, '\0');
  std::size_t used = 0;
  archive *a = archive_write_new();
  archive_write_set_format_pax_restricted(a);
  archive_write_add_filter_xz(a);
  archive_write_open_memory(a, buf.data(), buf.size(), &used);
  for (const auto &[p, c] : files)
    add_regular(a, p, c);
  archive_write_close(a);
  archive_write_free(a);
  buf.resize(used);
  return buf;
}

/// @brief Wraps control/data tarballs in the ar container a .deb is.
Result<void> make_deb(const stdfs::path &out, const std::string &data_tar) {
  std::string buf(std::size_t{1} << 20U, '\0');
  std::size_t used = 0;
  archive *a = archive_write_new();
  archive_write_set_format_ar_svr4(a);
  archive_write_open_memory(a, buf.data(), buf.size(), &used);
  add_regular(a, "debian-binary", "2.0\n");
  add_regular(a, "control.tar.xz", make_data_tar({{"./control", "Package: demo\n"}}));
  add_regular(a, "data.tar.xz", data_tar);
  archive_write_close(a);
  archive_write_free(a);
  buf.resize(used);
  return fs::write_file_atomic(out, buf);
}

} // namespace

int main() try {
  auto tmp = fs::TempDir::create(stdfs::temp_directory_path(), "extract-test-");
  CHECK(tmp.has_value());
  const auto root = tmp->path();

  const auto data = make_data_tar({
    {"./usr/lib/x86_64-linux-gnu/libdemo.so.1.2.3", "\x7f"
                                                    "ELF-not-really"},
    {"./usr/include/demo/demo.h", "static inline int f(void) { return 1; }\n"},
    {"../escape.txt", "must not be written"},
    {"/abs/escape.txt", "must not be written"},
  });
  CHECK(make_deb(root / "demo.deb", data).has_value());

  const auto dest = root / "x";
  const auto st = deb::LibarchiveExtractor{}.extract(root / "demo.deb", dest);
  CHECK(st.has_value());
  if (st) {
    CHECK_EQ(st->files, 2U);
    CHECK_EQ(st->refused, 2U);
    CHECK(st->bytes > 0);
  }
  CHECK(stdfs::exists(dest / "usr/lib/x86_64-linux-gnu/libdemo.so.1.2.3"));
  CHECK(stdfs::exists(dest / "usr/include/demo/demo.h"));
  CHECK(!stdfs::exists(root / "escape.txt"));
  CHECK(!stdfs::exists("/abs/escape.txt"));
  const auto hdr = fs::read_file(dest / "usr/include/demo/demo.h");
  CHECK(hdr && hdr->starts_with("static inline"));

  // Not a .deb at all -> parse error, not a crash.
  CHECK(fs::write_file_atomic(root / "junk.deb", "hello").has_value());
  const auto bad = deb::LibarchiveExtractor{}.extract(root / "junk.deb", root / "y");
  CHECK(!bad);
  if (bad) {
    std::println(
      stderr, "junk.deb unexpectedly extracted: files={}",
      static_cast<unsigned long long>(bad->files)
    );
  } else {
    std::println(stderr, "junk.deb -> {}: {}", to_string(bad.error().code), bad.error().message);
    CHECK(bad.error().code == ErrorCode::parse);
  }

  // Missing file -> io error.
  const auto missing = deb::LibarchiveExtractor{}.extract(root / "nope.deb", root / "z");
  CHECK(!missing && missing.error().code == ErrorCode::io);

  // read_maybe_compressed handles xz and plain text alike.
  CHECK(fs::write_file_atomic(root / "plain.txt", "Package: a\n").has_value());
  const auto plain = deb::read_maybe_compressed(root / "plain.txt");
  CHECK(plain && *plain == "Package: a\n");

  return test::report("extract");
} catch (...) {
  static_cast<void>(std::fputs("extract: unexpected exception\n", stderr));
  return 1;
}
