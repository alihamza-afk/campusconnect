#pragma once
#include <optional>
#include <string>

struct SniffedImage {
    std::string mime;
    std::string ext;
};

std::optional<SniffedImage> sniffImage(const std::string& bytes);
std::string extForMime(const std::string& mime);
