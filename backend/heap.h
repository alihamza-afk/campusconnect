#pragma once
#include <functional>
#include <utility>

template <typename T>
//classes and objects are used
class Heap {
    //pointer is used
    T* arr;
    int n = 0;
    int cap;
    std::function<bool(const T&, const T&)> before;

    void grow() {
        //uses dynamic memory allocation
        T* bigger = new T[cap * 2];
        for (int i = 0; i < n; i++) bigger[i] = arr[i];
        delete[] arr;
        arr = bigger;
        cap = cap * 2;
    }
//uses recursion
    void up(int i) {
        while (i > 0) {
            int p = (i - 1) / 2;
            if (!before(arr[i], arr[p])) break;
            std::swap(arr[i], arr[p]);
            i = p;
        }
    }

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
//uses constructor and deconstructor
public:
    Heap(std::function<bool(const T&, const T&)> f, int startCap = 16) : cap(startCap), before(f) {
        arr = new T[cap];
    }

    ~Heap() {
        delete[] arr;
    }

    Heap(const Heap&) = delete;
//uses operator overloading 
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
