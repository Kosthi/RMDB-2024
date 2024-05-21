//
// Created by Koschei on 2024/5/21.
//

#ifndef RWLATCH_H
#define RWLATCH_H

#include <shared_mutex>

class RWLatch {
public:
    void WLock() {
        mutex_.lock();
    }

    void WUnlock() {
        mutex_.unlock();
    }


    void RLock() {
        mutex_.lock_shared();
    }

    void RUnlock() {
        mutex_.unlock_shared();
    }

private:
    std::shared_mutex mutex_;
};

#endif //RWLATCH_H
