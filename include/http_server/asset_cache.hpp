#ifndef HTTP_SERVER_ASSET_CACHE_HPP
#define HTTP_SERVER_ASSET_CACHE_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

namespace http_server {

struct AssetCacheLimits {
    // Maximum size of one asset body, in bytes.
    std::size_t max_asset_bytes;
    // Maximum total bytes of unique asset bodies.
    std::size_t max_total_bytes;
};

/*
 * Immutable preloaded response for one allowlisted path. The body is shared;
 * lookups copy only the small metadata/header prefix.
 */
struct CachedAsset {
    int status_code = 404;
    std::string status_line;
    std::string header_prefix;
    std::shared_ptr<const std::string> body;
};

/*
 * Read-only cache built once at startup. All lookups are const and require no
 * mutex. Bodies are shared through shared_ptr and are never copied per
 * request. Reload requires a server restart.
 */
class AssetCache {
   public:
    AssetCache(const std::string& static_dir, const AssetCacheLimits& limits);

    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    // Unknown paths return the 404 fallback entry.
    const CachedAsset& lookup(const std::string& path) const;

    std::size_t cachedAssetCount() const { return assets_.size(); }
    std::size_t cachedBytes() const { return cached_bytes_; }

   private:
    std::unordered_map<std::string, CachedAsset> assets_;
    CachedAsset fallback_;
    std::size_t cached_bytes_ = 0;
};

}  // namespace http_server

#endif  // HTTP_SERVER_ASSET_CACHE_HPP
