#include "app/materialize.hpp"

#include <algorithm>
#include <array>
#include <fstream>

#include "core/contracts.hpp"

namespace abistudy::app {
namespace {

/// @brief Known binary versions for `name` in this release, newest first.
const std::vector<VersionString> *versions_of(const Release &r, const BinaryName &name) {
  for (const auto &b : r.binaries) {
    if (b.name == name)
      return &b.versions;
  }
  return nullptr;
}

/// @brief Downloads one binary package (first version that has an amd64/all
///        file) and extracts it under `dest`. Returns false if no file exists.
Result<bool> fetch_one(
  const Services &sv, const Release &r, const BinaryName &name,
  const std::filesystem::path &scratch, const std::filesystem::path &dest, std::uint64_t &bytes
) {
  const auto *vers = versions_of(r, name);
  if (!vers)
    return false;
  for (const auto &v : *vers) {
    ABISTUDY_TRY(auto hash, ports::amd64_deb_hash(sv.packages, name, v));
    if (!hash)
      continue;
    const auto deb = scratch / (name.get() + ".deb");
    ABISTUDY_TRY_VOID(sv.packages.download(*hash, deb));
    auto st = sv.extractor.extract(deb, dest);
    std::error_code ec;
    std::filesystem::remove(deb, ec);
    if (!st)
      return forward_error(st);
    bytes += st->bytes;
    return true;
  }
  return false;
}

bool has_elf_magic(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::array<char, 4> m{};
  return in.read(m.data(), 4) && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}

} // namespace

std::vector<BinaryName> packages_for(const Release &rel, Want want) {
  std::vector<BinaryName> names;
  for (const auto &pkg : rel.runtime) {
    if (want.runtime)
      names.push_back(pkg);
    if (want.dbgsym)
      names.emplace_back(pkg.get() + "-dbgsym");
  }
  if (want.dev)
    names.insert(names.end(), rel.dev.begin(), rel.dev.end());
  return names;
}

std::vector<std::filesystem::path> find_shared_objects(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_symlink(ec) || !it->is_regular_file(ec))
      continue;
    const auto name = it->path().filename().string();
    if (!name.starts_with("lib") || name.find(".so") == std::string::npos)
      continue;
    if (has_elf_magic(it->path()))
      out.push_back(it->path());
  }
  std::ranges::sort(out);
  return out;
}

Result<Materialized> materialize(
  const Services &sv, const Release &rel, Want want, const std::filesystem::path &scratch_base
) {
  ABISTUDY_EXPECTS(!scratch_base.empty());
  ABISTUDY_TRY(fs::TempDir scratch, fs::TempDir::create(scratch_base, "rel-"));
  Materialized m{
    .scratch = std::move(scratch),
    .runtime_root = {},
    .debug_root = {},
    .include_root = {},
    .shared_objects = {},
    .bytes_extracted = 0,
    .missing = {}
  };
  const auto &root = m.scratch.path();
  const auto rt = root / "rt";
  const auto dbg = root / "dbg";
  const auto dev = root / "dev";

  for (const auto &pkg : rel.runtime) {
    if (want.runtime) {
      ABISTUDY_TRY(bool ok, fetch_one(sv, rel, pkg, root, rt, m.bytes_extracted));
      if (!ok)
        m.missing.push_back(pkg.get());
    }
    if (want.dbgsym) {
      const BinaryName ds{pkg.get() + "-dbgsym"};
      ABISTUDY_TRY(bool ok, fetch_one(sv, rel, ds, root, dbg, m.bytes_extracted));
      if (!ok)
        m.missing.push_back(ds.get());
    }
  }
  if (want.dev) {
    for (const auto &pkg : rel.dev) {
      ABISTUDY_TRY(bool ok, fetch_one(sv, rel, pkg, root, dev, m.bytes_extracted));
      if (!ok)
        m.missing.push_back(pkg.get());
    }
  }
  std::error_code ec;
  if (std::filesystem::is_directory(rt, ec)) {
    m.runtime_root = rt;
    m.shared_objects = find_shared_objects(rt);
  }
  if (const auto d = dbg / "usr" / "lib" / "debug"; std::filesystem::is_directory(d, ec))
    m.debug_root = d;
  if (const auto i = dev / "usr" / "include"; std::filesystem::is_directory(i, ec))
    m.include_root = i;
  return m;
}

} // namespace abistudy::app
