#pragma once
#include <map>
#include <set>
#include <string>
#include <sstream>
#include <cctype>

class Trie {
    struct Node {
        std::map<char, Node*> kids;
        std::set<int> ids;
    };

    static const int MAX_WORDS = 20;
    Node* root;

    void freeNode(Node* node) {
        for (auto& p : node->kids) freeNode(p.second);
        delete node;
    }

    std::string clean(const std::string& w) const {
        std::string res;
        for (char c : w) {
            if (std::isalnum((unsigned char)c)) res += (char)std::tolower((unsigned char)c);
        }
        return res;
    }

    int splitWords(const std::string& text, std::string words[]) const {
        int count = 0;
        std::istringstream in(text);
        std::string w;
        while (count < MAX_WORDS && in >> w) {
            w = clean(w);
            if (!w.empty()) {
                words[count] = w;
                count++;
            }
        }
        return count;
    }

public:
    Trie() {
        root = new Node();
    }

    ~Trie() {
        freeNode(root);
    }

    Trie(const Trie&) = delete;
    Trie& operator=(const Trie&) = delete;

    void insert(const std::string& title, int id) {
        std::string words[MAX_WORDS];
        int count = splitWords(title, words);
        for (int i = 0; i < count; i++) {
            Node* cur = root;
            for (char c : words[i]) {
                if (cur->kids.count(c) == 0) cur->kids[c] = new Node();
                cur = cur->kids[c];
                cur->ids.insert(id);
            }
        }
    }

    void erase(const std::string& title, int id) {
        std::string words[MAX_WORDS];
        int count = splitWords(title, words);
        for (int i = 0; i < count; i++) {
            Node* cur = root;
            for (char c : words[i]) {
                if (cur->kids.count(c) == 0) break;
                cur = cur->kids[c];
                cur->ids.erase(id);
            }
        }
    }

    std::set<int> search(const std::string& query) const {
        std::string words[MAX_WORDS];
        int count = splitWords(query, words);
        std::set<int> result;

        for (int i = 0; i < count; i++) {
            const Node* cur = root;
            for (char c : words[i]) {
                auto it = cur->kids.find(c);
                if (it == cur->kids.end()) return std::set<int>();
                cur = it->second;
            }

            if (i == 0) {
                result = cur->ids;
            } else {
                std::set<int> common;
                for (int id : result) {
                    if (cur->ids.count(id)) common.insert(id);
                }
                result = common;
            }
        }
        return result;
    }
};
