#include "snapshot/client.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <curl/curl.h>

#include "core/contracts.hpp"
#include "core/fs.hpp"
#include "core/hash.hpp"

namespace abistudy::snapshot {
namespace {

/// @brief Process-wide request pacing state, reached through a function so
///        there is no mutable namespace-scope object.
struct RateState {
  std::once_flag curl_init;
  std::mutex mutex;
  std::chrono::steady_clock::time_point last_request;
};
RateState &rate_state() {
  static RateState s;
  return s;
}

/// @brief Blocks until `min_interval` has passed since the previous request.
void throttle(std::chrono::milliseconds min_interval) {
  auto &st = rate_state();
  const std::scoped_lock lk(st.mutex);
  const auto now = std::chrono::steady_clock::now();
  const auto wait = st.last_request + min_interval - now;
  if (wait.count() > 0)
    std::this_thread::sleep_for(wait);
  st.last_request = std::chrono::steady_clock::now();
}

std::size_t write_to_string(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

/// @brief Owns a CURL easy handle for one request.
struct Easy {
  CURL *h = curl_easy_init();
  ~Easy() {
    if (h)
      curl_easy_cleanup(h);
  }
  Easy(const Easy &) = delete ("owns a libcurl handle");
  Easy &operator=(const Easy &) = delete ("owns a libcurl handle");
  Easy() = default;
};

[[nodiscard]] bool retryable_status(long s) noexcept { return s == 429 || s == 408 || s >= 500; }

} // namespace

std::string url_encode_segment(std::string_view s) {
  static constexpr std::string_view hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    const bool unreserved = (std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' ||
                            c == '~' || c == '+' ||
                            c == ':'; // '+' and ':' are legal in Debian versions
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex.at(c >> 4U));
      out.push_back(hex.at(c & 15U));
    }
  }
  return out;
}

Result<Client> Client::create(Options opt) {
  ABISTUDY_EXPECTS(!opt.cache_dir.empty());
  std::call_once(rate_state().curl_init, [] {
    static_cast<void>(curl_global_init(CURL_GLOBAL_DEFAULT));
  });
  ABISTUDY_TRY_VOID(fs::ensure_dir(opt.cache_dir));
  return Client{std::move(opt)};
}

Result<Response> Client::get(std::string_view url) const {
  const std::string u{url};
  std::string last_err;
  long status = 0;
  for (int attempt = 1; attempt <= opt_.max_attempts; ++attempt) {
    throttle(opt_.min_interval);
    Easy e;
    if (!e.h)
      return fail(ErrorCode::network, "curl_easy_init failed");
    std::string body;
    std::array<char, CURL_ERROR_SIZE> errbuf{};
    curl_easy_setopt(e.h, CURLOPT_URL, u.c_str());
    curl_easy_setopt(e.h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e.h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(e.h, CURLOPT_USERAGENT, opt_.user_agent.c_str());
    curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, &write_to_string);
    curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(e.h, CURLOPT_ERRORBUFFER, errbuf.data());
    curl_easy_setopt(e.h, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(e.h, CURLOPT_LOW_SPEED_LIMIT, 1024L); // < 1 KiB/s ...
    curl_easy_setopt(e.h, CURLOPT_LOW_SPEED_TIME, 120L);   // ... for 2 min => abort
    curl_easy_setopt(e.h, CURLOPT_ACCEPT_ENCODING, "");
    const CURLcode rc = curl_easy_perform(e.h);
    if (rc == CURLE_OK) {
      curl_easy_getinfo(e.h, CURLINFO_RESPONSE_CODE, &status);
      if (status >= 200 && status < 300)
        return Response{.status = status, .body = std::move(body)};
      if (!retryable_status(status)) {
        return fail(
          status == 404 ? ErrorCode::not_found : ErrorCode::http_status, "GET {} -> HTTP {}", u,
          status
        );
      }
      last_err = std::format("HTTP {}", status);
    } else {
      last_err = errbuf[0] != 0 ? errbuf.data() : curl_easy_strerror(rc);
    }
    // Exponential backoff with a cap; 429 gets a longer floor.
    const auto base = status == 429 ? std::chrono::seconds(10) : std::chrono::seconds(1);
    std::this_thread::sleep_for(std::min(base * (1 << (attempt - 1)), std::chrono::seconds(60)));
  }
  return fail(
    status ? ErrorCode::http_status : ErrorCode::network, "GET {} failed after {} attempts: {}", u,
    opt_.max_attempts, last_err
  );
}

Result<Json> Client::api(std::string_view path) const {
  const auto cache_file = opt_.cache_dir / ("api_" + sha1_hex(path) + ".json");
  std::error_code ec;
  if (std::filesystem::exists(cache_file, ec)) {
    const auto age =
      std::chrono::file_clock::now() - std::filesystem::last_write_time(cache_file, ec);
    if (!ec && age < opt_.api_cache_ttl) {
      if (auto text = fs::read_file(cache_file)) {
        if (auto j = parse_json(*text, cache_file.string()))
          return j;
      }
    }
  }
  ABISTUDY_TRY(Response r, get(opt_.base_url + std::string{path}));
  ABISTUDY_TRY(Json j, parse_json(r.body, std::string{path}));
  (void)fs::write_file_atomic(cache_file, r.body); // cache failure is not an error
  return j;
}

Result<void> Client::download(const FileHash &hash, const std::filesystem::path &dest) const {
  ABISTUDY_EXPECTS(hash.get().size() == 40);
  std::error_code ec;
  if (std::filesystem::exists(dest, ec)) {
    if (auto data = fs::read_file(dest); data && sha1_hex(*data) == hash.get())
      return {};
    std::filesystem::remove(dest, ec);
  }
  const std::string url = opt_.base_url + "/file/" + hash.get();
  for (int attempt = 1; attempt <= 2; ++attempt) {
    ABISTUDY_TRY(Response r, get(url));
    if (sha1_hex(r.body) == hash.get())
      return fs::write_file_atomic(dest, r.body);
  }
  return fail(ErrorCode::integrity, "SHA-1 mismatch downloading {} (twice)", hash);
}

Result<std::optional<std::uint64_t>> Client::content_length(const FileHash &hash) const {
  ABISTUDY_EXPECTS(hash.get().size() == 40);
  const std::string url = opt_.base_url + "/file/" + hash.get();
  throttle(opt_.min_interval);
  Easy e;
  if (!e.h) {
    return fail(ErrorCode::network, "curl_easy_init failed");
  }
  curl_easy_setopt(e.h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(e.h, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(e.h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(e.h, CURLOPT_USERAGENT, opt_.user_agent.c_str());
  curl_easy_setopt(e.h, CURLOPT_CONNECTTIMEOUT, 30L);
  if (const CURLcode rc = curl_easy_perform(e.h); rc != CURLE_OK) {
    return fail(ErrorCode::network, "HEAD {}: {}", url, curl_easy_strerror(rc));
  }
  long status = 0;
  curl_easy_getinfo(e.h, CURLINFO_RESPONSE_CODE, &status);
  if (status == 404) {
    return fail(ErrorCode::not_found, "HEAD {} -> 404", url);
  }
  curl_off_t len = -1;
  curl_easy_getinfo(e.h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
  if (len < 0) {
    return std::optional<std::uint64_t>{};
  }
  return std::optional{static_cast<std::uint64_t>(len)};
}

Result<void> Client::fetch_to_file(
  std::string_view url, const std::filesystem::path &dest, std::chrono::seconds max_age
) const {
  std::error_code ec;
  if (std::filesystem::exists(dest, ec)) {
    const auto age = std::chrono::file_clock::now() - std::filesystem::last_write_time(dest, ec);
    if (!ec && age < max_age)
      return {};
  }
  ABISTUDY_TRY(Response r, get(url));
  return fs::write_file_atomic(dest, r.body);
}

} // namespace abistudy::snapshot
