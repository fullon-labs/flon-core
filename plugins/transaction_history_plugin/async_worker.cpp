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

   // History writes and block checkpoints must retain chain event order. A
   // single writer also avoids races between per-transaction index updates.
   const size_t num_threads = 1;
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
   queue_not_full_.notify_all();

   for (auto& worker : workers_) {
      if (worker.joinable()) {
         worker.join();
      }
   }

   workers_.clear();

   // Clear remaining tasks
   std::queue<queued_task> empty;
   std::unique_lock<std::mutex> lock(queue_mutex_);
   tasks_.swap(empty);
   pending_bytes_ = 0;
}

size_t async_worker::pending_tasks() const {
   std::unique_lock<std::mutex> lock(queue_mutex_);
   return tasks_.size();
}

size_t async_worker::pending_bytes() const {
   std::unique_lock<std::mutex> lock(queue_mutex_);
   return pending_bytes_;
}

void async_worker::enqueue(task_type task, size_t retained_bytes) {
   if (retained_bytes > max_pending_bytes) {
      throw std::runtime_error("task exceeds async_worker byte budget");
   }

   {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_not_full_.wait(lock, [this, retained_bytes] {
         return stopping_ ||
                (tasks_.size() < max_pending_tasks &&
                 pending_bytes_ <= max_pending_bytes - retained_bytes);
      });
      if (stopping_) {
         throw std::runtime_error("enqueue on stopped async_worker");
      }
      tasks_.push(queued_task{std::move(task), retained_bytes});
      pending_bytes_ += retained_bytes;
   }

   condition_.notify_one();
}

bool async_worker::try_enqueue(task_type task, size_t retained_bytes) {
   if (retained_bytes > max_pending_bytes) {
      return false;
   }

   {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (stopping_ || tasks_.size() >= max_pending_tasks ||
          pending_bytes_ > max_pending_bytes - retained_bytes) {
         return false;
      }
      tasks_.push(queued_task{std::move(task), retained_bytes});
      pending_bytes_ += retained_bytes;
   }

   condition_.notify_one();
   return true;
}

void async_worker::worker_thread() {
   dlog("Transaction history worker thread started");

   while (true) {
      queued_task queued;

      {
         std::unique_lock<std::mutex> lock(queue_mutex_);
         condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

         if (stopping_ && tasks_.empty()) {
            break;
         }

         queued = std::move(tasks_.front());
         tasks_.pop();
         pending_bytes_ -= queued.retained_bytes;
         queue_not_full_.notify_one();
      }

      try {
         queued.task();
      } catch (const std::exception& e) {
         elog("Exception in transaction history worker thread: ${what}", ("what", e.what()));
      } catch (...) {
         elog("Unknown exception in transaction history worker thread");
      }
   }

   dlog("Transaction history worker thread stopped");
}

} // namespace eosio
