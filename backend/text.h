#pragma once
// ---------------------------------------------------------------------------
// text.h - one place that decides what a "word" is.
//
// The trie (prefix search), the inverted index (exact-word search) and the
// store (which splits a query) must agree on tokenisation, or a word could be
// indexed one way and searched another. So the rules live here:
//   * lowercase
//   * keep only letters and digits
//   * drop empty words
//   * at most maxWords words
// ---------------------------------------------------------------------------
#include <cstddef>
#include <string>
#include <vector>

// Cleans one word (lowercase, alphanumeric only).
std::string cleanWord(const std::string& word);

// Splits text into cleaned words, dropping empties, capped at maxWords.
std::vector<std::string> splitWords(const std::string& text, std::size_t maxWords = 20);
