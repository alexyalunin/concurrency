#pragma once

#include <stack>
#include <mutex>
#include <hazard_ptr.h>

template <class T>
class Stack {
public:
    struct Node {
        T value;
        Node* next;
    };
    void Push(const T& value) {
        auto new_head = new Node{value, head_.load()};
        while (!head_.compare_exchange_weak(new_head->next, new_head)) {
        }
    }

    bool Pop(T* value) {
        // Protect the head of the stack with a hazard pointer
        Node* old_head = Acquire(&head_);

        // If the stack is empty, return false
        if (old_head == nullptr) {
            Release();
            return false;
        }

        // Otherwise, remove the head of the stack
        if (head_.compare_exchange_strong(old_head, old_head->next)) {
            // Mark the old head as retired with a hazard pointer
            Retire(old_head);
            if (value != nullptr) {
                *value = old_head->value;
            }
            return true;
        } else {
            Release();
            return false;
        }
    }

    void Clear() {
        T* val = nullptr;
        while (head_ != nullptr) {
            Pop(val);
        }
        Retire(val);
        Release();
        ScanFreeList();
    }

private:
    std::atomic<Node*> head_ = nullptr;
};
