/*  简单日志系统 v2.0
 *  此模块用于简单输出日志到屏幕和写入到文件
 *  系统内部使用 redisdic 维护 日志结构数据, 使用 日志结构的 name 进行区分不同的日志, 用户不用关心底层,
 *  所以即使是多线程, 也可以通过 name 把信息添加到指定的日志结构中
 *
 *  使用的 redisdic 结构是 redis3.0 中的原型, 但是所有的函数都重新封装, 一般情况下不会和正常的 redis 冲突
 *
 *  使用方式:
 *       1. logsysInit()    开启日志系统, 如有必要, 可使用 logsysSet*() 相关函数进行相关设置
 *       2. logCreate()     添加用户日志结构
 *       3. logAdd*()       添加日志到指定的日志结构中, 每次添加会保证把信息写入到关联的文件中
 *       4. logsysRelease() 停止和释放资源, 其实这一步可省略, 日志系统应该时伴随整个程序流程的, 程序结束的时候会自动释放资源
 *                          在使用 valgrind 测试时一定要加上这一步
 *
 *  相关说明:
 *      0. 系统日志文件在  ./logs/sys.out 中, 不可通过 API 修改, 如要更改, 修改宏定义 LOGSYS_PATH 即可
 *      1. 系统日志大小默认为 1M, 即当系统日志文件大于 1M 时, 会自动清空, 可通过 logsysSetFileSize(size_mb), 进行设置, 但每次都须重新设置
 *      2. 系统日志默认为静默模式, 即所有的正常操作只记录到日志中, 不输出到控制台, 但操作异常会输出相关信息到控制台
 *      3. 用户日志大小默认为 100M, 可使用 logSetFileSize(size_mb), 每次使用都须重新设置, 每个用户日志均有自己的属性, 互不影响
 *
 * 注意:
 *      本日志系统并没有执行相同文件测试, 即两个日志结构可以指向同一个文件
 *
 * author: ziyht
 *
*/

#include <stdio.h>      // FILE
#include <stdbool.h>    //
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef LOG_H
#define LOG_H


#define LOG_ERR     0
#define LOG_OK      1
#define LOGDICT_ERR 2
#define LOGDICT_OK  3

#define DF_LOG_DIR     "./logs/"               // 默认的日志文件存放地点
#define DF_LOG_SIZE    100                     // 默认日志文件大小 100 M

#define NMUTE false
#define MUTE  true

typedef struct Log{
    char* name;         // 本日志的名称, 每次输出的时候都会附带, 以区分不同的日志信息
    char* path;         // 存储日志文件的位置
    FILE* fp;           // 文件流指针, 指向存储日志的本地文件
    size_t maxsize;     // 最大文件大小, 默认为 0, 表示不设限制
    bool mutetype;      // 静默属性, 决定在添加日志时是否显示到控制台上
}* LogPtr;

/* ------------------------------- logdict struct ------------------------------------*/

/* 字典的原子数据结构, 实际的数据存储在这里 */
typedef struct logdictEntry {
    void *key;                  // key
    LogPtr v;                   // value
    struct logdictEntry *next;  // 用于 iterator 迭代时使用
} logdictEntry;

/* 字典类型, 每个dict都拥有一个自己dictType, 使得每个dict不同,
 * 如hash函数不同, 键值复制, 比较不同等等, 使得 dict 的用途多元化 */
typedef struct logdictType {
    unsigned int (*hashFunction) (const void *key);
    void         (*valDestructor)(void *obj);
} logdictType;

/* hash 表结构, 每一个字典都有两个 hash 表, 多的一个是用于扩容时使用,
 * 当一个 hash 表满了, 从旧表向新表进行 rehash 操作
 */
typedef struct logdictht {
    logdictEntry **table;   // 指向一个由字典原子结构指针组成的数组: &|dictEntry*|dictEntry*|dictEntry*|dictEntry*|...|
    unsigned long size;     // 当前 hash 表的容量, 2的倍数, 2| 4|  8|  16|...
    unsigned long sizemask; // 大小掩码, 用以 hash 后定位,  1|11|111|1111|..., 实际就是 size-1
    unsigned long used;     // 当前 hash 表的使用量
} logdictht;

/* 字典顶层结构 */
typedef struct logdict {
    logdictType *type;  // 字典类型
    void *privdata;     //
    logdictht ht[2];    // 维护两个 hash 表
    long rehashidx;     // if rehashidx == -1 表示当前没有进行扩容
    int iterators;      // 特指当前 safe iterators 的数量
} logdict;

#define DICT_HT_INITIAL_SIZE  4

// #define assert(c) (c)
#define logdictHashKey(d, key) (d)->type->hashFunction(key)
#define logdictIsRehashing(d) ((d)->rehashidx != -1)
#define logdictCompareKeys(d, key1, key2) !strcmp((key1), (key2))
#define logdictSetKey(d, entry, _key_) do { \
        entry->key = strdup(_key_); \
} while(0)

#define logdictSetVal(d, entry, _val_) do { \
        entry->v = (_val_); \
} while(0)
#define logdictFreeKey(d, entry) free((entry)->key)
#define logdictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((entry)->v)
#define logdictGetKey(he) ((he)->key)
#define logdictGetVal(he) ((he)->v)

/* ------------------------------- logsys API ------------------------------------*/
#define LOGSYS_PATH     "./logs/sys.out"

#define DF_SYS_LOG            NULL    // 系统日志结构指针
#define DF_LOGSYS_SERVICE     false   // 系统日志初始化状态, 只有为 true , 以下 SET API 才有效
#define DF_LOGSYS_MUTETYPE    MUTE    // 系统日志静默属性, 默认静默
#define DF_LOGSYS_FILESIZE    1       // 系统日志大小, 默认为 1 M
#define DF_LOGSYS_DIC         NULL    // 日志系统维护的日志结构字典
#define DF_LOGSYS_DICTYPE     NULL    // 日志字典类型

int  logsysInit();                              // 初始化日志系统
void logsysStop();                              // 停用日志系统
void logsysRelease();                           // 停用日志系统 并 释放资源
void logsysSetMutetype(bool mutetype);          // 设置日志系统静默属性
int  logsysSetFileSize(size_t size_mb);         // 设置系统日志最大文件大小
int  logsysFlieEmpty();                         // 清空系统日志文件

// 以下不建议用户使用
int  logsysShowTime();                              // 当系统日志无法输出(未开启或静默)时, 在控制台上显示时间
int  logsysShowText(const char* text, ...);         // 当系统日志无法输出(未开启或静默)时, 在控制台上显示 text
int  logsysShow(const char* text, ...);             // 当系统日志无法输出(未开启或静默)时, 在控制台上显示 时间 和 text
void logsysAddText(LogPtr log, const char* text, ...);      // 添加 text 到系统日志中, 由系统日志静默属性决定是否输出到控制台
void logsysAddTextMute(LogPtr log, const char* text, ...);  // 添加 text 到系统日志中, 强制静默
void logsysAddTextNMute(LogPtr log, const char* text, ...); // 添加 text 到系统日志中, 强制非静默
void logsysAdd(LogPtr log, const char* text, ...);          // 添加 时间 和 text 到系统日志中, 由系统日志静默属性决定是否输出到控制台
void logsysAddMute(LogPtr log, const char* text, ...);      // 添加 时间 和 text 到系统日志中, 强制静默
void logsysAddNMute(LogPtr log, const char* text, ...);     // 添加 时间 和 text 到系统日志中, 强制非静默

/* ------------------------------- log API ------------------------------------*/
// 独立输出API, 这部分直接输出到 控制台
void logShowTime();                         // 打印当前时间到控制台
void logShowText(const char* text, ...);    // 打印 text 到控制台, 不添加任何东西, 和 printf 相同的功能
void logShow(const char* text, ...);        // 打印时间和内容到控制台

// 创建及修改相关 API
int    logCreate(const char* name, const char* path, bool mutetype);    // 创建一个 日志 结构
int    logDestroy(const char* name);                          // 销毁一个 日志 结构
size_t logFileSize(const char* name);                         // 获取日志结构所指文件的大小
int    logSetFileSize(const char* name, size_t size_mb);      // 设置文件大小限制, 单位为 MB
void   logSetMutetype(const char* name, bool mutetype);       // 设置日志结构的 静默 属性
int    logFlieEmpty(const char* name);                        // 清空结构所指日志文件

// 添加日志 API
void logAddTime(const char* name);                            // 添加当前时间到 日志 中, 由 (*log).mute 决定是否静默处理
void logAddTimeMute(const char* name);                        // 添加当前时间到 日志 中, 强制静默处理
void logAddTimeNMute(const char* name);                       // 添加当前时间到 日志 中, 强制非静默处理
void logAddText(const char* name, const char* text, ...);     // 添加 text 到 日志 中, 由 (*log).mute 决定是否静默处理
void logAddTextMute(const char* name, const char* text, ...); // 添加 text 到 日志 中, 强制静默处理
void logAddTextNMute(const char* name, const char* text, ...);// 添加 text 到 日志 中, 强制非静默处理
void logAdd(const char* name, const char* text, ...);         // 添加 时间 和 text 到 日志中, 由 (*log).mute 决定是否静默处理
void logAddMute(const char* name, const char* text, ...);     // 添加 时间 和 text 到 日志中, 强制静默处理
void logAddNMute(const char* name, const char* text, ...);    // 添加 时间 和 text 到 日志中, 强制非静默处理

/* ------------------------------- Test Function ------------------------------------*/
// ...

#endif
