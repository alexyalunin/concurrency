#pragma once

#include <chrono>
#include <coroutine>
#include <functional>
#include <queue>
#include <thread>
#include <list>
#include <iostream>

using TimePoint = std::chrono::system_clock::time_point;
using Duration = std::chrono::system_clock::duration;

class Coroutine {
public:
    struct Promise;

    class Awaiter {
    public:
        explicit Awaiter(TimePoint tp) : tp_{tp} {
        }

        bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<Coroutine::Promise>) {
        }

        void await_resume() const noexcept {
        }

    private:
        TimePoint tp_;
    };

    struct Promise {
        Coroutine get_return_object() {
            return Coroutine{Coroutine::Handle::from_promise(*this)};
        }
        std::suspend_always initial_suspend() {
            return std::suspend_always{};
        }
        std::suspend_always final_suspend() noexcept {
            return std::suspend_always{};
        }
        void unhandled_exception() {
        }
        void return_void() {
        }
        Awaiter await_transform(TimePoint tp) {
            resumeTime = tp;
            return Awaiter(tp);
        }
        Awaiter await_transform(Duration duration) {
            return await_transform(std::chrono::system_clock::now() + duration);
        }

        TimePoint resumeTime;
    };

    using Handle = std::coroutine_handle<Promise>;
    using promise_type = Promise;

    explicit Coroutine(Handle handle) : handle_{handle} {
    }

    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    Coroutine(Coroutine&& other) : handle_{other.handle_} {
        other.handle_ = nullptr;
    }
    Coroutine& operator=(Coroutine&& other) {
        if (this != &other) {
            Clear();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Coroutine() {
        Clear();
    }

    void Resume() const {
        handle_.resume();
    }

    bool Done() const {
        return handle_.done();
    }

    void Clear() {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    TimePoint ResumeTime() const {
        return handle_.promise().resumeTime;
    }

private:
    Handle handle_;
};

using Pair = std::pair<Coroutine, int>;

class Scheduler {
public:
    template <class... Args>
    void AddTask(auto&& task, Args&&... args) {
        int order_c = order_;
        not_started_coroutines_.emplace_back(task(std::forward<Args>(args)...), order_c);
        order_ += 1;
    }

    bool Step() {
        if (coroutines_.empty() && not_started_coroutines_.empty()) {
            return false;
        }
        if (!not_started_coroutines_.empty()) {
            Pair c = std::move(not_started_coroutines_.front());
            not_started_coroutines_.pop_front();

            c.first.Resume();
            if (!c.first.Done()) {
                coroutines_.push(std::move(c));
            }
        } else {
            Pair c = std::move(const_cast<Pair&>(coroutines_.top()));
            coroutines_.pop();
            std::this_thread::sleep_until(c.first.ResumeTime());

            c.first.Resume();
            if (!c.first.Done()) {
                coroutines_.push(std::move(c));
            }
        }
        return true;
    }

    void Run() {
        while (Step()) {
        }
    }

private:
    int order_ = 0;
    std::list<Pair> not_started_coroutines_;

    std::priority_queue<Pair, std::vector<Pair>, std::function<bool(const Pair&, const Pair&)>>
        coroutines_{[](const Pair& a, const Pair& b) {
            if (a.first.ResumeTime() == b.first.ResumeTime()) {
                return a.second > b.second;
            }
            return a.first.ResumeTime() > b.first.ResumeTime();
        }};
};
