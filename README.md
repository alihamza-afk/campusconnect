# CampusConnect (Part 1 of 2, about 40% of the project)

Frontend: HTML, CSS, JavaScript (frontend/)
Backend: C++17 with DSA (backend/)

## Run
1. Download `httplib.h` (cpp-httplib) and `json.hpp` (nlohmann/json) into backend/
2. cd backend && g++ -std=c++17 main.cpp -o campusconnect -pthread
3. ./campusconnect  and open http://localhost:8080
(Without the backend, opening frontend/index.html still shows a demo with sample data.)

## DSA used
- HashTable (chaining): item lookup by id
- Trie: prefix search on title words
- Heap (plain array that grows) with custom comparator: newest / cheapest / dearest ordering
