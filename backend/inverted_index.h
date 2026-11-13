#pragma once
// ---------------------------------------------------------------------------
// inverted_index.h - DECLARATIONS for exact-word search (word -> notice ids).
//
// The trie answers "starts with"; this answers "contains this word". Together
// they cover both jobs:
//
//   query "cal"    -> trie prefix    -> calculator, calendar
//   query "wallet" -> inverted index -> notices whose title has "wallet"
//
// The structure is a hash map of word -> set of ids, which is O(1) per word to
// build and to look up.
// ---------------------------------------------------------------------------
#include <set>
#include <string>
#include <unordered_map>

class InvertedIndex {
    std::unordered_map<std::string, std::set<int>> byWord_;

public:
    void insert(const std::string& text, int id);
    void erase(const std::string& text, int id);

    // Ids whose text contains this word exactly (the word is cleaned first).
    std::set<int> find(const std::string& word) const;

    // Number of distinct words indexed (tests / observation).
    std::size_t wordCount() const;
};
