#include "adapters/snapshot/archive_index.hpp"

#include <charconv>
#include <sstream>

#include "adapters/libarchive/extract.hpp"
#include "core/fs.hpp"

namespace abistudy::archive {

Result<std::vector<PopconEntry>> parse_popcon(const std::filesystem::path &by_inst) {
  ABISTUDY_TRY(std::string text, fs::read_file(by_inst));
  std::vector<PopconEntry> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream ls(line);
    std::string rank_s;
    std::string name;
    std::string inst_s;
    if (!(ls >> rank_s >> name >> inst_s))
      continue; // trailer lines
    std::uint32_t rank = 0;
    std::uint64_t inst = 0;
    if (std::from_chars(rank_s.data(), rank_s.data() + rank_s.size(), rank).ec != std::errc{})
      continue;
    if (std::from_chars(inst_s.data(), inst_s.data() + inst_s.size(), inst).ec != std::errc{})
      inst = 0;
    out.push_back(
      PopconEntry{
        .rank = PopconRank{rank}, .name = BinaryName{name}, .installs = InstallCount{inst}
      }
    );
  }
  return out;
}

Result<DependsIndex> parse_packages_depends(const std::filesystem::path &packages_file) {
  ABISTUDY_TRY(std::string text, deb::read_maybe_compressed(packages_file));
  DependsIndex out;
  std::istringstream in(text);
  std::string line;
  std::string current;
  while (std::getline(in, line)) {
    if (line.starts_with("Package: ")) {
      current = line.substr(9);
    } else if (line.starts_with("Depends: ") && !current.empty()) {
      out[BinaryName{current}] = line.substr(9);
    } else if (line.empty()) {
      current.clear();
    }
  }
  return out;
}

} // namespace abistudy::archive
