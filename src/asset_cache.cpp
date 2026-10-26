#include "http_server/asset_cache.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "http_server/router.hpp"
#include "http_server/utils.hpp"

namespace http_server {

namespace {

constexpr const char* INLINE_404_BODY =
    "<html><body><h1>404 Not Found</h1></body></html>";

struct BodyStore {
    const AssetCacheLimits& limits;
    std::unordered_map<std::string, std::shared_ptr<const std::string>> bodies;
    std::size_t cached_bytes = 0;
};

std::shared_ptr<const std::string> makeBody(const std::string& data,
                                            BodyStore& store) {
    const std::size_t size = data.size();
    if (size > store.limits.max_total_bytes ||
        store.cached_bytes > store.limits.max_total_bytes - size) {
        throw std::runtime_error(
            "Static asset cache exceeds MAX_ASSET_CACHE_BYTES");
    }
    auto body = std::make_shared<const std::string>(data);
    store.cached_bytes += size;
    return body;
}

std::shared_ptr<const std::string> loadBody(const std::string& path,
                                            BodyStore& store) {
    const auto cached = store.bodies.find(path);
    if (cached != store.bodies.end()) return cached->second;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return nullptr;

    file.seekg(0, std::ios::end);
    const std::streampos end = file.tellg();
    if (end < 0) {
        throw std::runtime_error("Cannot determine size of asset '" + path +
                                 "'");
    }
    const std::streamoff stream_size = static_cast<std::streamoff>(end);
    if (stream_size < 0) {
        throw std::runtime_error("Invalid size for asset '" + path + "'");
    }
    const auto size = static_cast<std::uintmax_t>(stream_size);
    if (size > store.limits.max_asset_bytes) {
        throw std::runtime_error("Asset '" + path + "' is " +
                                 std::to_string(size) +
                                 " bytes, exceeding MAX_ASSET_BYTES (" +
                                 std::to_string(store.limits.max_asset_bytes) +
                                 ")");
    }
    if (size > store.limits.max_total_bytes ||
        store.cached_bytes > store.limits.max_total_bytes - size) {
        throw std::runtime_error(
            "Static asset cache exceeds MAX_ASSET_CACHE_BYTES");
    }

    std::string data(static_cast<std::size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    if (!data.empty()) {
        file.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) {
            throw std::runtime_error("Failed to read asset '" + path + "'");
        }
    }

    auto body = std::make_shared<const std::string>(std::move(data));
    store.cached_bytes += body->size();
    store.bodies.emplace(path, body);
    return body;
}

std::shared_ptr<const std::string> loadInlineFallback(BodyStore& store) {
    auto body = makeBody(INLINE_404_BODY, store);
    store.bodies.emplace("__inline_404__", body);
    return body;
}

std::string statusLine(int status_code) {
    switch (status_code) {
        case 200:
            return "200 OK";
        case 404:
            return "404 Not Found";
        default:
            return "500 Internal Server Error";
    }
}

CachedAsset makeAsset(int status_code, const std::string& content_type,
                      const std::shared_ptr<const std::string>& body) {
    CachedAsset asset;
    asset.status_code = status_code;
    asset.status_line = statusLine(status_code);
    asset.body = body;
    asset.header_prefix = "HTTP/1.1 " + asset.status_line +
                          "\r\nContent-Type: " + content_type +
                          "\r\nContent-Length: " +
                          std::to_string(body->size()) + "\r\n";
    return asset;
}

}  // namespace

AssetCache::AssetCache(const std::string& static_dir,
                       const AssetCacheLimits& limits) {
    BodyStore store{limits, {}, 0};

    auto fallback_body = loadBody(static_dir + "/404.html", store);
    if (!fallback_body) {
        fallback_body = loadInlineFallback(store);
    }
    fallback_ = makeAsset(404, "text/html", fallback_body);

    for (const std::string& path : allowlistedPaths()) {
        const Route route = routeRequest(path, static_dir);
        auto body = loadBody(route.file_path, store);
        if (!body) {
            // Missing allowlisted assets preserve the previous behavior:
            // serve the 404 body with status 404 instead of failing startup.
            assets_.emplace(path, fallback_);
            continue;
        }
        assets_.emplace(
            path,
            makeAsset(route.status_code, getContentType(route.file_path), body));
    }

    cached_bytes_ = store.cached_bytes;
}

const CachedAsset& AssetCache::lookup(const std::string& path) const {
    const auto found = assets_.find(path);
    return found == assets_.end() ? fallback_ : found->second;
}

}  // namespace http_server
