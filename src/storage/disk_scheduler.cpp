#include "disk_scheduler.h"
// #include "common/exception.h"
#include <cassert>

#include "disk_manager.h"

DiskScheduler::DiskScheduler(DiskManager *disk_manager) : disk_manager_(disk_manager) {
    // TODO(P1): remove this line after you have implemented the disk scheduler API
    // throw NotImplementedException(
    //      "DiskScheduler is not implemented yet. If you have finished implementing the disk scheduler, please remove the
    //      " "throw exception line in `disk_scheduler.cpp`.");

    // Spawn the background thread
    stop_thread_ = false;
    background_thread_.emplace([&] { StartWorkerThread(); });
    // background_thread_->detach();
}

DiskScheduler::~DiskScheduler() {
    // Put a `std::nullopt` in the queue to signal to exit the loop
    // for (auto &[_, req] : request_queue_) {
    //     std::ignore = _;
    //     req.Put(std::nullopt);
    // }
    stop_thread_ = true;
    if (background_thread_.has_value()) {
        if (background_thread_.value().joinable()) {
            background_thread_->join();
        }
    }
}

void DiskScheduler::Schedule(DiskRequest r) { request_queue_[r.page_id_.page_no].Put(std::make_optional(std::move(r))); }

void DiskScheduler::ScheduleRead(Page &page) {
    auto e = request_queue_[page.get_page_no()].TryReadFromQueue();
    if (e.has_value() && e.value().has_value()) {
        auto &last_req = e.value();
        memcpy(page.get_data(), last_req->data_, PAGE_SIZE);
    } else {
        disk_manager_->read_page(page.get_page_id().fd, page.get_page_id().page_no, page.get_data(), PAGE_SIZE);
        request_queue_[page.get_page_no()].LoadBuffer(DiskRequest(page.get_page_id(), page.get_data()));
    }
}

void DiskScheduler::StartWorkerThread() {
    while (!stop_thread_) {
        for (auto &[_, req] : request_queue_) {
            std::ignore = _;
            while (auto e = req.Get()) {
                if (!e.has_value()) {
                    break;
                }
                auto &r = e.value();
                disk_manager_->write_page(r.page_id_.fd, r.page_id_.page_no, r.data_, PAGE_SIZE);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}
