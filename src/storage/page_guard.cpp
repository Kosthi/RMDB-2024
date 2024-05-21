//
// Created by Koschei on 2024/5/21.
//

#include "buffer_pool_manager.h"
#include "page_guard.h"


BasicPageGuard::BasicPageGuard(BasicPageGuard &&that) noexcept {
    bpm_ = that.bpm_;
    page_ = that.page_;
    is_dirty_ = that.is_dirty_;

    that.bpm_ = nullptr;
    that.page_ = nullptr;
    that.is_dirty_ = false;
}

auto BasicPageGuard::operator=(BasicPageGuard &&that) noexcept -> BasicPageGuard & {
    Drop();

    bpm_ = that.bpm_;
    page_ = that.page_;
    is_dirty_ = that.is_dirty_;

    that.bpm_ = nullptr;
    that.page_ = nullptr;
    that.is_dirty_ = false;

    return *this;
}

void BasicPageGuard::Drop() {
    if (bpm_ && page_) {
        bpm_->unpin_page(page_->get_page_id(), is_dirty_);
    }

    bpm_ = nullptr;
    page_ = nullptr;
    is_dirty_ = false;
}

BasicPageGuard::~BasicPageGuard() {
    Drop();
}

auto BasicPageGuard::UpgradeRead() -> ReadPageGuard {
    page_->RLatch();
    ReadPageGuard read_page_guard(bpm_, page_);
    bpm_ = nullptr;
    page_ = nullptr;
    is_dirty_ = false;
    return read_page_guard;
}

auto BasicPageGuard::UpgradeWrite() -> WritePageGuard {
    page_->WLatch();
    WritePageGuard write_page_guard(bpm_, page_);
    bpm_ = nullptr;
    page_ = nullptr;
    is_dirty_ = false;
    return write_page_guard;
}

ReadPageGuard::ReadPageGuard(ReadPageGuard &&that) noexcept = default;

auto ReadPageGuard::operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard & {
    if (guard_.page_) {
        guard_.page_->RUnlatch();
    }
    guard_ = std::move(that.guard_);
    return *this;
}

void ReadPageGuard::Drop() {
    if (guard_.page_) {
        guard_.page_->RUnlatch();
    }
    guard_.Drop();
}

ReadPageGuard::~ReadPageGuard() {
    Drop();
}

WritePageGuard::WritePageGuard(WritePageGuard &&that) noexcept = default;

auto WritePageGuard::operator=(WritePageGuard &&that) noexcept -> WritePageGuard & {
    if (guard_.page_) {
        guard_.page_->WUnlatch();
    }
    guard_ = std::move(that.guard_);
    return *this;
}

void WritePageGuard::Drop() {
    if (guard_.page_) {
        guard_.page_->WUnlatch();
    }
    guard_.Drop();
}

WritePageGuard::~WritePageGuard() {
    Drop();
}
