#pragma once
// =============================================================================
// HTTP client for snapshot.debian.org: the machine-readable API and the
// content-addressed file store. Handles retries, throttling, on-disk API
// caching and integrity verification so that nothing above it thinks about
// the network. Large files stream to disk; nothing is buffered whole.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"
#include "core/types.hpp"
#include "domain/records.hpp"

namespace abistudy::snapshot {

/// @brief Tunables for Client.
struct ClientOptions {
  std::string base_url = "https://snapshot.debian.org";
  std::filesystem::path cache_dir; ///< Where API responses are cached; required.
  /// Age after which a cached API response is refetched. Long by default: a
  /// study's corpus must not drift while it runs (REVIEW.md §3.1).
  std::chrono::seconds api_cache_ttl{7 * 24 * 3600};
  int max_attempts = 6;                        ///< Per request, including the first.
  std::chrono::milliseconds min_interval{150}; ///< Politeness gap between requests, process-wide.
  std::string user_agent = "abistudy/4.0 (+https://github.com/tothambrus11/abi-breakage)";
};

/// @brief One HTTP response body plus status.
struct Response {
  long status;
  std::string body;
};

/// @brief snapshot.debian.org client.
/// @invariant Requests are rate-limited process-wide to ClientOptions::min_interval.
/// @thread    Safe to use from multiple threads; each request uses its own
///            libcurl handle and the rate limiter is mutex-protected.
class Client {
public:
  /// @brief Constructs a client and initialises libcurl (once per process).
  /// @pre   `opt.cache_dir` is non-empty.
  /// @errors io if the cache directory cannot be created.
  [[nodiscard]] static Result<Client> create(ClientOptions opt);

  /// @brief GET `url` into memory with retries on transport errors, 429 and 5xx.
  /// @post  On success status is 2xx and body is complete.
  /// @errors network after max_attempts transport failures; http_status if
  ///         the final answer is 4xx/5xx (404 maps to not_found).
  [[nodiscard]] Result<Response> get(std::string_view url) const;

  /// @brief GET a machine-readable API path (e.g. "/mr/package/zlib/")
  ///        and parse it as JSON, with on-disk caching subject to the TTL.
  /// @errors As get(), plus parse if the body is not JSON.
  [[nodiscard]] Result<Json> api(std::string_view path) const;

  /// @brief Downloads the content-addressed file `hash` to `dest`, streaming
  ///        to a temporary sibling and hashing incrementally.
  /// @post  On success `dest` exists and its SHA-1 equals `hash`; on failure
  ///        no partial file is left at `dest`.
  /// @errors As get(); integrity on hash mismatch after retries; io.
  [[nodiscard]] Result<void> download(
    const FileHash &hash, const std::filesystem::path &dest
  ) const;

  /// @brief Size of the content-addressed file `hash` from a HEAD request.
  /// @returns nullopt if the server does not report a length.
  /// @errors As get().
  [[nodiscard]] Result<std::optional<std::uint64_t>> content_length(const FileHash &hash) const;

  /// @brief Plain text GET (popcon, Packages index) written to `dest`,
  ///        reusing `dest` if younger than `max_age`.
  /// @errors As get(); io.
  [[nodiscard]] Result<void> fetch_to_file(
    std::string_view url, const std::filesystem::path &dest, std::chrono::seconds max_age
  ) const;

private:
  explicit Client(ClientOptions opt) : opt_(std::move(opt)) {}
  ClientOptions opt_;
};

/// @brief Percent-encodes one URL path segment (RFC 3986 unreserved kept).
[[nodiscard]] std::string url_encode_segment(std::string_view s);

} // namespace abistudy::snapshot
