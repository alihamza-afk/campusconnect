#pragma once
// ---------------------------------------------------------------------------
// trie.h - DECLARATIONS for the prefix tree over notice title words.
//
// Phase 7 refactor: tokenisation moved to text.h so the trie and the inverted
// index agree on what a word is. The trie now also exposes prefix(cleanedWord)
// so a caller (Store) can combine prefix matches with exact-word matches from
// the inverted index.
// ---------------------------------------------------------------------------
#include <map>
#include <set>
#include <string>

class Trie {
    struct Node {
        std::map<char, Node*> kids;   // next character -> child node
        std::set<int> ids;            // notice ids whose title passes through here
    };

    Node* root;

    void freeNode(Node* node);

public:
    Trie();
    ~Trie();

    // Owns raw Node pointers: copying would double-free them.
    Trie(const Trie&) = delete;
    Trie& operator=(const Trie&) = delete;

    void insert(const std::string& title, int id);
    void erase(const std::string& title, int id);

    // Ids whose title has a word starting with this already-cleaned prefix.
    std::set<int> prefix(const std::string& cleanedWord) const;

    // Convenience: split the query into words, AND their prefix matches.
    std::set<int> search(const std::string& query) const;
};
