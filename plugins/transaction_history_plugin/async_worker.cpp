#include <eosio/transaction_history_plugin/async_worker.hpp>
#include <fc/log/logger.hpp>

namespace eosio {

async_worker::async_worker() : stopping_(false) {
}

async_worker::~async_worker() {
   stop();
}

void async_worker::start() {
   if (!workers_.empty()) {
      return; // Already started
   }

   stopping_ = false;

   // Create worker threads (typically 2-4 threads for I/O bound tasks)
   const size_t num_threads = std::min(4u, std::max(2u, std::thread::hardware_concurrency()));
   workers_.reserve(num_threads);

   for (size_t i = 0; i < num_threads; ++i) {
      workers_.emplace_back(&async_worker::worker_thread, this);
   }

   ilog("Started ${count} worker threads for transaction history processing",
        ("count", num_threads));
}

void async_worker::stop() {
   {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      stopping_ = true;
   }

   condition_.notify_all();

   for (auto& worker : workers_) {
      if (worker.joinable()) {
         worker.join();
      }
   }

   workers_.clear();

   // Clear remaining tasks
   std::queue<task_type> empty;
   std::unique_lock<std::mutex> lock(queue_mutex_);
   tasks_.swap(empty);
}

size_t async_worker::pending_tasks() const {
   std::unique_lock<std::mutex> lock(queue_mutex_);
   return tasks_.size();
}

void async_worker::worker_thread() {
   dlog("Transaction history worker thread started");

   while (true) {
      task_type task;

      {
         std::unique_lock<std::mutex> lock(queue_mutex_);
         condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

         if (stopping_ && tasks_.empty()) {
            break;
         }

         task = std::move(tasks_.front());
         tasks_.pop();
      }

      try {
         task();
      } catch (const std::exception& e) {
         elog("Exception in transaction history worker thread: ${what}", ("what", e.what()));
      } catch (...) {
         elog("Unknown exception in transaction history worker thread");
      }
   }

   dlog("Transaction history worker thread stopped");
}

} // namespace eosio
