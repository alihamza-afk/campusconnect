#include "text.h"

#include <cctype>

std::string cleanWord(const std::string& word) {
    std::string out;
    for (char c : word) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out += static_cast<char>(std::tolower(u));
    }
    return out;
}

std::vector<std::string> splitWords(const std::string& text, std::size_t maxWords) {
    // Split on every character that is not a letter or digit, not just spaces:
    // "CALCULATOR-case!" becomes "calculator" + "case", so punctuation cannot
    // glue two words together and make them unsearchable.
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            current += static_cast<char>(std::tolower(u));
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
            if (words.size() >= maxWords) return words;
        }
    }
    if (!current.empty() && words.size() < maxWords) words.push_back(std::move(current));
    return words;
}
