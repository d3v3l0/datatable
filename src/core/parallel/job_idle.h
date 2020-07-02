//------------------------------------------------------------------------------
// Copyright 2019-2020 H2O.ai
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//------------------------------------------------------------------------------
#ifndef dt_PARALLEL_JOB_IDLE_h
#define dt_PARALLEL_JOB_IDLE_h
#include <atomic>               // std::atomic
#include <cstddef>              // std::size_t
#include <mutex>                // std::mutex
#include "parallel/monitor_thread.h"
#include "parallel/semaphore.h"
#include "parallel/thread_job.h"
namespace dt {

class SleepTask;


/**
  * This class handles putting to sleep/awaking of workers in a thread pool.
  * A single instance of this class exists in `thread_pool`.
  *
  * Initially all workers in a thread pool are in the "idle" state, running the
  * sleep task returned by this scheduler. This sleep task is `tsleep[0]`, and
  * it contains a mutex, and a condition variable. In this state the workers are
  * simply waiting, though they may occasionally be woken by the operating system
  * to check whether `tsleep[0].next_scheduler` became non-null.
  *
  * More precisely, a thread is considered to be asleep if its scheduler is this
  * class, and if the thread already requested a sleep task from this scheduler
  * and started executing that sleep task.
  *
  * When master thread calls `awaken` (and only the master thread is allowed to
  * do so), we do the following:
  *   - lock `tsleep[0].mutex` (at this point no thread can awaken, even
  *     spuriously, because they would need to acquire lock on the same mutex as
  *     they wake up);
  *   - set `tsleep[0].next_scheduler` to the job that needs to be executed;
  *   - set `tsleep[1].next_scheduler` to nullptr;
  *   - change `index` from 0 to 1;
  *   - unlock the mutex and notify all threads waiting on
  *     `tsleep[0].wakeup_all_threads_cv`.
  *
  * As the threads awaken, they check their task's `next_scheduler` property, see
  * that it is now not-null, they will switch to that scheduler and finish their
  * current sleep task. Note that it may take some time for OS to notify and
  * awaken all the threads; some threads may already finish their new task by the
  * time the last thread in the team gets up.
  *
  * When a thread's queue is exhausted and there are no more tasks to do, that
  * worker receives a `nullptr` from `get_next_task()`. At this moment the
  * worker switches back to `Job_Idle`, and requests a task. The
  * thread sleep scheduler will now return `tsleep[1]`, which has its own mutex
  * and a condition variable, and its `.next_scheduler` is null, indicating the
  * sleeping state. This will allow the thread to go safely to sleep, while other
  * threads might still be waking up from the initial sleep.
  *
  * The master thread that called `awaken(job)` will then call `job.join()`,
  * and it is the responsibility of ThreadJob `job` to wait until all
  * threads have finished execution and were put back to sleep. Thus, the master
  * thread ensures that all threads are sleeping again before the next call to
  * `awaken`.
  */
class Job_Idle : public ThreadJob {
  private:
    // "Current" sleep task, meaning that all sleeping threads are executing
    // `curr_sleep_task->execute()`.
    SleepTask* curr_sleep_task;

    // The "previous" sleep task. The pointers `prev_sleep_task` and
    // `curr_sleep_task` flip-flop.
    SleepTask* prev_sleep_task;

    // How many threads are currently active (i.e. not sleeping)
    std::atomic<int> n_threads_running;
    int : 32;

    // If an exception occurs during execution, it will be saved here
    std::exception_ptr saved_exception;

    // Thread-worker object corresponding to the master thread.
    ThreadWorker* master_worker;

  public:
    Job_Idle();

    ThreadTask* get_next_task(size_t thread_index) override;

    // Called from the master thread, this function will awaken all threads
    // in the thread pool, and give them `job` to execute.
    // Precondition: that all threads in the pool are currently sleeping.
    void awaken_and_run(ThreadJob* job, size_t nthreads);

    // Called from the master thread, this function will block until all the
    // work is finished and all worker threads have been put to sleep. If there
    // was an exception during execution of any of the tasks, this exception
    // will be rethrown here (but only after all workers were put to sleep).
    void join();

    // Called from worker threads, within the `catch(...){ }` block, this method
    // is used to signal that an exception have occurred. The method will save
    // this exception, so that it can be re-thrown after the parallel region
    // exits.
    void catch_exception() noexcept;

    // Return true if there is a task currently being run in parallel.
    bool is_running() const noexcept;

    void set_master_worker(ThreadWorker*) noexcept;

    // Register changes in the total number of currently active threads.
    void add_running_thread();
    void remove_running_thread();
};



class SleepTask : public ThreadTask {
  friend class Job_Idle;
  private:
    Job_Idle* const controller;
    ThreadJob* next_scheduler;
    LightweightSemaphore semaphore_;

  public:
    SleepTask(Job_Idle*);
    void execute() override;
};




}  // namespace dt
#endif