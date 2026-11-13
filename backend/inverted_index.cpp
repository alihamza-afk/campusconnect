// ---------------------------------------------------------------------------
// inverted_index.cpp - word -> ids, the other half of keyword search.
//
// "Inverted" means the map goes from a word to the documents that contain it,
// instead of from a document to its words. That is what makes a search cheap:
// find the word's bucket, and the answer is already a set of notice ids.
// ---------------------------------------------------------------------------
#include "inverted_index.h"

#include "text.h"

void InvertedIndex::insert(const std::string& text, int id) {
    for (const std::string& word : splitWords(text)) {
        byWord_[word].insert(id);
    }
}

void InvertedIndex::erase(const std::string& text, int id) {
    for (const std::string& word : splitWords(text)) {
        auto it = byWord_.find(word);
        if (it == byWord_.end()) continue;
        it->second.erase(id);
        if (it->second.empty()) byWord_.erase(it);   // do not keep empty buckets
    }
}

std::set<int> InvertedIndex::find(const std::string& word) const {
    const std::string cleaned = cleanWord(word);
    if (cleaned.empty()) return {};
    auto it = byWord_.find(cleaned);
    if (it == byWord_.end()) return {};
    return it->second;
}

std::size_t InvertedIndex::wordCount() const { return byWord_.size(); }
