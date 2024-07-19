#include "watermark.h"

auto Watermark::AddTxn(timestamp_t read_ts) -> void {
    if (read_ts < commit_ts_) {
        throw InternalError("read ts < commit ts");
    }

    // TODO(fall2023): implement me!
    ++current_reads_[read_ts];
    watermark_ = std::min(watermark_, read_ts);
}

auto Watermark::RemoveTxn(timestamp_t read_ts) -> void {
    // TODO(fall2023): implement me!
    if (--current_reads_[read_ts] == 0) {
        current_reads_.erase(read_ts);
        watermark_ = current_reads_.empty() ? commit_ts_ : current_reads_.begin()->first;
    }
}
