/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

/**
 * @typedef YY_BUFFER_STATE
 * 表示词法分析器的缓冲区状态。
 */
typedef struct yy_buffer_state *YY_BUFFER_STATE;

/**
 * @typedef yyscan_t
 * 表示词法分析器实例的句柄。
 */
typedef void *yyscan_t;

/**
 * @brief 解析输入源。
 *
 * @param yyscanner 词法分析器实例的句柄。
 * @return 解析结果的状态码。
 */
int yyparse(yyscan_t yyscanner);

/**
 * @brief 初始化词法分析器实例，并传递用户定义的数据。
 *
 * @param scanner 指向词法分析器实例句柄的指针。
 * @return 初始化结果的状态码。
 */
int yylex_init(yyscan_t *scanner);

/**
 * @brief 扫描指定的字符串，并创建一个词法分析器缓冲区。
 *
 * @param yy_str 要扫描的字符串。
 * @param yyscanner 词法分析器实例的句柄。
 * @return 创建的词法分析器缓冲区的句柄。
 */
YY_BUFFER_STATE yy_scan_string(const char *yy_str, yyscan_t yyscanner);

/**
 * @brief 删除指定的词法分析器缓冲区。
 *
 * @param buffer 要删除的词法分析器缓冲区句柄。
 * @param yyscanner 词法分析器实例的句柄。
 */
void yy_delete_buffer(YY_BUFFER_STATE buffer, yyscan_t yyscanner);

/**
 * @brief 销毁指定的词法分析器实例。
 *
 * @param yyscanner 要销毁的词法分析器实例的句柄。
 * @return 销毁结果的状态码。
 */
int yylex_destroy(yyscan_t yyscanner);
