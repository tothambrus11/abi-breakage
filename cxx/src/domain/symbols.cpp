#include "domain/symbols.hpp"

#include <cctype>

namespace abistudy {

SonameStem soname_stem(std::string_view s) {
  if (const auto slash = s.rfind('/'); slash != std::string_view::npos)
    s.remove_prefix(slash + 1);
  if (const auto so = s.find(".so"); so != std::string_view::npos)
    s = s.substr(0, so);
  return SonameStem{std::string{s}};
}

bool is_private_version_node(std::string_view node) noexcept {
  std::string up;
  up.reserve(node.size());
  for (const char c : node)
    up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  return up.contains("PRIVATE") || up.contains("INTERNAL");
}

bool is_linkable_library_dir(std::string_view relative_dir) noexcept {
  while (relative_dir.starts_with('/'))
    relative_dir.remove_prefix(1);
  while (relative_dir.ends_with('/'))
    relative_dir.remove_suffix(1);
  if (relative_dir.starts_with("usr/"))
    relative_dir.remove_prefix(4);
  std::string_view head = relative_dir;
  std::string_view tail;
  if (const auto slash = relative_dir.find('/'); slash != std::string_view::npos) {
    head = relative_dir.substr(0, slash);
    tail = relative_dir.substr(slash + 1);
  }
  if (head != "lib" && head != "lib64" && head != "lib32" && head != "libx32")
    return false;
  if (tail.empty())
    return true;
  // One multiarch triplet: "x86_64-linux-gnu", "i386-linux-gnu", "aarch64-linux-gnu".
  return !tail.contains('/') && tail.contains("-linux-");
}

std::string digits_blind(std::string_view s) {
  std::string out;
  bool in_digits = false;
  for (const char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      if (!in_digits)
        out.push_back('#');
      in_digits = true;
    } else {
      out.push_back(c);
      in_digits = false;
    }
  }
  return out;
}

} // namespace abistudy
