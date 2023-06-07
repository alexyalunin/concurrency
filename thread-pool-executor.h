#include <memory>
#include <chrono>
#include <vector>
#include <functional>
#include <iostream>
#include <thread>
#include <queue>
#include <optional>
#include <mutex>
#include <condition_variable>

template <class T>
class ConcurrentQueue {
public:
    bool TryPeek(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_.empty()) {
            return false;
        }
        value = std::move(buffer_.front());
        buffer_.pop();
        return true;
    }

    void Push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.emplace(std::move(value));
    }

private:
    std::mutex mutex_;
    std::queue<T> buffer_;
};

enum TaskState { Created, Completed, Failed, Canceled };

class Task : public std::enable_shared_from_this<Task> {
public:
    virtual ~Task() {
    }

    virtual void Run() = 0;

    void AddDependency(std::shared_ptr<Task> dep) {
        deps_.emplace_back(dep);
    }

    void AddTrigger(std::shared_ptr<Task> dep) {
        trigs_.emplace_back(dep);
    }
    void SetTimeTrigger(std::chrono::system_clock::time_point at) {
        deadline_ = at;
    }

    // Task::Run() completed without throwing exception
    bool IsCompleted() {
        return state_ == Completed;
    }

    // Task::Run() throwed exception
    bool IsFailed() {
        return state_ == Failed;
    }

    // Task was Canceled
    bool IsCanceled() {
        return state_ == Canceled;
    }

    // Task either completed, failed or was Canceled
    bool IsFinished() {
        return state_ != Created;
    }

    bool CanBeRun() {
        if (deps_.empty() && trigs_.empty() && !deadline_.has_value() && !IsFinished()) {
            return true;
        }

        bool deps_finished =
            std::all_of(deps_.begin(), deps_.end(),
                        [](std::shared_ptr<Task> task) { return task->IsFinished(); }) &&
            !deps_.empty();
        bool trig_finished =
            std::any_of(trigs_.begin(), trigs_.end(),
                        [](std::shared_ptr<Task> task) { return task->IsFinished(); });
        bool deadline_passed =
            deadline_.has_value() && std::chrono::system_clock::now() > deadline_;
        return (deps_finished || trig_finished || deadline_passed) && !IsFinished();
    }

    void SetError(std::exception_ptr exc) {
        task_exception_ = exc;
    }

    std::exception_ptr GetError() {
        return task_exception_;
    }

    void Cancel() {
        SetState(Canceled);
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        while (!IsFinished()) {
            done_.wait(lock);
        }
    }

    void SetState(TaskState new_state) {
        state_ = new_state;
        done_.notify_all();
    }

    std::condition_variable done_;
    std::mutex wait_mutex_;

private:
    std::atomic<TaskState> state_;
    std::vector<std::shared_ptr<Task>> deps_;
    std::vector<std::shared_ptr<Task>> trigs_;
    std::optional<std::chrono::system_clock::time_point> deadline_;
    std::exception_ptr task_exception_;
};

template <class T>
class Future : public Task {
public:
    Future(std::function<T()> fn) : fn_(fn) {
    }

    T Get() {
        Wait();
        if (IsFailed()) {
            std::rethrow_exception(GetError());
            // throw std::logic_error{"Test"};
        }
        return result_;
    }

    void Run() {
        result_ = fn_();
    }

private:
    T result_;
    std::function<T()> fn_;
};

template <class T>
using FuturePtr = std::shared_ptr<Future<T>>;

// Used instead of void in generic code
struct Unit {};

class Executor {
public:
    Executor(uint32_t num_threads) {
        for (uint32_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (!executor_finished_) {
                    std::shared_ptr<Task> task;
                    if (task_queue_.TryPeek(task)) {
                        auto guard = std::lock_guard(task->wait_mutex_);
                        if (task->CanBeRun()) {
                            try {
                                task->Run();
                                task->SetState(Completed);
                            } catch (...) {
                                task->SetError(std::current_exception());
                                task->SetState(Failed);
                            }
                        } else if (task->IsFinished()) {
                            continue;
                        } else {
                            task_queue_.Push(task);
                        }
                    }
                }
                std::shared_ptr<Task> task;
                while (task_queue_.TryPeek(task)) {
                    if (task->CanBeRun()) {
                        task->Cancel();
                    }
                }
            });
        }
    }

    ~Executor() {
        StartShutdown();
        WaitShutdown();
    }

    void Submit(std::shared_ptr<Task> task) {
        if (executor_finished_) {
            // TODO: remove all deps
            task->Cancel();
            return;
        }
        task_queue_.Push(std::move(task));
    }

    void StartShutdown() {
        executor_finished_ = true;
    }

    void WaitShutdown() {
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    template <class T>
    FuturePtr<T> Invoke(std::function<T()> fn) {
        FuturePtr<T> task = std::make_shared<Future<T>>(fn);
        Submit(task);
        return task;
    }

    template <class Y, class T>
    FuturePtr<Y> Then(FuturePtr<T> input, std::function<Y()> fn) {
        FuturePtr<Y> future_b = std::make_shared<Future<Y>>(fn);
        future_b->AddDependency(input);
        Submit(future_b);
        return future_b;
    }

    template <class T>
    FuturePtr<std::vector<T>> WhenAll(std::vector<FuturePtr<T>> all) {
        FuturePtr<std::vector<T>> task = std::make_shared<Future<std::vector<T>>>([all] {
            std::vector<T> results;
            results.reserve(all.size());
            for (auto& dep : all) {
                results.emplace_back(dep->Get());
            }
            return results;
        });
        for (auto& dep : all) {
            task->AddDependency(dep);
        }
        Submit(task);
        return task;
    }

    template <class T>
    FuturePtr<T> WhenFirst(std::vector<FuturePtr<T>> all) {
        FuturePtr<T> task = std::make_shared<Future<T>>([all] {
            for (auto& trig : all) {
                if (trig->IsFinished()) {
                    return trig->Get();
                }
            }
            return all.at(0)->Get();
        });
        for (auto& trig : all) {
            task->AddTrigger(trig);
        }
        Submit(task);
        return task;
    }

    template <class T>
    FuturePtr<std::vector<T>> WhenAllBeforeDeadline(
        std::vector<FuturePtr<T>> all, std::chrono::system_clock::time_point deadline) {
        FuturePtr<std::vector<T>> task = std::make_shared<Future<std::vector<T>>>([all] {
            std::vector<T> results;
            results.reserve(all.size());
            for (auto& t : all) {
                if (t->IsFinished()) {
                    results.emplace_back(t->Get());
                }
            }
            return results;
        });
        task->SetTimeTrigger(deadline);
        Submit(task);
        return task;
    }

private:
    std::vector<std::thread> workers_;
    std::atomic_bool executor_finished_ = false;
    ConcurrentQueue<std::shared_ptr<Task>> task_queue_;
};

inline std::shared_ptr<Executor> MakeThreadPoolExecutor(uint32_t num_threads) {
    return std::make_shared<Executor>(num_threads);
}
