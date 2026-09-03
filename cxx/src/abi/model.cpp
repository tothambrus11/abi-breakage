#include "abi/model.hpp"

#include <string>

namespace abistudy {

void to_json(nlohmann::json &j, const ChangeCounts &c) {
  j = nlohmann::json::object();
  for (const auto &[k, n] : c.items())
    j[std::string{to_string(k)}] = n;
}

void from_json(const nlohmann::json &j, ChangeCounts &c) {
  c = ChangeCounts{};
  for (const auto &[name, n] : j.items()) {
    const auto k = parse_change_kind(name);
    if (!k)
      throw nlohmann::json::other_error::create(501, "unknown change kind '" + name + "'", &j);
    c.add(*k, n.get<std::uint32_t>());
  }
}

} // namespace abistudy
