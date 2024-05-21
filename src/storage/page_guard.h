//
// Created by Koschei on 2024/5/21.
//

#ifndef PAGE_GUARD_H
#define PAGE_GUARD_H

#include "page.h"

class BufferPoolManager;
class ReadPageGuard;
class WritePageGuard;

class BasicPageGuard {
public:
    BasicPageGuard() = default;

    BasicPageGuard(BufferPoolManager *bpm, Page *page) : bpm_(bpm), page_(page) {
    }

    BasicPageGuard(const BasicPageGuard &) = delete;

    auto operator=(const BasicPageGuard &) -> BasicPageGuard & = delete;

    // noexcept 优化性能
    BasicPageGuard(BasicPageGuard &&that) noexcept;

    void Drop();

    auto operator=(BasicPageGuard &&that) noexcept -> BasicPageGuard &;

    ~BasicPageGuard();

    auto UpgradeRead() -> ReadPageGuard;

    auto UpgradeWrite() -> WritePageGuard;

    auto GetPageId() const -> PageId { return page_->get_page_id(); }
    auto GetData() const -> const char * { return page_->get_data(); }

    auto GetDataMut() -> char * {
        is_dirty_ = true;
        return page_->get_data();
    }

    template<class T>
    auto As() -> const T * {
        return reinterpret_cast<const T *>(GetData());
    }

    template<class T>
    auto AsMut() -> T * {
        return reinterpret_cast<T *>(GetDataMut());
    }

private:
    friend ReadPageGuard;
    friend WritePageGuard;
    BufferPoolManager *bpm_{nullptr};
    Page *page_{nullptr};
    bool is_dirty_{false};
};

class ReadPageGuard {
public:
    ReadPageGuard() = default;

    ReadPageGuard(BufferPoolManager *bpm, Page *page) : guard_(bpm, page) {
    }

    ReadPageGuard(const ReadPageGuard &) = delete;

    auto operator=(const ReadPageGuard &) -> ReadPageGuard & = delete;

    // noexcept 优化性能
    ReadPageGuard(ReadPageGuard &&that) noexcept;

    void Drop();

    auto operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard &;

    ~ReadPageGuard();

    auto GetPageId() const -> PageId { return guard_.GetPageId(); }
    auto GetData() const -> const char * { return guard_.GetData(); }

    template<class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

private:
    BasicPageGuard guard_;
};

class WritePageGuard {
public:
    WritePageGuard() = default;

    WritePageGuard(BufferPoolManager *bpm, Page *page) : guard_(bpm, page) {
    }

    WritePageGuard(const WritePageGuard &) = delete;

    auto operator=(const WritePageGuard &) -> WritePageGuard & = delete;

    // noexcept 优化性能
    WritePageGuard(WritePageGuard &&that) noexcept;

    void Drop();

    auto operator=(WritePageGuard &&that) noexcept -> WritePageGuard &;

    ~WritePageGuard();

    auto GetPageId() const -> PageId { return guard_.GetPageId(); }
    auto GetData() const -> const char * { return guard_.GetData(); }

    template<class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

    auto GetDataMut() -> char * {
        return guard_.GetDataMut();
    }

    template<class T>
    auto AsMut() -> const T * {
        return guard_.AsMut<T>();
    }

private:
    BasicPageGuard guard_;
};
#endif //PAGE_GUARD_H
