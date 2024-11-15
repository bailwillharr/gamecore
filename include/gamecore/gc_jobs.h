#pragma once

#include <cstdint>

#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>

#include "gc_ring_buffer.h"

/* multithreaded job system */

namespace gc {

struct JobDispatchArgs {
    unsigned int job_index;
    unsigned int group_index;
};

class Jobs {
    const unsigned int m_num_threads;
    RingBuffer<std::function<void()>, 256> m_ring_buffer;
    std::mutex m_ring_buffer_mutex;
    std::condition_variable m_wake_condition;
    std::mutex m_wake_condition_mutex;
    uint64_t m_current_label;
    std::atomic<uint64_t> m_finished_label;
    std::atomic<bool> m_shutdown_threads;
    std::atomic<unsigned int> m_num_threads_running;
    std::vector<std::thread> m_workers;

public:
    Jobs(unsigned int num_threads);

    ~Jobs();

    Jobs(const Jobs&) = delete;
    Jobs(Jobs&&) = delete;

    Jobs& operator=(const Jobs&) = delete;
    Jobs& operator=(Jobs&&) = delete;

    /* add a job to execute asynchronously, any idle thread will execute this job. */
    void execute(const std::function<void()>& func);

    /* divide a job onto multiple jobs and execute in parallel. */
    /*    job_count       : how many jobs to generate for this task */
    /*    group_size      : how many jobs to execute per thread */
    /*                      less threads may be used depending on how fast jobs take */
    void dispatch(unsigned int job_count, unsigned int group_size, const std::function<void(JobDispatchArgs)>& func);

    bool isBusy();

    /* wait until all threads are idle */
    void wait();
};

} // namespace gc
