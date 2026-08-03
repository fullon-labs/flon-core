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
   static constexpr size_t max_pending_tasks = 10000;

   async_worker();
   ~async_worker();

   /**
    * @brief Start the worker thread pool
    */
   void start();

   /**
    * @brief Stop the worker thread pool and wait for completion
    */
   void stop();

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
       -> std::future<typename std::result_of<F(Args...)>::type> {

      using return_type = typename std::result_of<F(Args...)>::type;

      auto task = std::make_shared<std::packaged_task<return_type()>>(
         std::bind(std::forward<F>(f), std::forward<Args>(args)...)
      );

      std::future<return_type> result = task->get_future();

      {
         std::unique_lock<std::mutex> lock(queue_mutex_);
         queue_not_full_.wait(lock, [this] {
            return stopping_ || tasks_.size() < max_pending_tasks;
         });
         if (stopping_) {
            throw std::runtime_error("enqueue on stopped async_worker");
         }
         tasks_.emplace([task](){ (*task)(); });
      }

      condition_.notify_one();
      return result;
   }

   /**
    * @brief Get number of pending tasks
    * @return Number of tasks in queue
    */
   size_t pending_tasks() const;

private:
   void worker_thread();

   std::vector<std::thread> workers_;
   std::queue<task_type> tasks_;
   mutable std::mutex queue_mutex_;
   std::condition_variable condition_;
   std::condition_variable queue_not_full_;
   std::atomic<bool> stopping_;
};

} // namespace eosio
