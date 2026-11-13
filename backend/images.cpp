#include "images.h"

namespace {
bool startsWith(const std::string& bytes, const char* prefix, std::size_t n) {
    if (bytes.size() < n) return false;
    for (std::size_t i = 0; i < n; i++) {
        if (static_cast<unsigned char>(bytes[i]) != static_cast<unsigned char>(prefix[i])) return false;
    }
    return true;
}
}

std::optional<SniffedImage> sniffImage(const std::string& bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xD8 && static_cast<unsigned char>(bytes[2]) == 0xFF) {
        return SniffedImage{"image/jpeg", ".jpg"};
    }
    if (startsWith(bytes, "\x89PNG\r\n\x1a\n", 8)) return SniffedImage{"image/png", ".png"};
    if (startsWith(bytes, "GIF87a", 6) || startsWith(bytes, "GIF89a", 6)) {
        return SniffedImage{"image/gif", ".gif"};
    }
    if (bytes.size() >= 12 && startsWith(bytes, "RIFF", 4) && startsWith(bytes.substr(8), "WEBP", 4)) {
        return SniffedImage{"image/webp", ".webp"};
    }
    return std::nullopt;
}

std::string extForMime(const std::string& mime) {
    if (mime == "image/jpeg") return ".jpg";
    if (mime == "image/png") return ".png";
    if (mime == "image/gif") return ".gif";
    if (mime == "image/webp") return ".webp";
    return "";
}
