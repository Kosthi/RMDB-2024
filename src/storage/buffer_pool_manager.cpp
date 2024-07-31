#include "buffer_pool_manager.h"

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page *BufferPoolManager::fetch_page(PageId page_id) {
    return instances_[get_instance_no(page_id)]->fetch_page(page_id);
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    return instances_[get_instance_no(page_id)]->unpin_page(page_id, is_dirty);
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    return instances_[get_instance_no(page_id)]->flush_page(page_id);
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page *BufferPoolManager::new_page(PageId *page_id) {
    *page_id = {page_id->fd, disk_manager_->allocate_page(page_id->fd)};
    return instances_[get_instance_no(*page_id)]->new_page(page_id);
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    return instances_[get_instance_no(page_id)]->delete_page(page_id);
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages(int fd) {
    for (auto &instance: instances_) {
        instance->flush_all_pages(fd);
    }
}

/** 为创建检查点调用
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages_for_checkpoint(int fd) {
    for (auto &instance: instances_) {
        instance->flush_all_pages_for_checkpoint(fd);
    }
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::delete_all_pages(int fd) {
    for (auto &instance: instances_) {
        instance->delete_all_pages(fd);
    }
}

// auto BufferPoolManager::FetchPageBasic(PageId page_id) -> BasicPageGuard {
//     auto *page = fetch_page(page_id);
//     return {this, page};
// }
//
// auto BufferPoolManager::FetchPageRead(PageId page_id) -> ReadPageGuard {
//     auto *page = fetch_page(page_id);
//     page->RLatch();
//     return {this, page};
// }
//
// auto BufferPoolManager::FetchPageWrite(PageId page_id) -> WritePageGuard {
//     auto *page = fetch_page(page_id);
//     page->WLatch();
//     return {this, page};
// }
//
// auto BufferPoolManager::NewPageGuarded(PageId *page_id) -> BasicPageGuard {
//     auto *page = new_page(page_id);
//     return {this, page};
// }
