#include "adapters/snapshot/client.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <curl/curl.h>
#include <openssl/evp.h>

#include "adapters/fs/artifact_store.hpp"
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

// NOLINTNEXTLINE(misc-const-correctness): libcurl's callback signature
std::size_t write_to_string(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

/// @brief Streaming sink: bytes go to a FILE and through a SHA-1 context.
struct FileSink {
  std::FILE *file = nullptr;
  EVP_MD_CTX *md = nullptr;
  bool failed = false;
};

// NOLINTNEXTLINE(misc-const-correctness): libcurl's callback signature
std::size_t write_to_sink(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  auto *s = static_cast<FileSink *>(userdata);
  const auto n = size * nmemb;
  if (std::fwrite(ptr, 1, n, s->file) != n || EVP_DigestUpdate(s->md, ptr, n) != 1) {
    s->failed = true;
    return 0; // makes curl abort with CURLE_WRITE_ERROR
  }
  return n;
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
  Easy(Easy &&) = delete ("owns a libcurl handle");
  Easy &operator=(Easy &&) = delete ("owns a libcurl handle");
  Easy() = default;
};

[[nodiscard]] bool retryable_status(long s) noexcept { return s == 429 || s == 408 || s >= 500; }

void common_options(CURL *h, const std::string &url, const ClientOptions &opt) {
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(h, CURLOPT_USERAGENT, opt.user_agent.c_str());
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L); // < 1 KiB/s ...
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 120L);   // ... for 2 min => abort
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
}

std::chrono::seconds backoff(long status, int attempt) {
  const auto base = status == 429 ? std::chrono::seconds(10) : std::chrono::seconds(1);
  auto delay = base;
  for (int i = 1; i < attempt && delay < std::chrono::seconds(60); ++i)
    delay *= 2;
  return std::min(delay, std::chrono::seconds(60));
}

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

Result<Client> Client::create(ClientOptions opt) {
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
    common_options(e.h, u, opt_);
    curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, &write_to_string);
    curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(e.h, CURLOPT_ERRORBUFFER, errbuf.data());
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
    std::this_thread::sleep_for(backoff(status, attempt));
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
        if (auto j = fsstore::parse_json(*text, cache_file.string()))
          return j;
      }
    }
  }
  ABISTUDY_TRY(Response r, get(opt_.base_url + std::string{path}));
  ABISTUDY_TRY(Json j, fsstore::parse_json(r.body, path));
  static_cast<void>(fs::write_file_atomic(cache_file, r.body)); // cache failure is not an error
  return j;
}

Result<void> Client::download(const FileHash &hash, const std::filesystem::path &dest) const {
  ABISTUDY_EXPECTS(hash.get().size() == 40);
  std::error_code ec;
  std::filesystem::remove(dest, ec);
  if (dest.has_parent_path())
    ABISTUDY_TRY_VOID(fs::ensure_dir(dest.parent_path()));
  const std::string url = opt_.base_url + "/file/" + hash.get();
  const auto tmp = dest.string() + ".part";
  std::string last_err;
  long status = 0;
  for (int attempt = 1; attempt <= opt_.max_attempts; ++attempt) {
    throttle(opt_.min_interval);
    Easy e;
    if (!e.h)
      return fail(ErrorCode::network, "curl_easy_init failed");
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
      return fail(ErrorCode::io, "cannot create '{}': {}", tmp, fs::errno_text(errno));
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md, EVP_sha1(), nullptr);
    FileSink sink{.file = f, .md = md, .failed = false};
    std::array<char, CURL_ERROR_SIZE> errbuf{};
    common_options(e.h, url, opt_);
    curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, &write_to_sink);
    curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(e.h, CURLOPT_ERRORBUFFER, errbuf.data());
    const CURLcode rc = curl_easy_perform(e.h);
    const bool closed_ok = std::fclose(f) == 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int len = 0;
    EVP_DigestFinal_ex(md, digest.data(), &len);
    EVP_MD_CTX_free(md);
    if (rc == CURLE_OK && closed_ok && !sink.failed) {
      curl_easy_getinfo(e.h, CURLINFO_RESPONSE_CODE, &status);
      if (status >= 200 && status < 300) {
        static constexpr std::string_view hex = "0123456789abcdef";
        std::string got;
        for (unsigned i = 0; i < len; ++i) {
          got.push_back(hex[digest.at(i) >> 4U]);
          got.push_back(hex[digest.at(i) & 15U]);
        }
        if (got == hash.get()) {
          if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
            std::filesystem::remove(tmp, ec);
            return fail(ErrorCode::io, "rename '{}': {}", tmp, fs::errno_text(errno));
          }
          return {};
        }
        last_err = "SHA-1 mismatch";
      } else if (!retryable_status(status)) {
        std::filesystem::remove(tmp, ec);
        return fail(
          status == 404 ? ErrorCode::not_found : ErrorCode::http_status, "GET {} -> HTTP {}", url,
          status
        );
      } else {
        last_err = std::format("HTTP {}", status);
      }
    } else if (sink.failed || !closed_ok) {
      std::filesystem::remove(tmp, ec);
      return fail(ErrorCode::io, "writing '{}' failed", tmp);
    } else {
      last_err = errbuf[0] != 0 ? errbuf.data() : curl_easy_strerror(rc);
    }
    std::filesystem::remove(tmp, ec);
    std::this_thread::sleep_for(backoff(status, attempt));
  }
  ErrorCode code = ErrorCode::network;
  if (last_err == "SHA-1 mismatch") {
    code = ErrorCode::integrity;
  } else if (status != 0) {
    code = ErrorCode::http_status;
  }
  return fail(code, "download {} failed after {} attempts: {}", hash, opt_.max_attempts, last_err);
}

Result<std::optional<std::uint64_t>> Client::content_length(const FileHash &hash) const {
  ABISTUDY_EXPECTS(hash.get().size() == 40);
  const std::string url = opt_.base_url + "/file/" + hash.get();
  throttle(opt_.min_interval);
  Easy e;
  if (!e.h) {
    return fail(ErrorCode::network, "curl_easy_init failed");
  }
  common_options(e.h, url, opt_);
  curl_easy_setopt(e.h, CURLOPT_NOBODY, 1L);
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
