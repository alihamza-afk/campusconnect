// ---------------------------------------------------------------------------
// trie.cpp - prefix search over notice titles.
//
// Each node stores the set of notice ids whose title words passed through it,
// so a search for "cal" returns every notice with a title word starting "cal"
// (calculator, calendar, ...). Multiple query words are intersected (AND).
//
// The trie is good at prefixes; the inverted index (inverted_index.cpp) is good
// at whole words. Store combines the two: for each query word it unions the
// exact matches and the prefix matches, then ANDs across words.
// ---------------------------------------------------------------------------
#include "trie.h"

#include "text.h"

Trie::Trie() {
    root = new Node();
}

Trie::~Trie() { freeNode(root); }

void Trie::freeNode(Node* node) {
    for (auto& child : node->kids) freeNode(child.second);
    delete node;
}

void Trie::insert(const std::string& title, int id) {
    for (const std::string& word : splitWords(title)) {
        Node* cur = root;
        for (char c : word) {
            if (cur->kids.count(c) == 0) cur->kids[c] = new Node();
            cur = cur->kids[c];
            cur->ids.insert(id);
        }
    }
}

void Trie::erase(const std::string& title, int id) {
    for (const std::string& word : splitWords(title)) {
        Node* cur = root;
        for (char c : word) {
            auto it = cur->kids.find(c);
            if (it == cur->kids.end()) break;
            cur = it->second;
            cur->ids.erase(id);
        }
    }
}

std::set<int> Trie::prefix(const std::string& cleanedWord) const {
    const Node* cur = root;
    for (char c : cleanedWord) {
        auto it = cur->kids.find(c);
        if (it == cur->kids.end()) return {};   // no such prefix
        cur = it->second;
    }
    return cur->ids;
}

std::set<int> Trie::search(const std::string& query) const {
    const std::vector<std::string> words = splitWords(query);
    std::set<int> result;
    for (std::size_t i = 0; i < words.size(); i++) {
        const std::set<int> ids = prefix(words[i]);
        if (i == 0) {
            result = ids;
        } else {
            std::set<int> common;
            for (int id : result) {
                if (ids.count(id)) common.insert(id);
            }
            result = common;
        }
    }
    return result;
}
