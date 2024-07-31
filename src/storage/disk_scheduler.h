#pragma once

#include <future>  // NOLINT
#include <optional>
#include <thread>  // NOLINT

#include "common/channel.h"
#include "disk_manager.h"
#include "page.h"

#include <cstring> // for memcpy

struct DiskRequest {
 /** Flag indicating whether the request is a write or a read. */
 // bool is_write_;

 DiskRequest(PageId page_id, char *data) : page_id_(page_id) {
  memcpy(data_, data, PAGE_SIZE);
 }

 ~DiskRequest() {
  delete []data_;
 }

 // Copy constructor
 DiskRequest(const DiskRequest& other): page_id_(other.page_id_) {
  data_ = new char[PAGE_SIZE];
  memcpy(data_, other.data_, PAGE_SIZE);
 }

 // Move constructor
 DiskRequest(DiskRequest&& other) noexcept : page_id_(other.page_id_), data_(other.data_) {
  other.data_ = nullptr;
 }

 // Copy assignment operator
 DiskRequest& operator=(const DiskRequest& other) {
  if (this != &other) {
   delete []data_; // clean up existing resources
   page_id_ = other.page_id_;
   data_ = new char[PAGE_SIZE];
   memcpy(data_, other.data_, PAGE_SIZE);
  }
  return *this;
 }

 // Move assignment operator
 DiskRequest& operator=(DiskRequest&& other) noexcept {
  if (this != &other) {
   delete []data_;
   page_id_ = other.page_id_;
   data_ = other.data_;
   other.data_ = nullptr;
  }
  return *this;
 }

 /**
  *  Pointer to the start of the memory location where a page is either:
  *   1. being read into from disk (on a read).
  *   2. being written out to disk (on a write).
  */
 char *data_ = new char[PAGE_SIZE];

 /** ID of the page being read from / written to disk. */
 PageId page_id_;

 /** Callback used to signal to the request issuer when the request has been completed. */
 // std::promise<bool> callback_;
};

/**
 * @brief The DiskScheduler schedules disk read and write operations.
 *
 * A request is scheduled by calling DiskScheduler::Schedule() with an appropriate DiskRequest object. The scheduler
 * maintains a background worker thread that processes the scheduled requests using the disk manager. The background
 * thread is created in the DiskScheduler constructor and joined in its destructor.
 */
class DiskScheduler {
public:
 explicit DiskScheduler(DiskManager *disk_manager);

 ~DiskScheduler();

 /**
  * TODO(P1): Add implementation
  *
  * @brief Schedules a request for the DiskManager to execute.
  *
  * @param r The request to be scheduled.
  */
 void Schedule(DiskRequest r);

 void ScheduleRead(Page &page);

 /**
  * TODO(P1): Add implementation
  *
  * @brief Background worker thread function that processes scheduled requests.
  *
  * The background thread needs to process requests while the DiskScheduler exists, i.e., this function should not
  * return until ~DiskScheduler() is called. At that point you need to make sure that the function does return.
  */
 void StartWorkerThread();

 using DiskSchedulerPromise = std::promise<bool>;

 /**
  * @brief Create a Promise object. If you want to implement your own version of promise, you can change this function
  * so that our test cases can use your promise implementation.
  *
  * @return std::promise<bool>
  */
 auto CreatePromise() -> DiskSchedulerPromise { return {}; };

private:
 /** Pointer to the disk manager. */
 DiskManager *disk_manager_ __attribute__((__unused__));
 /** A shared queue to concurrently schedule and process requests. When the DiskScheduler's destructor is called,
  * `std::nullopt` is put into the queue to signal to the background thread to stop execution. */
 std::unordered_map<page_id_t, Channel<std::optional<DiskRequest> >> request_queue_;
 /** The background thread responsible for issuing scheduled requests to the disk manager. */
 std::optional<std::thread> background_thread_;
 std::atomic_bool stop_thread_{};
};
