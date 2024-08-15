#!/bin/bash

# Flex 输入文件
FLEX_INPUT="lex.l"
# Flex 输出文件
FLEX_OUTPUT_CPP="lex.yy.cpp"
FLEX_OUTPUT_HEADER="lex.yy.hpp"

# Bison 输入文件
BISON_INPUT="yacc.y"
# Bison 输出文件
BISON_OUTPUT_CPP="yacc.tab.cpp"
BISON_OUTPUT_HEADER="yacc.tab.hpp"

# 检查 Flex 输入文件是否存在
if [ ! -f "$FLEX_INPUT" ]; then
  echo "Error: Flex input file '$FLEX_INPUT' not found!"
  exit 1
fi

# 检查 Bison 输入文件是否存在
if [ ! -f "$BISON_INPUT" ]; then
  echo "Error: Bison input file '$BISON_INPUT' not found!"
  exit 1
fi

# 运行 Flex 命令
echo "Running Flex..."
flex --header-file="$FLEX_OUTPUT_HEADER" -o "$FLEX_OUTPUT_CPP" "$FLEX_INPUT"
if [ $? -ne 0 ]; then
  echo "Error: Flex failed!"
  exit 1
fi
echo "Flex completed successfully."

# 运行 Bison 命令
echo "Running Bison..."
bison --defines="$BISON_OUTPUT_HEADER" -o "$BISON_OUTPUT_CPP" "$BISON_INPUT"
if [ $? -ne 0 ]; then
  echo "Error: Bison failed!"
  exit 1
fi
echo "Bison completed successfully."

echo "Build process completed."
