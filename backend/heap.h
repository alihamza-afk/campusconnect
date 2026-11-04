#pragma once
#include <functional>
#include <utility>

// Class template: one Heap class works for any element type T.
template <typename T>
class Heap {
    // Raw pointer to a heap-allocated array that this class owns.
    T* arr;
    int n = 0;
    int cap;
    std::function<bool(const T&, const T&)> before;

    void grow() {
        // Dynamic memory allocation: double the capacity, copy, free the old array.
        T* bigger = new T[cap * 2];
        for (int i = 0; i < n; i++) bigger[i] = arr[i];
        delete[] arr;
        arr = bigger;
        cap = cap * 2;
    }
    // Sift-up, written iteratively (a loop, not recursion): move arr[i]
    // towards the root while it should come before its parent. O(log n).
    void up(int i) {
        while (i > 0) {
            int p = (i - 1) / 2;
            if (!before(arr[i], arr[p])) break;
            std::swap(arr[i], arr[p]);
            i = p;
        }
    }

    // Sift-down, also iterative: swap arr[i] with whichever child should
    // come first until neither child does. O(log n).
    void down(int i) {
        while (true) {
            int l = 2 * i + 1;
            int r = 2 * i + 2;
            int best = i;
            if (l < n && before(arr[l], arr[best])) best = l;
            if (r < n && before(arr[r], arr[best])) best = r;
            if (best == i) break;
            std::swap(arr[i], arr[best]);
            i = best;
        }
    }
public:
    // Constructor allocates the array; destructor frees it (RAII).
    Heap(std::function<bool(const T&, const T&)> f, int startCap = 16) : cap(startCap), before(f) {
        arr = new T[cap];
    }

    ~Heap() {
        delete[] arr;
    }

    // Copying is disabled (= delete). A default copy would duplicate the
    // pointer, and both copies would later delete[] the same array.
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    bool empty() const {
        return n == 0;
    }

    void push(const T& x) {
        if (n == cap) grow();
        arr[n] = x;
        up(n);
        n++;
    }

    T pop() {
        T top = arr[0];
        n--;
        arr[0] = arr[n];
        if (n > 0) down(0);
        return top;
    }
};
