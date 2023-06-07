#pragma once

#include <atomic>
#include <functional>
#include <shared_mutex>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <memory>

thread_local std::atomic<void*> hazard_ptr{nullptr};

class ThreadState {
public:
    ThreadState(std::atomic<void*>* p_tomic) : ptr(p_tomic) {
    }
    std::atomic<void*>* ptr;
};

std::mutex threads_lock;
std::unordered_set<ThreadState*> threads;

struct RetiredPtr {
    void* value;
    std::function<void()> deleter;
    RetiredPtr* next;
};
std::atomic<RetiredPtr*> free_list = nullptr;
std::atomic<int> approximate_free_list_size = 0;

std::mutex scan_lock;

// Each thread must call RegisterThread before working with Hazard Ptr.
inline void RegisterThread() {
    std::lock_guard<std::mutex> lock(threads_lock);
    threads.insert(new ThreadState(&hazard_ptr));
}

// UnregisterThread is called by each thread before terminating. You cannot work with Hazard Ptr
// after calling UnregisterThread.
inline void UnregisterThread() {
    std::lock_guard<std::mutex> lock(threads_lock);
    for (auto ptr : threads) {
        if (ptr->ptr == &hazard_ptr) {
            threads.erase(ptr);
            delete ptr;
            break;
        }
    }

    if (threads.empty()) {
        while (free_list.load()) {
            free_list.load()->deleter();
            auto retired = free_list.load();
            free_list = free_list.load()->next;
            delete retired;
        }
    }
}

template <class T>
T* Acquire(std::atomic<T*>* ptr) {
    auto value = ptr->load();  // (2)

    do {
        hazard_ptr.store(value);

        auto new_value = ptr->load();  // (3)
        if (new_value == value) {      // (1)
            return value;
        }

        value = new_value;
    } while (true);
}

inline void Release() {
    hazard_ptr.store(nullptr);
}

void ScanFreeList() {
    // (0) Set approximate_free_list_size to zero so that other threads don't try to enter the
    // ScanFreeList with us.
    approximate_free_list_size.store(0);

    // (1) Using the mutex, make sure that no more than one thread is scanning.
    // In real code, don't forget to use guard.
    //    if (!scan_lock.try_lock()) {
    //        return;
    //    }
    std::lock_guard guard2{scan_lock};

    // (2) Get all pointers from free_list
    auto* retired = free_list.exchange(nullptr);

    // (3) Read a set of protected pointers, bypassing all ThreadStates.
    std::vector<void*> hazard;
    {
        std::lock_guard guard{threads_lock};
        for (auto* thread : threads) {
            if (auto* ptr = thread->ptr->load()) {
                hazard.push_back(ptr);
            }
        }
    }

    // (4) Scan all retired pointers.
    int c = 0;
    while (retired) {
        RetiredPtr* next = retired->next;
        bool is_hazard = false;

        // (a) Check if the current pointer is in hazard.
        for (void* h : hazard) {
            if (retired->value == h) {
                is_hazard = true;
                break;
            }
        }

        // (b) If not in hazard, call the destructor and free the memory under RetiredPtr.
        if (!is_hazard) {
            retired->deleter();
            delete retired;
        } else {
            // (c) If still in hazard, put back in the free_list.
            retired->next = free_list.load();
            while (!free_list.compare_exchange_weak(retired->next, retired)) {
                retired->next = free_list.load();
            }
            c += 1;
        }
        retired = next;
    }
    approximate_free_list_size.fetch_add(c);
}

template <class T, class Deleter = std::default_delete<T>>
void Retire(T* value, Deleter deleter = {}) {
    // 1) Add ptr to free list.
    auto ptr = new RetiredPtr{value, [deleter, value] { deleter(value); }, free_list.load()};

    while (!free_list.compare_exchange_weak(ptr->next, ptr)) {
        // 2) If CAS fails, try again.
    }
    // 2) Increment free list size.
    approximate_free_list_size.fetch_add(1, std::memory_order_relaxed);
    // 3) Scan free list ScanFreeList(), if size > K.
    if (approximate_free_list_size.load() > 10) {
        ScanFreeList();
    }
}
