#pragma once
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>
#include <vector>
#include <stdexcept>
#include <type_traits>

namespace eosio {

/**
 * @brief Asynchronous Worker Thread Pool
 *
 * Provides a thread pool for executing database operations asynchronously
 * without blocking the main chain processing thread. Supports both
 * fire-and-forget tasks and tasks that return values.
 */
class async_worker {
public:
   using task_type = std::function<void()>;
   static constexpr size_t max_pending_tasks = 10000; // default bounded admission
   static constexpr size_t max_pending_bytes = 256 * 1024 * 1024;

   explicit async_worker(size_t pending_task_limit = max_pending_tasks,
                         size_t pending_byte_limit = max_pending_bytes);
   ~async_worker();

   /**
    * @brief Start the worker thread pool
    */
   void start();

   /**
    * @brief Stop the worker thread pool and wait for completion
    * @param drain If false, discard tasks which have not started yet. The
    *              currently running task is always allowed to finish.
    */
   void stop(bool drain = true);

   /**
    * @brief Enqueue a task for asynchronous execution
    * @tparam F Function type
    * @tparam Args Argument types
    * @param f Function to execute
    * @param args Arguments to pass to function
    * @return Future for the result
    */
   template<typename F, typename... Args>
   auto enqueue_task(F&& f, Args&&... args)
       -> std::future<std::invoke_result_t<F, Args...>> {

      return enqueue_task_with_size(0, std::forward<F>(f), std::forward<Args>(args)...);
   }

   /**
    * @brief Enqueue a task while accounting for memory retained by the task
    * @param task_size Estimated retained bytes until the task starts executing
    */
   template<typename F, typename... Args>
   auto enqueue_task_with_size(size_t task_size, F&& f, Args&&... args)
       -> std::future<std::invoke_result_t<F, Args...>> {

      using return_type = std::invoke_result_t<F, Args...>;

      auto task = std::make_shared<std::packaged_task<return_type()>>(
         std::bind(std::forward<F>(f), std::forward<Args>(args)...)
      );

      std::future<return_type> result = task->get_future();
      enqueue([task](){ (*task)(); }, task_size);
      return result;
   }

   /**
    * @brief Try to enqueue without waiting for queue capacity
    *
    * This is safe to call from the worker itself. It prevents a single worker
    * from deadlocking while attempting to enqueue behind a full queue that only
    * it can drain.
    */
   template<typename F, typename... Args>
   bool try_enqueue_task(F&& f, Args&&... args) {
      return try_enqueue_task_with_size(0, std::forward<F>(f), std::forward<Args>(args)...);
   }

   template<typename F, typename... Args>
   bool try_enqueue_task_with_size(size_t task_size, F&& f, Args&&... args) {
      return try_enqueue(task_type(std::bind(std::forward<F>(f), std::forward<Args>(args)...)),
                         task_size);
   }

   /**
    * @brief Enqueue a required task, waiting until bounded queue capacity is available
    *
    * Chain event producers use this method when dropping the task would make
    * transaction history incomplete. Do not call it from the worker thread;
    * optional tasks scheduled by a worker must continue to use try_enqueue_task.
    *
    * @return false only when the task exceeds the byte limit or the worker is stopping
    */
   template<typename F, typename... Args>
   bool enqueue_task_with_backpressure(F&& f, Args&&... args) {
      return enqueue_task_with_backpressure_and_size(
         0, std::forward<F>(f), std::forward<Args>(args)...);
   }

   template<typename F, typename... Args>
   bool enqueue_task_with_backpressure_and_size(size_t task_size, F&& f, Args&&... args) {
      return enqueue_with_backpressure(
         task_type(std::bind(std::forward<F>(f), std::forward<Args>(args)...)), task_size);
   }

   /**
    * @brief Get number of pending tasks
    * @return Number of tasks in queue
    */
   size_t pending_tasks() const;
   size_t pending_bytes() const;
   size_t pending_task_limit() const;
   size_t pending_byte_limit() const;

private:
   struct queued_task {
      task_type task;
      size_t retained_bytes = 0;
   };

   void enqueue(task_type task, size_t retained_bytes);
   bool enqueue_with_backpressure(task_type task, size_t retained_bytes);
   bool try_enqueue(task_type task, size_t retained_bytes);
   void worker_thread();

   std::vector<std::thread> workers_;
   std::queue<queued_task> tasks_;
   const size_t pending_task_limit_;
   const size_t pending_byte_limit_;
   size_t pending_bytes_ = 0;
   mutable std::mutex queue_mutex_;
   std::condition_variable condition_;
   std::condition_variable queue_not_full_;
   std::atomic<bool> stopping_;
};

} // namespace eosio
