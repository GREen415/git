#define _POSIX_C_SOURCE 200809L  // 启用 POSIX 扩展

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


// 全局变量
struct editorConfig E;

/*** 常量定义 ***/
#define CTRL_KEY(k) ((k) & 0x1f)  // 获取控制键的宏
#define EDITOR_VERSION "0.0.1"    // 编辑器版本
#define EDITOR_TAB_STOP 4         // Tab键的空格数
#define QUIT_TIMES 3              // 退出确认次数

// 特殊键枚举（避免与普通字符冲突）
enum editorKey {
    BACKSPACE = 127,  // 退格键
    ARROW_LEFT = 1000,//左右上下箭头，表示光标的移动
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,//左移右移，表示翻页（光标移动到最上面最下面）
    PAGE_DOWN,//右移左移，表示翻页
    HOME_KEY,//光标移动到行首
    END_KEY,//光标移动到行尾
    DEL_KEY// 删除键
};

/*** 数据结构 ***/
// 文本行结构
typedef struct erow {
    char *chars;      // 原始字符
    char *render;     // 渲染后的字符（将制表符转换为空格等）
    int len;          // 原始长度
    int rlen;         // 渲染后长度
} erow;

// 编辑器配置结构体（全局状态）
struct editorConfig {
    int cx, cy;        // 光标位置（基于原始字符）
    int rx;            // 光标的渲染位置（用于处理制表符）
    int rowoff;        // 行偏移（用于垂直滚动）
    int coloff;        // 列偏移（用于水平滚动）
    int screenrows;    // 屏幕行数
    int screencols;    // 屏幕列数
    int numrows;       // 总行数
    erow *row;         // 行数组
    int dirty;         // 文件是否被修改
    char *filename;    // 文件名
    char statusmsg[80];// 状态消息
    time_t statusmsg_time; // 状态消息显示时间
    struct termios orig_termios; // 原始终端设置
};

// 函数声明
void die(const char *s);//错误处理函数
void disableRawMode();//禁用原始模式
void enableRawMode();//启用原始模式

int editorReadKey();//读取编辑器按键（处理特殊键）
int getWindowSize(int *rows, int *cols);//获取终端窗口大小

void editorUpdateRow(erow *row);//将制表符转换为空格，并计算渲染后的位置
void editorInsertRow(int at, char *s, size_t len);//在指定位置插入一行

// ...其他函数声明
void editorRefreshScreen();  // 特别添加

struct editorConfig E; // 全局编辑器配置



/*** 终端控制 ***/
/**
 * 错误处理函数
 * @param s 错误信息
 */
void die(const char *s) {
    // 清屏并重置光标
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);  // 打印错误信息
    exit(1);    // 退出程序
}

/**
 * 禁用原始模式：恢复终端原始设置
 */
void disableRawMode() {
    // 使用TCSAFLUSH选项：等待所有输出完成，丢弃未读输入
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");  // 如果设置失败则报错
}

/**
 * 启用原始模式
 */
void enableRawMode() {
    // 获取当前终端属性
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) 
        die("tcgetattr");
    
    // 注册退出处理函数：确保程序退出时恢复终端设置（无论以何种方式退出均运行括号内的程序）
    atexit(disableRawMode);
    
    // 创建终端设置的副本用于修改
    struct termios raw = E.orig_termios;
    
    // 修改本地模式标志：
    // ~ECHO: 禁用输入回显
    // ~ICANON: 禁用规范模式（逐行输入）
    // ~ISIG: 禁用信号处理（Ctrl+C等）
    // ~IEXTEN: 禁用扩展输入处理
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    
    // 修改输入模式标志：
    // ~BRKINT: 禁用中断信号
    // ~INPCK: 禁用奇偶校验
    // ~ISTRIP: 禁止剥离第8位
    // ~IXON: 禁用软件流控制（Ctrl+S/Ctrl+Q）
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | IXON);
    
    // 修改输出模式标志：禁用输出处理
    raw.c_oflag &= ~(OPOST);
    
    // 设置控制字符：
    // VMIN=0: 最小读取字节数为0
    // VTIME=1: 超时时间为100ms (1 = 0.1秒)
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    
    // 应用修改后的终端设置
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

/**
 * 读取编辑器按键（处理特殊键）
 * @return 读取的键值
 */
int editorReadKey() {
    int nread;
    char c;
    // 读取一个字符
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // 处理转义序列（特殊键）
    if (c == '\x1b') {
        char seq[3];
        
        // 尝试读取转义序列的后续字符
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        
        // 检查是否是箭头键
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // 读取第三个字符（如Page Up/Down）
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        
        return '\x1b';  // 无法识别的转义序列
    } else {
        return c;  // 普通字符
    }
}

/**
 * 获取终端窗口大小
 * @param rows 行数存储指针
 * @param cols 列数存储指针
 * @return 成功返回0，失败返回-1
 */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;  // 窗口大小结构体，存储终端尺寸信息
    
    // 使用ioctl获取窗口大小
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        //&ws：存储结果的缓冲区 TIOCGWINSZ：获取窗口大小的命令码
        // 如果ioctl失败，使用备用方法
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return -1; // 简化处理
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}





/*** 行操作 ***/
/**
 * 将制表符转换为空格，并计算渲染后的位置
 * @param row 文本行
 */
void editorUpdateRow(erow *row) {
    int tabs = 0;//tabs变量存储该行中\t的数量
    int j;
    
    // 计算制表符数量
    for (j = 0; j < row->len; j++)
        if (row->chars[j] == '\t') tabs++;
        //预先统计制表符数量，以便准确计算所需内存
    
    // 分配渲染后字符串的内存
    free(row->render);
    row->render = malloc(row->len + tabs*(EDITOR_TAB_STOP - 1) + 1);
    
    int idx = 0;
    for (j = 0; j < row->len; j++) {
        if (row->chars[j] == '\t') {
            // 插入空格直到下一个制表符停止位
            row->render[idx++] = ' ';
            while (idx % EDITOR_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rlen = idx;
}

/**
 * 在指定位置插入一行
 * @param at 插入位置
 * @param s 行内容
 * @param len 行长度
 */
void editorInsertRow(int at, char *s, size_t len) {
    // 调整行数组内存
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    if (at < E.numrows) {
        memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    }
    
    // 初始化新行
    E.row[at].len = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    
    E.row[at].rlen = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);
    
    E.numrows++;
    E.dirty = 1; // 标记文件已修改
}

/**
 * 释放行内存
 * @param row 文本行
 */
void editorFreeRow(erow *row) {
    free(row->render);// 释放渲染后的字符串
    free(row->chars);// 释放原始字符
}

/**
 * 删除指定行
 * @param at 行号
 */
void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    
    // 释放该行内存
    editorFreeRow(&E.row[at]);
    
    // 移动后续行
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;//总行数减一
    E.dirty = 1; // 标记文件已修改
}

/**
 * 在行中插入字符
 * @param row 文本行
 * @param at 插入位置
 * @param c 要插入的字符
 */
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->len) at = row->len;
    
    // 重新分配内存（增加1字节）
    row->chars = realloc(row->chars, row->len + 2);
    
    // 移动后续字符
    memmove(&row->chars[at + 1], &row->chars[at], row->len - at + 1);
    row->len++;
    row->chars[at] = c;
    
    editorUpdateRow(row);
    E.dirty = 1; // 标记文件已修改
}

/**
 * 删除行中的字符
 * @param row 文本行
 * @param at 删除位置
 */
void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->len) return;
    
    // 移动后续字符
    memmove(&row->chars[at], &row->chars[at + 1], row->len - at);
    row->len--;
    
    editorUpdateRow(row);
    E.dirty = 1; // 标记文件已修改
}





/*** 编辑器操作 ***/

/**
 * 在光标位置插入字符
 * @param c 字符
 */
void editorInsertChar(int c) {
    // 如果光标在文件末尾，插入新行
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    
    // 在行中插入字符
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

// ... 其他代码 ...


/**
 * 插入新行
 */
void editorInsertNewline() {
    // 如果在行首，直接插入空行
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        // 获取当前行
        erow *row = &E.row[E.cy];
        
        // 确保不会越界
        int new_line_len = row->len - E.cx;
        if (new_line_len < 0) new_line_len = 0;
        
        // 创建新行内容
        char *new_line = malloc(new_line_len + 1);
        if (!new_line) die("malloc");
        if (new_line_len > 0) {
            memcpy(new_line, &row->chars[E.cx], new_line_len);
        }
        new_line[new_line_len] = '\0';
        
        // 插入新行
        editorInsertRow(E.cy + 1, new_line, new_line_len);
        free(new_line);
        
        // 更新当前行
        char *new_chars = malloc(E.cx + 1);
        if (!new_chars) die("malloc");
        if (E.cx > 0) {
            memcpy(new_chars, row->chars, E.cx);
        }
        new_chars[E.cx] = '\0';
        
        free(row->chars);
        row->chars = new_chars;
        row->len = E.cx;
        editorUpdateRow(row);
    }
    
    // 移动到新行
    E.cy++;
    
    // 重置光标位置到行首
    E.cx = 0;
    E.rx = 0;  // 重要：重置渲染位置
    
    E.dirty = 1; // 标记文件已修改
    
    // 强制滚动到新行
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
}

// ... 其他代码 ...
/**
 * 删除字符（退格键）
 */
void editorDelChar() {
    // 如果光标在文件开头，无操作
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;
    
    erow *row = &E.row[E.cy];
    
    if (E.cx > 0) {
        // 删除光标前字符
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        // 删除行首：合并到上一行
        E.cx = E.row[E.cy - 1].len;
        // 将当前行内容追加到上一行
        char *new_chars = malloc(E.row[E.cy - 1].len + row->len + 1);
        memcpy(new_chars, E.row[E.cy - 1].chars, E.row[E.cy - 1].len);
        memcpy(new_chars + E.row[E.cy - 1].len, row->chars, row->len);
        new_chars[E.row[E.cy - 1].len + row->len] = '\0';
        
        free(E.row[E.cy - 1].chars);
        E.row[E.cy - 1].chars = new_chars;
        E.row[E.cy - 1].len += row->len;
        editorUpdateRow(&E.row[E.cy - 1]);
        
        // 删除当前行
        editorDelRow(E.cy);
        E.cy--;
    }
}

/**
 * 移动光标
 * @param key 方向键
 */
void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].len;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->len) {
                E.cx++;
            } else if (row && E.cx == row->len) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy > 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }
    
    // 确保光标位置有效
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->len : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}





/*** 文件操作 ***/
/**
 * 将行数组转换为字符串
 * @param buflen 返回字符串长度
 * @return 字符串指针（需调用者释放）
 */
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int i;
    
    // 计算总长度（包括每行的换行符）
    for (i = 0; i < E.numrows; i++)
        totlen += E.row[i].len + 1;  // +1为换行符
    
    *buflen = totlen;
    
    char *buf = malloc(totlen);
    char *p = buf;
    
    // 复制每行内容
    for (i = 0; i < E.numrows; i++) {
        memcpy(p, E.row[i].chars, E.row[i].len);
        p += E.row[i].len;
        *p = '\n';
        p++;
    }
    
    return buf;
}

/**
 * 打开文件并读取内容
 * @param filename 文件名
 */
void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);  // 复制文件名
    
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");
    
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    
    // 逐行读取文件内容
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        // 移除行尾的换行符
        while (linelen > 0 && (line[linelen - 1] == '\n' || 
                               line[linelen - 1] == '\r'))
            linelen--;
        
        // 将行插入编辑器
        editorInsertRow(E.numrows, line, linelen);
    }
    
    free(line);
    fclose(fp);
    E.dirty = 0; // 文件未修改
}

/**
 * 保存文件
 */
void editorSave() {
    if (E.filename == NULL) {
        // 提示输入文件名（简化处理）
        E.filename = "newfile.txt";
    }
    
    int len;
    char *buf = editorRowsToString(&len);
    
    // 打开文件（读写模式，不存在则创建）
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) die("open");
    
    // 设置文件长度为len（截断或扩展）
    if (ftruncate(fd, len) == -1) die("ftruncate");
    
    // 写入文件
    if (write(fd, buf, len) != len) die("write");
    
    close(fd);
    free(buf);
    E.dirty = 0;  // 重置修改标志
    
    // 设置状态消息
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Saved %d bytes to %s", len, E.filename);
    E.statusmsg_time = time(NULL);
}

/*** 输入处理 ***/
/**
 * 获取用户输入
 * @param prompt 提示信息
 * @return 用户输入的字符串（需调用者释放）
 */
char *editorPrompt(char *prompt) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    
    size_t buflen = 0;
    buf[0] = '\0';
    
    while (1) {
        // 设置状态消息
        snprintf(E.statusmsg, sizeof(E.statusmsg), "%s: %s (ESC to cancel)", prompt, buf);
        editorRefreshScreen();
        
        int c = editorReadKey();
        if (c == '\x1b') { // ESC键
            free(buf);
            return NULL;
        } else if (c == '\r') { // 回车键
            if (buflen != 0) {
                return buf;
            }
        } else if (c == BACKSPACE || c == DEL_KEY) {
            if (buflen > 0) buf[--buflen] = '\0';
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

/*** 输出处理 ***/
/**
 * 滚动编辑器内容，确保光标可见
 */
void editorScroll() {
    // 计算光标的渲染位置（处理制表符）
    E.rx = 0;
    if (E.cy < E.numrows) {
        erow *row = &E.row[E.cy];
        for (int j = 0; j < E.cx; j++) {
            if (row->chars[j] == '\t')
                E.rx += (EDITOR_TAB_STOP - 1) - (E.rx % EDITOR_TAB_STOP);
            E.rx++;
        }
    }
    
    // 垂直滚动
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    
    // 水平滚动
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

/**
 * 绘制屏幕内容
 */
void editorDrawRows() {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        
        // 在文件行范围内
        if (filerow < E.numrows) {
            // 获取该行的渲染内容
            erow *row = &E.row[filerow];
            
            // 计算要显示的渲染内容长度
            int len = row->rlen - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            
            // 显示渲染内容
            write(STDOUT_FILENO, &row->render[E.coloff], len);
        }
        
        // 清除行尾
        write(STDOUT_FILENO, "\x1b[K", 3);
        
        // 换行（最后一行除外）
        if (y < E.screenrows - 1) {
            write(STDOUT_FILENO, "\r\n", 2);
        }
    }
}

/**
 * 绘制状态栏
 */
void editorDrawStatusBar() {
    // 设置反色显示
    write(STDOUT_FILENO, "\x1b[7m", 4);
    
    char status[80], rstatus[80];
    // 左侧状态：文件名和修改状态
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                      E.filename ? E.filename : "[No Name]",
                      E.numrows,
                      E.dirty ? "(modified)" : "");
    
    // 右侧状态：行号
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
                       E.cy + 1, E.numrows);
    
    // 确保状态栏长度不超过屏幕宽度
    if (len > E.screencols) len = E.screencols;
    write(STDOUT_FILENO, status, len);
    
    // 填充剩余空间
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            write(STDOUT_FILENO, rstatus, rlen);
            break;
        } else {
            write(STDOUT_FILENO, " ", 1);
            len++;
        }
    }
    
    // 重置显示属性
    write(STDOUT_FILENO, "\x1b[m", 3);
    write(STDOUT_FILENO, "\r\n", 2);
}

/**
 * 清屏并重新绘制界面
 */
void editorRefreshScreen() {
    // 滚动以确保光标可见
    editorScroll();
    
    // 隐藏光标
    write(STDOUT_FILENO, "\x1b[?25l", 6);
    // 移动光标到左上角
    write(STDOUT_FILENO, "\x1b[H", 3);
    
    // 绘制文本行
    editorDrawRows();
    // 绘制状态栏
    editorDrawStatusBar();
    
    // 移动光标到正确位置
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    write(STDOUT_FILENO, buf, strlen(buf));
    
    // 显示光标
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/**
 * 设置状态消息
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}





/*** 初始化 ***/
/**
 * 初始化编辑器
 */
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';//状态消息
    E.statusmsg_time = 0;//状态消息显示时间
    
    // 获取终端大小
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    
    // 状态栏和消息栏各占一行
    E.screenrows -= 2;
}

/**
 * 主函数
 * @param argc 参数个数
 * @param argv 参数数组
 */
int main(int argc, char *argv[]) {
    // 启用原始模式
    enableRawMode();
    // 初始化编辑器
    initEditor();
    
    // 打开文件
    if (argc >= 2) {
        editorOpen(argv[1]);
    } 
    
    // 主循环
    while (1) {
        editorRefreshScreen();
        int c = editorReadKey();//读取编辑器按键，返回读取的键值
        
        switch (c) {
        //特殊输入处理
            case HOME_KEY:
                E.cx = 0;
                break;
                
            case END_KEY:
                if (E.cy < E.numrows)
                    E.cx = E.row[E.cy].len;
                break;
                
            case PAGE_UP:
            case PAGE_DOWN: {
                // 翻页：移动光标到屏幕顶部/底部
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                break;
            }
                
            case ARROW_UP:
            case ARROW_DOWN:
            case ARROW_LEFT:
            case ARROW_RIGHT:
                editorMoveCursor(c);
                break;
                
            case '\r':  // 回车键
                editorInsertNewline();
                break;
                
            case BACKSPACE:   // 退格键
            case DEL_KEY:     // Delete键
                editorDelChar();
                break;
                
            default:
            // 插入普通字符
                editorInsertChar(c);
                break;
        }
    }
    
    return 0;
}