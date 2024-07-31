#pragma once

#include <condition_variable>  // NOLINT
#include <mutex>               // NOLINT
#include <queue>
#include <utility>

/**
 * Channels allow for safe sharing of data between threads. This is a multi-producer multi-consumer channel.
 */
template<class T>
class Channel {
public:
    Channel() = default;

    ~Channel() = default;

    /**
     * @brief Inserts an element into a shared queue.
     *
     * @param element The element to be inserted.
     */
    void Put(T element) {
        std::unique_lock lk(m_);
        // is_cache_ = false;
        q_.emplace(std::move(element));
        lk.unlock();
        // cv_.notify_all();
    }

    /**
     * @brief Gets an element from the shared queue. If the queue is empty, blocks until an element is available.
     */
    auto Get() -> T {
        std::unique_lock lk(m_);
        // cv_.wait(lk, [&]() { return !q_.empty(); });
        if (q_.empty()) {
            return std::nullopt;
        }
        T element = std::move(q_.front());
        q_.pop();
        // if (q_.empty()) {
        //     is_cache_ = true;
        //     buffer_ = element;
        // }
        return element;
    }

    // auto isEmptyOrReturn() -> std::optional<T> {
    //     std::unique_lock lk(m_);
    //     if (q_.empty()) {
    //         return std::nullopt;
    //     }
    //     T element = std::move(q_.front());
    //     q_.pop();
    //     return element;
    // }

    auto TryReadFromQueue() -> std::optional<T> {
        std::unique_lock lk(m_);
        if (q_.empty()) {
            // if (is_cache_) {
            //     return buffer_;
            // }
            return std::nullopt;
        }
        return q_.back();
    }

    // void LoadBuffer(T element) {
    //     std::unique_lock lk(m_);
    //     buffer_ = std::move(element);
    //     is_cache_ = true;
    // }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<T> q_;
    // T buffer_;
    // bool is_cache_{false};
};
