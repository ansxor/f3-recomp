#pragma once
#include "audio/hle/core.hpp"
#include "platform/thread.hpp"
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>

namespace f3rt {

template <class W, class Message>
concept Applies = requires(W &worker, const Message &message) { worker.apply(message); };

template <class Message, class Worker, size_t Capacity>
    requires Applies<Worker, Message>
class WorkerQueue {
public:
    explicit WorkerQueue(Worker &worker)
        : m_worker(worker), m_thread(ThreadRole::AudioSynth, 0, [this] { run(); }) {}
    ~WorkerQueue() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_all();
        m_completed.notify_all();
        m_thread.join();
    }
    WorkerQueue(const WorkerQueue &) = delete;
    WorkerQueue &operator=(const WorkerQueue &) = delete;

    void post(const Message &message) {
        std::unique_lock lock(m_mutex);
        m_completed.wait(lock, [&] { return m_pushed - m_popped < Capacity || m_failure; });
        if (m_failure) std::rethrow_exception(m_failure);
        m_slots[m_pushed++ % Capacity] = message;
        m_wake.notify_one();
    }
    void flush() {
        std::unique_lock lock(m_mutex);
        const uint64_t target = m_pushed;
        m_completed.wait(lock, [&] { return m_done >= target || m_failure; });
        if (m_failure) std::rethrow_exception(m_failure);
    }

private:
    void run() noexcept {
        hle::Core::enable_flush_denormals();
        try {
            for (;;) {
                Message message{};
                uint64_t ticket = 0;
                {
                    std::unique_lock lock(m_mutex);
                    m_wake.wait(lock, [&] { return m_stopping || m_popped != m_pushed; });
                    if (m_stopping && m_popped == m_pushed) return;
                    message = std::move(m_slots[m_popped++ % Capacity]);
                    ticket = m_popped;
                    m_completed.notify_all();
                }
                m_worker.apply(message);
                {
                    std::lock_guard lock(m_mutex);
                    m_done = ticket;
                }
                m_completed.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard lock(m_mutex);
                m_failure = std::current_exception();
            }
            m_completed.notify_all();
            m_wake.notify_all();
        }
    }

    Worker &m_worker;
    std::mutex m_mutex;
    std::condition_variable m_wake, m_completed;
    std::array<Message, Capacity> m_slots{};
    uint64_t m_pushed = 0, m_popped = 0, m_done = 0;
    bool m_stopping = false;
    std::exception_ptr m_failure;
    Thread m_thread;
};

}
