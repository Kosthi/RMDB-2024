#pragma once

#include <queue>

#define MAX_CHUNK_SIZE 1000

class SortExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta cols_; // 框架中只支持一个键排序，需要自行修改数据结构支持多个键排序
    size_t tuple_num;
    bool is_desc_;
    std::vector<size_t> used_tuple;
    std::unique_ptr<RmRecord> current_tuple_;
    std::deque<std::unique_ptr<RmRecord> > records_;
    size_t len_; // 字段总长度
    // 题目要求排序结果写入 sorted_results.txt，字符串
    std::fstream out_expected_file_;
    // 排序结果实际读取文件，二进制
    std::fstream outfile_;
    // 实际二进制文件名
    std::string filename_;
    std::vector<std::string> temp_files_;
    // 为每个 sort 算子实例分配一个 ID
    size_t id_;
    bool is_end_;

    static std::size_t generateID() {
        static size_t current_id = 0;
        return ++current_id;
    }

    // 排序一部分元组并放入块（临时文件）中
    void sortAndSaveChunk() {
        std::sort(records_.begin(), records_.end(),
                  [&](const std::unique_ptr<RmRecord> &l_rec, const std::unique_ptr<RmRecord> &r_rec) {
                      auto &&lhs = l_rec->data + cols_.offset;
                      auto &&rhs = r_rec->data + cols_.offset;
                      return is_desc_
                                 ? compare(lhs, rhs, cols_.len, cols_.type) > 0
                                 : compare(lhs, rhs, cols_.len, cols_.type) < 0;
                  });

        std::string temp_filename = "sorted_chunk_" + std::to_string(temp_files_.size()) + ".tmp";
        std::ofstream temp_file(temp_filename, std::ios::binary | std::ios::trunc);
        for (const auto &record: records_) {
            temp_file.write(record->data, record->size);
        }
        temp_file.close();
        temp_files_.emplace_back(temp_filename);
        records_.clear();
    }

    void mergeSortedChunks() {
        auto compareRecords = [&](std::pair<std::unique_ptr<RmRecord>, std::ifstream *> &a,
                                  std::pair<std::unique_ptr<RmRecord>, std::ifstream *> &b) {
            auto &&lhs = a.first->data + cols_.offset;
            auto &&rhs = b.first->data + cols_.offset;
            return is_desc_
                       ? compare(lhs, rhs, cols_.len, cols_.type) < 0
                       : compare(lhs, rhs, cols_.len, cols_.type) > 0;
        };

        std::priority_queue<
            std::pair<std::unique_ptr<RmRecord>, std::ifstream *>,
            std::vector<std::pair<std::unique_ptr<RmRecord>, std::ifstream *> >,
            decltype(compareRecords)> min_heap(compareRecords);

        std::vector<std::ifstream> temp_file_streams;
        // 读取每个块的第一条记录
        temp_file_streams.reserve(temp_files_.size());
        for (const auto &filename: temp_files_) {
            temp_file_streams.emplace_back(filename, std::ios::binary);
        }

        for (auto &file_stream: temp_file_streams) {
            auto record = std::make_unique<RmRecord>(len_);
            file_stream.read(record->data, record->size);
            // 有记录则放入小根堆中
            if (file_stream.gcount() > 0) {
                min_heap.emplace(std::move(record), &file_stream);
            }
        }

        // 以期望格式写入 sorted_results.txt
        out_expected_file_.open("sorted_results.txt", std::ios::out | std::ios::app);

        // 打印表头
        out_expected_file_ << "|";
        for (auto &col_meta: prev_->cols()) {
            out_expected_file_ << " " << col_meta.name << " |";
        }
        out_expected_file_ << "\n";

        // 每次从小根堆中取出最小的记录，写入结果文件
        while (!min_heap.empty()) {
            auto &[record, file_stream] = min_heap.top();

            // 打印记录
            std::vector<std::string> columns;
            columns.reserve(prev_->cols().size());

            for (auto &col: prev_->cols()) {
                std::string col_str;
                char *rec_buf = record->data + col.offset;
                if (col.type == TYPE_INT) {
                    col_str = std::to_string(*(int *) rec_buf);
                } else if (col.type == TYPE_FLOAT) {
                    col_str = std::to_string(*(float *) rec_buf);
                } else if (col.type == TYPE_STRING) {
                    col_str = std::string((char *) rec_buf, col.len);
                    col_str.resize(strlen(col_str.c_str()));
                }
                columns.emplace_back(col_str);
            }

            // 打印记录
            out_expected_file_ << "|";
            for (auto &col: columns) {
                out_expected_file_ << " " << col << " |";
            }
            out_expected_file_ << "\n";

            outfile_.write(record->data, record->size);
            auto new_record = std::make_unique<RmRecord>(len_);
            file_stream->read(new_record->data, new_record->size);
            if (file_stream->gcount() > 0) {
                min_heap.emplace(std::move(new_record), file_stream);
            } else {
                file_stream->close();
            }

            min_heap.pop();
        }

        out_expected_file_.close();

        // 清除临时文件
        for (auto &temp_file: temp_files_) {
            unlink(temp_file.c_str());
        }
    }

public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const TabCol &sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        // cols_ = prev_->get_col_offset(sel_cols);
        cols_ = *get_col(prev_->cols(), sel_cols);
        is_desc_ = is_desc;
        tuple_num = 0;
        used_tuple.clear();
        len_ = prev_->tupleLen();
        id_ = generateID();
        filename_ = "sorted_results" + std::to_string(id_) + ".txt";
    }

    void beginTuple() override {
        records_.clear();
        temp_files_.clear();
        current_tuple_ = nullptr;
        is_end_ = false;

        struct stat st;
        // 当前算子排序过了，已经存在排序结果文件
        if (stat(filename_.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            outfile_.open(filename_, std::ios::in);

            // 缓存记录
            do {
                current_tuple_ = std::make_unique<RmRecord>(len_);
                outfile_.read(current_tuple_->data, current_tuple_->size);
                if (outfile_.gcount() == 0) {
                    break;
                }
                records_.emplace_back(std::move(current_tuple_));
            } while (records_.size() < MAX_CHUNK_SIZE);

            if (!records_.empty()) {
                current_tuple_ = std::move(records_.front());
                records_.pop_front();
            } else {
                is_end_ = true;
                current_tuple_ = nullptr;
                outfile_.close();
            }
            return;
        }

        // ios::in 需要文件存在，ios::out文件不存在则创建
        // 两者同时存在且文件不存在时，会打开失败（文件不存在）
        outfile_.open(filename_, std::ios::out | std::ios::app);
        if (!outfile_.is_open()) {
            std::stringstream s;
            s << "Failed to open file: " << std::strerror(errno);
            throw InternalError(s.str());
        }

        // 内存中可能放不下，实现外部排序
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            records_.emplace_back(prev_->Next());
            if (records_.size() >= MAX_CHUNK_SIZE) {
                sortAndSaveChunk();
            }
        }

        // 剩下未满最大值的记录们也写入磁盘
        if (!records_.empty()) {
            sortAndSaveChunk();
        }

        // 合并所有块
        mergeSortedChunks();

        outfile_.close();

        // 读模式
        outfile_.open(filename_, std::ios::in);

        // 缓存记录
        do {
            current_tuple_ = std::make_unique<RmRecord>(len_);
            outfile_.read(current_tuple_->data, current_tuple_->size);
            if (outfile_.gcount() == 0) {
                break;
            }
            records_.emplace_back(std::move(current_tuple_));
        } while (records_.size() < MAX_CHUNK_SIZE);

        if (!records_.empty()) {
            current_tuple_ = std::move(records_.front());
            records_.pop_front();
        } else {
            is_end_ = true;
            current_tuple_ = nullptr;
            outfile_.close();
        }
    }

    void nextTuple() override {
        if (!records_.empty()) {
            current_tuple_ = std::move(records_.front());
            records_.pop_front();
            return;
        }

        do {
            current_tuple_ = std::make_unique<RmRecord>(len_);
            outfile_.read(current_tuple_->data, current_tuple_->size);
            if (outfile_.gcount() == 0) {
                break;
            }
            records_.emplace_back(std::move(current_tuple_));
        } while (records_.size() < MAX_CHUNK_SIZE);

        if (!records_.empty()) {
            current_tuple_ = std::move(records_.front());
            records_.pop_front();
        } else {
            is_end_ = true;
            current_tuple_ = nullptr;
            outfile_.close();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(current_tuple_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const { return is_end_; }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    size_t tupleLen() const override { return prev_->tupleLen(); }
};
