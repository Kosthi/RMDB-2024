#pragma once

#include <sys/mman.h>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class LoadExecutor : public AbstractExecutor {
private:
    TabMeta tab_; // 表的元数据
    RmFileHandle *fh_; // 表的数据文件句柄
    std::string filename_; // 载入文件名
    std::string tab_name_; // 表名称
    SmManager *sm_manager_;
    int record_len_; // 记录大小
    int max_nums_; // 一个页面能放入的最大记录数量

public:
    LoadExecutor(SmManager *sm_manager, std::string &filename, std::string &tab_name,
                 Context *context) : filename_(std::move(filename)), tab_name_(std::move(tab_name)) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        context_ = context;
        record_len_ = fh_->get_file_hdr().record_size;
        max_nums_ = fh_->get_file_hdr().num_records_per_page;
    }

    std::unique_ptr<RmRecord> Next() override {
        int fd = open(filename_.c_str(), O_RDONLY);
        if (fd == -1) {
            perror("Error opening file");
            return nullptr;
        }

        // 得到文件大小
        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            perror("Error getting file size");
            close(fd);
            return nullptr;
        }

        size_t file_size = sb.st_size;

        // Memory map the file
        char *file_content = static_cast<char *>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (file_content == MAP_FAILED) {
            perror("Error mapping file");
            close(fd);
            return nullptr;
        }

        std::string currentLine;
        std::vector<std::string> cols;

        // 跳过表头
        size_t i = 0;
        // 使用 std::find 查找换行符的位置
        const char *newlinePos = std::find(file_content, file_content + file_size, '\n');
        // 一定存在表头
        // if (newlinePos != file_content + file_size) {
        i = newlinePos - file_content + 1; // 跳过换行符
        // }

        // 按页刷入
        auto page_size = max_nums_ * record_len_;
        char *data = new char[page_size * 2];
        char *cur = data;
        // 从第一页开始放数据
        int page_no = 1;

        int row = 0;
        int nums_record = 0;

        // 读取到 data 中
        auto &cols_meta = tab_.cols;
        for (; i < file_size; ++i) {
            if (file_content[i] == '\n' || i == file_size - 1) {
                if (i == file_size - 1 && file_content[i] != '\n') {
                    currentLine += file_content[i];
                }
                cols = std::move(split(currentLine, ','));
                for (int col = 0; col < cols.size(); ++col) {
                    switch (cols_meta[col].type) {
                        case TYPE_INT: {
                            *(int *) (cur + cols_meta[col].offset) = std::stoi(cols[col]);
                            break;
                        }
                        case TYPE_FLOAT: {
                            *(float *) (cur + cols_meta[col].offset) = std::stof(cols[col]);
                            break;
                        }
                        case TYPE_STRING: {
                            memcpy(cur + cols_meta[col].offset, cols[col].c_str(), cols[col].size());
                            break;
                        }
                        default:
                            break;
                    }
                }

                // Unique Index -> Insert into index
                for (auto &[index_name, index]: tab_.indexes) {
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    char *key = new char[index.col_tot_len];
                    for (auto &[index_offset, col_meta]: index.cols) {
                        memcpy(key + index_offset, cur + col_meta.offset, col_meta.len);
                    }
                    ih->insert_entry(key, {page_no, row % max_nums_}, context_->txn_);
                    delete []key;
                }

                cur += record_len_;

                // 满足一页或者读到最后了，刷进去
                if ((row + 1) % max_nums_ == 0 || i == file_size - 1) {
                    nums_record = (row + 1) % max_nums_ == 0 ? (row == 0 ? 1 : max_nums_) : (row + 1) % max_nums_;
                    fh_->load_record(page_no, data, nums_record, cur - data);
                    ++page_no;
                    cur = data;
                }

                ++row;
                currentLine.clear();
            } else {
                currentLine += file_content[i];
            }
        }

        // Unmap the file and close the file descriptor
        if (munmap(file_content, file_size) == -1) {
            perror("Error unmapping file");
        }
        close(fd);

        if (nums_record < max_nums_) {
            fh_->file_hdr_.first_free_page_no = page_no - 1;
        }

        delete []data;
        // sm_manager_->get_disk_manager()->set_fd2pageno(fh_->GetFd(), page_no);

        // printf("load success file: %s, tab: %s\n", filename_.c_str(), tab_name_.c_str());
        return nullptr;
    }

    // Function to split a string by a delimiter
    static std::vector<std::string> split(const std::string &s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.emplace_back(token);
        }
        return tokens;
    }

    Rid &rid() override { return _abstract_rid; }
};
