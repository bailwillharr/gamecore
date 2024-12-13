#include "gamecore/gc_jobs.h"

#include <cmath>

#include <functional>
#include <thread>

#include "gamecore/gc_assert.h"

#include "gamecore/gc_logger.h"

namespace gc {

Jobs::Jobs(unsigned int num_threads)
    : m_num_threads(std::max(1u, num_threads)),
      m_ring_buffer(),
      m_ring_buffer_mutex(),
      m_wake_condition(),
      m_wake_condition_mutex(),
      m_current_label(0),
      m_finished_label(0),
      m_shutdown_threads(false),
      m_num_threads_running(0),
      m_workers()
{
    for (unsigned int thread_id = 0; thread_id < m_num_threads; ++thread_id) {
        m_workers.emplace_back([&ring_buffer = m_ring_buffer, &ring_buffer_mutex = m_ring_buffer_mutex, &finished_label = m_finished_label,
                                &wake_condition = m_wake_condition, &wake_condition_mutex = m_wake_condition_mutex, &shutdown_threads = m_shutdown_threads,
                                &num_threads_running = m_num_threads_running] {
            num_threads_running.fetch_add(1);
            for (;;) {
                ring_buffer_mutex.lock();
                auto job = ring_buffer.popFront();
                ring_buffer_mutex.unlock();
                if (job.has_value()) {
                    // execute new job
                    Logger::instance().trace("Running job from ring buffer...");
                    job.value().operator()();
                    finished_label.fetch_add(1);
                }
                else {
                    // no job right now. make thread sleep
                    {
                        Logger::instance().trace("Thread going to sleep...");
                        std::unique_lock<std::mutex> lock(wake_condition_mutex);
                        wake_condition.wait(lock);
                        Logger::instance().trace("Thread woke up");
                    }
                    if (shutdown_threads.load()) {
                        Logger::instance().trace("Shutting down thread...");
                        num_threads_running.fetch_sub(1);
                        return; // end thread
                    }
                }
            }
        });
    }
    // now ensure threads have actually started
    while (m_num_threads_running < m_num_threads) {
        std::this_thread::yield();
    }
}

Jobs::~Jobs()
{
    wait();
    m_shutdown_threads.store(true);
    while (m_num_threads_running.load() > 0) { // wait until all threads have stopped running
        m_wake_condition.notify_one();
        std::this_thread::yield();
    }
    for (std::thread& worker : m_workers) {
        worker.join();
    }
}

void Jobs::execute(const std::function<void()>& func)
{
    m_current_label += 1;

    // infinite loop until func is pushed
    for (;;) {
        m_ring_buffer_mutex.lock();
        bool was_pushed = m_ring_buffer.pushBack(func);
        m_ring_buffer_mutex.unlock();
        if (was_pushed) {
            m_wake_condition.notify_one(); // now job has been pushed, wake a thread to execute it
            return;
        }
        else {
            m_wake_condition.notify_one(); // wake up a thread to hopefully free up the job ring buffer
            std::this_thread::yield();     // allow rescheduling
        }
    }
}

void Jobs::dispatch(unsigned int job_count, unsigned int group_size, const std::function<void(JobDispatchArgs)>& func)
{
    if (job_count == 0 || group_size == 0) {
        return;
    }

    const unsigned int group_count = static_cast<unsigned int>(std::ceil(static_cast<double>(job_count) / static_cast<double>(group_size)));
    GC_ASSERT(group_count * group_size >= job_count);

    m_current_label += group_count;

    for (unsigned int group_index = 0; group_index < group_count; ++group_index) {
        auto job_group = [job_count, group_size, func, group_index]() {
            const unsigned int group_job_offset = group_index * group_size;
            const unsigned int group_job_end = std::min(group_job_offset + group_size, job_count);

            JobDispatchArgs args;
            args.group_index = group_index;

            for (unsigned int i = group_job_offset; i < group_job_end; ++i) {
                args.job_index = i;
                func(args);
            }
        };

        // infinite loop until job_group is pushed
        for (;;) {
            m_ring_buffer_mutex.lock();
            bool was_pushed = m_ring_buffer.pushBack(job_group);
            m_ring_buffer_mutex.unlock();
            if (was_pushed) {
                break;
            }
            else {
                m_wake_condition.notify_one(); // wake up a thread to hopefully free up the job ring buffer
                std::this_thread::yield();     // allow rescheduling
            }
        }

        m_wake_condition.notify_one();
    }
}

bool Jobs::isBusy()
{
    // if finished label hasn't reached current label, jobs are still executing
    uint64_t finished_label = m_finished_label.load();
    return finished_label < m_current_label;
}

void Jobs::wait()
{
    while (isBusy()) {
        m_wake_condition.notify_one();
        std::this_thread::yield();
    }
}

} // namespace gc
