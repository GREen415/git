# 文本编辑器构建文件
# 最终版本

# 设置编译器为gcc
CC = gcc

# 设置编译选项：
# -Wall: 启用所有警告
# -Wextra: 启用额外警告
# -pedantic: 严格遵循ISO C标准
# -std=c99: 使用C99标准
CFLAGS = -Wall -Wextra -pedantic -std=c99

# 目标可执行文件名
TARGET = editor

# 默认构建目标
all: $(TARGET)

# 链接目标：将main.o链接成可执行文件
$(TARGET): main.o
	$(CC) $(CFLAGS) -o $@ $^

# 编译main.c生成目标文件
main.o: main.c
	$(CC) $(CFLAGS) -c $<

# 清理生成的文件
clean:
	rm -f $(TARGET) *.o