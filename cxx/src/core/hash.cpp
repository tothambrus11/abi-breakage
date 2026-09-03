#include "core/hash.hpp"

#include <array>
#include <string_view>

#include <openssl/evp.h>

#include "core/contracts.hpp"

namespace abistudy {

std::string sha1_hex(std::string_view data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
  unsigned int len = 0;
  EVP_Digest(data.data(), data.size(), md.data(), &len, EVP_sha1(), nullptr);
  static constexpr std::string_view hex = "0123456789abcdef";
  std::string out;
  out.reserve(static_cast<std::size_t>(len) * 2);
  for (unsigned i = 0; i < len; ++i) {
    out.push_back(hex.at(md.at(i) >> 4U));
    out.push_back(hex.at(md.at(i) & 15U));
  }
  ABISTUDY_ENSURES(out.size() == 40);
  return out;
}

std::string fingerprint(std::string_view data) { return sha1_hex(data).substr(0, 16); }

} // namespace abistudy
