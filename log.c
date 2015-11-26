
#include "log.h"

/* ---------------------- logdict private prototypes ---------------------------- */
static int dict_can_resize = 1;
static unsigned int dict_force_resize_ratio = 5;

static int _logdictInit(logdict *d, logdictType *type, void *privDataPtr);
static unsigned long _logdictNextPower(unsigned long size);
static int _logdictKeyIndex(logdict *d, const void *key);
static int _logdictExpandIfNeeded(logdict *d);

static unsigned int _dictGenHashFunction(const char *key, int len);   // MurmurHash2 算法
static logdict* _logdictCreate(logdictType *type, void *privDataPtr); // 创建一个新字典
static int _logdictExpand(logdict *d, unsigned long size);            // 扩展或创建新的 hashtable
//static int _logdictAdd(logdict *d, const char *key, void *val);       // 添加 key value 到 d 中, 内部调用 dictAddRaw() 和 宏 dictSetVal() 实现
static logdictEntry* _logdictAddRaw(logdict *d, const char *key);     // 创建一个包含 KEY 的 dictEntry 对象, 添加到 dict 中, 然后返回该对象的指针, 供上层使用(设置值)
static int _logdictRehash(logdict *d, int n);                         // 对字典的 n 个桶进行 rehash，计算每个桶内链表所有 key 并计算 hash 后， 存放在 ht[1] 中，并将 ht[0] 中处理过的桶指针指向空
static int _logdictDelete(logdict *d, const char *key);               // 查找并在 hashtable 删除某个 key 所对应的结构, 释放内存
static void _logdictRelease(logdict *d);                              // 将字典所有节点清空，并释放字典
static logdictEntry* _logdictFind(logdict *d, const char *key);       // 在字典中查找 key 并返回找到的 entry（会遍历 ht[0]和ht[1]）
static LogPtr _logdictFetchValue(logdict *d, const char *key);        // 查找并返回指定 key 对应的 valv


/* -------------------------- log private prototypes ---------------------------- */
#define TS_LOG  0
#define TS_FILE 1
static char* _timeStr(int type);                                    // 返回一个存储当前本地时间的静态字符串指针

typedef int status;
#define FILE_NOTEXIST   0
#define FILE_NOTWRITE   2
#define FILE_CANWRITE   1
static status _GetFileStatus(const char* path);                     // 获取文件状态

#define MAX_PATH_LENGTH     255
static char* _logPath(const char* dir, const char* name);           // 获取一个临时的 path 字串, 不要 free

static void _mkdir(const char* path, mode_t mode);                  // 根据路径依次创建文件夹, 直到文件的最底层
static void _logFileShrink(LogPtr log);                             // 若 日志文件 已达上限, 则清空文件
static LogPtr _logGenerate(const char* name, const char* path, bool mutetype);
static void _logReset(LogPtr log);
static size_t _logFileSize(LogPtr log);
static int _logFlieEmpty(LogPtr log);

/* ---------------------- logcheck private prototypes ---------------------------- */
static int _check_logsys(const char* name, const char* tag);         // 检查服务是否开启, 并输出相应提示信息
static int _check_name(const char* name, const char* tag);           // 检查 name 是否合法, 并输出相应提示信息
static int _check_path(const char* path, const char* tag);           // 检查 path 是否合法, 并输出相应提示信息
static LogPtr _check_log(const char* name, const char* tag);         // 检查 log 是否存在, 并输出相应提示信息
static int _check_size_mb(size_t size_mb, const char* name, const char* tag);  // 检查 log 是否存在, 并输出相应提示信息


/* ----------------------------- logdict implementation ------------------------- */

/** MurmurHash2, by Austin Appleby
 * 注意 - 本 hash 算法假定用户的行为有以下约定
 * 1. 能够从任意地址读取 4字节 而不会崩溃
 * 2. sizeof(int) == 4
 *
 * 并且有如下限制:
 * 1. 它不会以增量模式工作
 * 2. 在低端字节序和高端字节序(网路字节序)的机器上不会产生相同的结果
 *
 * @param  key 需要进行hash的字符串
 * @param  len 需要进行hash的字符串长度
 * @return 返回 hash 后的结果
 */
unsigned int _dictGenHashFunction(const char *key, int len) {
    /* 'm' and 'r' are mixing constants generated offline.
     They're not really 'magic', they just happen to work well.  */
    uint32_t seed = 5381;
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;

    /* Mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)key;

    while(len >= 4) {
        uint32_t k = *(uint32_t*)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array  */
    switch(len) {
    case 3: h ^= data[2] << 16;
    case 2: h ^= data[1] << 8;
    case 1: h ^= data[0]; h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     * bytes are well-incorporated. */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return (unsigned int)h;
}

/** 对 hashtable 进行重置操作 [私有函数]
 * @param  dictht *ht 需要初始化的 hashtable
 * @return 返回 hash 后的结果
 */
static void _logdictReset(logdictht *ht)
{
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/** 创建一个新的字典
 * @param  type        字典操作函数类型（type是一个包含一组回调函数的结构）
 * @param  privDataPtr 设置为不同类型回调函数提供的 私有参数
 * @return 返回一个新的字典
 */
logdict* _logdictCreate(logdictType *type, void *privDataPtr)
{
    logdict *d = malloc(sizeof(*d));

    _logdictInit(d, type, privDataPtr);    // 初始化字典
    return d;
}

/** 对新的字典进行初始化操作 对字典中的 hashtable 进行重置操作，并对其他属性设置默认值 [私有函数]
 * @param  d           需要初始化操作的字典
 * @param  type        字典操作函数类型（type是一个包含一组回调函数的结构）
 * @param  privDataPtr 设置为不同类型回调函数提供的 私有参数
 * @return 返回初始化的状态
 */
int _logdictInit(logdict *d, logdictType *type, void *privDataPtr)
{
    _logdictReset(&d->ht[0]);          // 重置第一个 hash 表, 全置为 0
    _logdictReset(&d->ht[1]);          // 重置第二个 hash 表, 全置为 0
    d->type = type;                    // 字典类型
    d->privdata = privDataPtr;         // 私有参数
    d->rehashidx = -1;
    d->iterators = 0;
    return LOGDICT_OK;
}

/** 为字典创建新的 hashable，或对 hashtable 桶的个数进行扩展
 * 如果正在 rehash 或者 当前节点数 小于传递过来的节点数 则不进行rehash
 * @param  d     需要扩展的字典
 * @param  size  当前的节点数 最小为 DICT_HT_INITIAL_SIZE
 * @return 返回扩展结果的状态
 */
int _logdictExpand(logdict *d, unsigned long size)
{
    logdictht n; /* 临时 hash 表 */
    unsigned long realsize = _logdictNextPower(size);  // 返回不小于 size 的 为 2 的倍数的 值,即新大小

    /* 正在扩容中 或 size不合法(size < hash表元素个数), 返回 err */
    if (logdictIsRehashing(d) || d->ht[0].used > size)
        return LOGDICT_ERR;

    /* 扩容到相同的大小是无意义的, 返回 err */
    if (realsize == d->ht[0].size) return LOGDICT_ERR;

    /* 为新 hash表 分配初始值和空间(所有的dictEntry*均指向 null, 调用 calloc 即可) */
    n.size = realsize;
    n.sizemask = realsize-1;
    n.table = calloc(realsize*sizeof(logdictEntry*), 1);
    n.used = 0;

    /* 如果是第一次初始化(即创建新的字典时), 那么实际上并不是真正的 rehash 操作
     * 只需要把新的 hash表 置为 字典的第一个表, 然后返回即可*/
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return LOGDICT_OK;
    }

    /* 如果是扩容操作, 那么把新的 hash表 置为字典的第二个表, 用来执行增量的 rehash操作 */
    d->ht[1] = n;
    d->rehashidx = 0;   // 注意置为0, 表示现在是 rehash 状态
    return LOGDICT_OK;
}

/** 对字典的 n 个桶进行 rehash，计算每个桶内链表所有 key 并计算 hash 后， 存放在 ht[1] 中，并将 ht[0] 中处理过的桶指针指向空
 * @param  d    需要rehash的字典
 * @param  n    进行rehash的桶的的个数
 * @return 返回 0 表示rehash结束; 否则 表示还没有完全rehash结束
 */
int _logdictRehash(logdict *d, int n) {
    int empty_visits = n*10;            // 最多扫描空桶的数量
    if (!logdictIsRehashing(d)) return 0;

    /// 如果符合条件, 每次把旧表 n 个桶的数据移动到 新表中
    while(n-- && d->ht[0].used != 0) {
        logdictEntry *de, *nextde;

        /* 注意 rehashidx 并不会溢出, 因为 ht[0].used != 0,
         * 所以如果要扫描的数量大于剩余待扫描的桶的数量, 那么在溢出之前, 一定会找到不为空的桶 */
        // assert(d->ht[0].size > (unsigned long)d->rehashidx);

        /// 从 ht[0].table[rehashidx] 开始, 扫描到第一个不为空的桶, 扫描数量不大于 empty_visits
        /// 如果扫描数量大于 empty_visits, 仍没找到, 则返回 1, 表示 rehash 未结束
        // 第一次 rehashidx = 0, 后每次都会更新为上次处理的桶的下一位置
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;     // 扫描的个数在这里
            if (--empty_visits == 0) return 1;
        }
        de = d->ht[0].table[d->rehashidx];  // de 指向 ht[0].table 中第一个不为空的桶所指元素

        /// 把当前旧桶中的 所有 key 移动到新的 HT 中
        while(de) {
            unsigned int h;

            nextde = de->next;              // 记录 当前元素的后一个元素
            // 调用当前字典的 hash 函数获得 hash 值, 并与掩码求与, 得到在新表中的位置,
            // 注意, 每次得到的位置不一定相同, 所以不能个进行整链直接交换
            h = logdictHashKey(d, de->key) & d->ht[1].sizemask;
            de->next = d->ht[1].table[h];   // 把 de 后一个元素置为新表相应位置所指的第一个元素
            d->ht[1].table[h] = de;         // 把 de 所指元素插入到新表相应位置所指第一个元素之前, 头插法移动元素
            d->ht[0].used--;            // 旧表元素减 1
            d->ht[1].used++;            // 新表元素加 1
            de = nextde;                // 后移
        }
        d->ht[0].table[d->rehashidx] = NULL;    // 当前桶置为 空
        d->rehashidx++;                         // 移动到下一个桶
    }

    /* 检查是否已经完全对旧表进行了 rehash 操作 */
    if (d->ht[0].used == 0) {   // 如果是
        free(d->ht[0].table);       // 释放第一个 hash 表
        d->ht[0] = d->ht[1];        // 第一个hash表结构 得到 第二个hash表结构 的数据
        _logdictReset(&d->ht[1]);   // 初始化维护第二个 hash 表的数据结构
        d->rehashidx = -1;          // 置为 未rehashidx 状态
        return 0;                   // 返回 0, 表示完成了 rehash 操作
    }

    /* 旧表中还有数据, 未完成 rehash 操作, 返回 1 */
    return 1;
}

/**
 * 如果没有进行迭代操作，则进行一次 rehash 操作（渐进式rehash，将 rehash 操作分布到每一个操作上）
 * 若有字典有 安全迭代器, 则不会进行 rehash 操作
 * @param  d    进行rehash的字典
 * @return  void
 */
static void _logdictRehashStep(logdict *d) {
    if (d->iterators == 0) _logdictRehash(d,1);
}

///**
// * 向 hashtable 中加入一个新的节点
// * @param  d    加入节点的字典
// * @param  key  节点的 key
// * @param  val  节点的 value
// * @return 成功或失败
// */
//int _logdictAdd(logdict *d, const char *key, void *val)
//{
//    logdictEntry *entry = _logdictAddRaw(d, key);
//    if (!entry) return LOGDICT_ERR;    // 若 key 已存在, 返回 err

//    logdictSetVal(d, entry, val);
//    return LOGDICT_OK;
//}

/**
 * 创建一个包含 KEY 的 dictEntry 对象, 添加到 dict 中, 然后返回该对象的指针, 供上层使用(设置值)
 * 如:
 * entry = dictAddRaw(dict,mykey);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 * 如果正在进行 Rehash，会执行一个桶的 Rehash 操作（在检查key的时候会判断是否触发rehash）
 * @param  d   加入节点的字典
 * @param  key 节点的key
 * @return 若 key 存在, 返回 NULL
 *         若 key 成功添加, 返回包含 key 的 dictEntry
 */
logdictEntry *_logdictAddRaw(logdict *d, const char *key)
{
    int index;
    logdictEntry *entry;
    logdictht *ht;

    if (logdictIsRehashing(d)) _logdictRehashStep(d); // 若在进行 rehash, 执行一个桶的 rehash 操作

    /* 获取新元素的 index, 若已存在 返回 -1 */
    if ((index = _logdictKeyIndex(d, key)) == -1)
        return NULL;

    /* 分配内存 并 存储新 entry */
    ht = logdictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    entry = malloc(sizeof(*entry));
    entry->next = ht->table[index];
    ht->table[index] = entry;
    ht->used++;

    /* SetKey */
    logdictSetKey(d, entry, key);
    return entry;
}

/**
 * 查找并在 hashtable 删除某个 key 所对应的结构
 * @param  d    加入节点的字典
 * @param  key  要删除的节点的key
 * @param  nofree 如果为1, 则删除的某个key的结构不进行内存释放
 * @return 成功或失败
 */
static int _logdictGenericDelete(logdict *d, const void *key, int nofree)
{
    unsigned int h, idx;
    logdictEntry *he, *prevHe;
    int table;

    if (d->ht[0].size == 0) return LOGDICT_ERR; /* d->ht[0].table is NULL */
    if (logdictIsRehashing(d)) _logdictRehashStep(d);
    h = logdictHashKey(d, key);

    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        prevHe = NULL;
        while(he) {
            if (logdictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;
                if (!nofree) {
                    logdictFreeKey(d, he);
                    logdictFreeVal(d, he);
                }
                free(he);
                d->ht[table].used--;
                return LOGDICT_OK;
            }
            prevHe = he;
            he = he->next;
        }
        if (!logdictIsRehashing(d)) break;
    }
    return LOGDICT_ERR; /* not found */
}

/**
 * 查找并在hashtable删除某个key所对应的结构，释放内存
 * @param  d 加入节点的字典
 * @param  key 要删除的节点的key
 * return  成功或失败
 */
int _logdictDelete(logdict *ht, const char *key) {
    return _logdictGenericDelete(ht,key,0);
}

/** 将字典中的某个 ht 中所有节点全部删除
 * @param  d    要进行删除操作的的字典
 * @param  ht   dictht[0] 或 dictht[1]
 * @param  callback 回调函数，void *参数为 dict结构中的pridata
 * return  成功或失败
 */
int _logdictClear(logdict *d, logdictht *ht, void(callback)(void *)) {
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        logdictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d->privdata);

        if ((he = ht->table[i]) == NULL) continue;
        while(he) {
            nextHe = he->next;
            logdictFreeKey(d, he);
            logdictFreeVal(d, he);
            free(he);
            ht->used--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    free(ht->table);
    /* Re-initialize the table */
    _logdictReset(ht);
    return LOGDICT_OK; /* never fails */
}

/**
 * 将字典所有节点清空，并释放字典
 * @param  d    要删除的字典
 * @return 成功或失败
 */
void _logdictRelease(logdict *d)
{
    if(!d) return;
    _logdictClear(d,&d->ht[0],NULL);
    _logdictClear(d,&d->ht[1],NULL);
    free(d);
}

/** 在字典中查找 key 并返回找到的 entry（会遍历 ht[0]和ht[1]）
 * @param  d 被查找的字典
 * @param  key 要查找的key
 * return  找到的 entry, 若找不到返回null
 */
logdictEntry* _logdictFind(logdict *d, const char *key)
{
    logdictEntry *he;
    unsigned int h, idx, table;

    if (d->ht[0].size == 0) return NULL; /* We don't have a table at all */
    if (logdictIsRehashing(d)) _logdictRehashStep(d);
    h = logdictHashKey(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while(he) {
            if (logdictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        if (!logdictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/** 查找并返回指定 key 对应的 val
 * @param  d 被查找的字典
 * @param  key 要查找的 key
 * return  返回 相应 key 对应的 val
 */
LogPtr _logdictFetchValue(logdict *d, const char *key) {
    logdictEntry *he;

    he = _logdictFind(d,key);
    return he ? logdictGetVal(he) : NULL;
}

/* ------------------------------- logsys API ------------------------------------*/
static LogPtr       _sys_log         = DF_SYS_LOG;          // 系统日志结构指针
static bool         _logsys_service  = DF_LOGSYS_SERVICE;   // 系统日志初始化状态, 只有为 true , 以下 SET API 才有效
static bool         _logsys_mutetype = DF_LOGSYS_MUTETYPE;  // 系统日志静默属性, 默认静默
static size_t       _logsys_filesize = DF_LOGSYS_FILESIZE;  // 系统日志大小, 默认为 1 M
static logdict*     _logsys_dic      = DF_LOGSYS_DIC;       // 日志系统维护的日志结构字典
static logdictType* _logsys_dictype  = DF_LOGSYS_DICTYPE;   // 日志字典类型


unsigned int _loghashFunction (const void *key)
{
     return _dictGenHashFunction(key, strlen(key));
}

void _valDestructor( void *obj)
{
    _logReset(obj);
    free(obj);
}

/**
 * @brief logsysInit - 初始化内部日志系统, 可执行可不执行
 * 若执行, 所有的日志创建销毁操作都会记录到默认的日志文件中, 通过 LOGSYS_PATH 查看具体文件
 * 若不执行, 则不记录相关操作
 */
int logsysInit()
{
    if(_logsys_service)  return LOG_OK;

    /* 创建系统日志结构并初始化 */
    if(!_sys_log)
    {
        _sys_log = _logGenerate(NULL, LOGSYS_PATH, _logsys_mutetype);
        if(!_sys_log)
        {   logShow("log system init err!\n");   return LOG_ERR;  }
    }
    _sys_log->maxsize = _logsys_filesize << 20;      // 设置内部日志文件最大限制, 默认为 1 MB
    _logsys_service = true;

    logsysAddText(NULL, "\n");
    logsysAdd(NULL, "[============== log system initialing =================]\n");
    logsysAdd(NULL, "--Creating dict for logs...");

    /* 创建 log字典类型 */
    if(!_logsys_dictype)
    {
        _logsys_dictype = calloc(sizeof(*_logsys_dictype), 1);
        _logsys_dictype->hashFunction = _loghashFunction;
        _logsys_dictype->valDestructor = _valDestructor;
    }

    /* 创建 log字典 */
    if(!_logsys_dic)
    {
        _logsys_dic = _logdictCreate(_logsys_dictype, NULL);
        if(!_logsys_dic)
        {
           logsysAddText(NULL, " ERR\n");
           logsysAdd(NULL, "[-------------- log system initial ERR ----------------]\n\n");
           return LOG_ERR;
        }
    }
    logsysAddText(NULL, " ok\n");
    logsysAdd(NULL, "[-------------- log system initial ok -----------------]\n");
    return LOG_OK;
}

/**
 * @brief logsysStop - 停止内部日志系统, 注意并不会销毁日志 dict
 */
void logsysStop()
{
    if(!_logsys_service)  return;

    logsysAdd(NULL, "[______________ log system stoped! ____________________]\n\n");
    _logsys_service = false;
    _logReset(_sys_log);
    free(_sys_log);
    _sys_log = NULL;
}

/**
 * @brief logsysRelease -  停止服务, 并释放所有资源
 */
void logsysRelease()
{
    logsysStop();

    if(_logsys_dic)
    {
        _logdictRelease(_logsys_dic);   // _logdictRelease 最后会释放 _logsys_dic 本身, 不需要进一步 free
        _logsys_dic = NULL;
        free(_logsys_dictype);
        _logsys_dictype = NULL;
    }
}

/**
 * @brief logsysSetMutetype - 设置系统日志的静默属性, 程序运行期间一直有效
 * @param mutetype
 */
void logsysSetMutetype(bool mutetype)
{
    _logsys_mutetype = mutetype;
    if(!_logsys_service)    return;

    _sys_log->mutetype = _logsys_mutetype;
    if(MUTE == mutetype)
        logsysAdd(NULL, "--Set logsys mutetype to [MUTE]\n");
    else
        logsysAdd(NULL, "--Set logsys mutetype to [NMUTE]\n");
}

/**
 * @brief logsysSetFileSize - 设置系统日志的文件大小, 程序运行期间一直有效
 * @param size_mb
 * @return
 */
int logsysSetFileSize(size_t size_mb)
{
    if(size_mb > INT_MAX>>20)   return -1;
    _logsys_filesize = size_mb<<20;

    if(_logsys_service)
    {
        _sys_log->maxsize = _logsys_filesize;
        logsysAdd(NULL, "--Set logsys filesize to [%d]\n", _logsys_filesize);
    }

    return _logsys_filesize;
}

/**
 * @brief logsysFlieEmpty - 清空日志系统文件
 * @return
 */
int  logsysFlieEmpty()
{
    if(!_logsys_service)    {
        logsysShow("--Empty logsys file... err: logsys serve is off\n");
        return LOG_ERR;
    }

    if(0 == _logFlieEmpty(_sys_log)){
        logsysAdd(NULL, "--Empty logsys file... ok: Log file had been truncated \n");
        return LOG_OK;
    }
    else{
        logsysAdd(NULL, "--Empty logsys file.... err: %s \n", strerror(errno));
        logsysShow("--Empty logsys file... err: %s \n", strerror(errno));
        return LOG_ERR;
    }
}

/**
 * @brief logsysShowTime - logsys 的纯输出函数, 输出时间
 * @return LOG_ERR
 * @note    只有在日志系统服务开启并且在静默模式下, 才会输出
 *  后面两个函数功能是一样的, 只是输出的内容不同
 *  这三个函数的作用旨在静默模式下输出错误信息到控制台, 而非静默模式不输出, 从而能够防止输出混乱
 *  因为这三个函数适合输出错误信息, 所以特意返回 LOG_ERR
 */
int logsysShowTime()
{
    // 如果服务未开启, logsys*不会有输出, 所以强制输出
    if(!_logsys_service){
        fprintf(stderr, "%s", _timeStr(TS_LOG));
        return LOG_ERR;
    }
    // 服务开启, 且是 静默模式, logsys*不会有输出, 所以输出
    if(_logsys_mutetype)
        fprintf(stderr, "%s", _timeStr(TS_LOG));

    return LOG_ERR;
}
int logsysShowText(const char* text, ...)
{
    if(!text || !(*text)) return LOG_ERR;

    va_list argptr;
    va_start(argptr, text);

    // 如果服务未开启, logsys*不会有输出, 所以强制输出
    if(!_logsys_service){
        vfprintf(stderr, text, argptr);
        return LOG_ERR;
    }
    // 服务开启, 且是 静默模式, logsys*不会有输出, 所以输出
    if(_logsys_mutetype)
        vfprintf(stderr, text, argptr);

    va_end(argptr);
    return LOG_ERR;
}
int logsysShow(const char* text, ...)
{
    if(!text || !(*text)) return LOG_ERR;

    va_list argptr;
    va_start(argptr, text);

    // 如果服务未开启, logsys*不会有输出, 所以强制输出
    if(!_logsys_service){
        fprintf(stderr, "%s", _timeStr(TS_LOG));
        vfprintf(stderr, text, argptr);
        return LOG_ERR;
    }
    // 服务开启, 且是 静默模式, logsys*不会有输出, 所以输出
    if(_logsys_mutetype){
        fprintf(stderr, "%s", _timeStr(TS_LOG));
        vfprintf(stderr, text, argptr);
    }

    va_end(argptr);
    return LOG_ERR;
}

/**
 * @brief logsysAddText - 添加 Text 到系统日志
 * @param log   现在操作的日志, 主要是为了得到 log->name, 用以区分, 可以为 NULL
 * @param text  内容
 */
void logsysAddText(LogPtr log, const char* text, ...)
{
    if(!_logsys_service || !_sys_log || !text || !(*text))  return;

    _logFileShrink(_sys_log);

    va_list argptr;
    va_start(argptr, text);

    /* 写入日志到 系统日志 中 */
    if(log && log->name)
        fprintf(_sys_log->fp, "[%s] ", log->name);
    vfprintf(_sys_log->fp, text, argptr);
    fflush(_sys_log->fp);

    /* 输出日志到 控制台 中 */
    if(!_sys_log->mutetype)
    {
        va_start(argptr, text);
        if(log && log->name)
        {
            fprintf(stderr, "[%s] ", log->name);
        }
        vfprintf(stderr, text, argptr);
    }

    va_end(argptr);
}
void logsysAddTextMute(LogPtr log, const char* text, ...)
{
    if(!_logsys_service || !_sys_log || !text || !(*text))  return;

    _logFileShrink(_sys_log);

    va_list argptr;
    va_start(argptr, text);

    /* 写入日志到 系统日志 中 */
    if(log && log->name)
    {
        fprintf(_sys_log->fp, "[%s] ", log->name);
    }
    vfprintf(_sys_log->fp, text, argptr);
    fflush(_sys_log->fp);

    va_end(argptr);
}
void logsysAddTextNMute(LogPtr log, const char* text, ...)
{
    if(!_logsys_service || !_sys_log || !text || !(*text))  return;

    _logFileShrink(_sys_log);

    va_list argptr;
    va_start(argptr, text);

    /* 写入日志到 系统日志 中 */
    if(log && log->name)
        fprintf(_sys_log->fp, "[%s] ", log->name);
    vfprintf(_sys_log->fp, text, argptr);
    fflush(_sys_log->fp);

    /* 输出日志到 控制台 中 */
    va_start(argptr, text);
    if(log && log->name)
        fprintf(stderr, "[%s] ", log->name);
    vfprintf(stderr, text, argptr);

    va_end(argptr);
}

/**
 * @brief logsysAdd - 添加日志到系统日志
 * @param log   现在操作的日志, 主要是为了得到 log->name, 用以区分, 可以为 NULL
 * @param text  内容
 */
void logsysAdd(LogPtr log, const char* text, ...)
{
    if(!_logsys_service || !_sys_log || !text || !(*text))  return;

    _logFileShrink(_sys_log);

    va_list argptr;
    va_start(argptr, text);

    /* 写入日志到 系统日志 中 */
    fprintf(_sys_log->fp, "%s", _timeStr(TS_LOG));
    if(log && log->name)
        fprintf(_sys_log->fp, "[%s] ", log->name);
    vfprintf(_sys_log->fp, text, argptr);
    fflush(_sys_log->fp);

    /* 输出日志到 控制台 中 */
    if(!_sys_log->mutetype)
    {
        va_start(argptr, text);
        fprintf(stderr, "%s", _timeStr(TS_LOG));
        if(log && log->name)
            fprintf(stderr, "[%s] ", log->name);
        vfprintf(stderr, text, argptr);
    }

    va_end(argptr);
}

/**
 * @brief logsysAddMute - 添加日志到系统日志, 静默模式
 * @param log   现在操作的日志, 主要是为了得到 log->name, 用以区分, 可以为 NULL
 * @param text  内容
 */
void logsysAddMute(LogPtr log, const char* text, ...)
{
    if(!_logsys_service || !_sys_log || !text || !(*text))  return;

    _logFileShrink(_sys_log);

    va_list argptr;
    va_start(argptr, text);

    /* 写入日志到 系统日志 中 */
    fprintf(_sys_log->fp, "%s", _timeStr(TS_LOG));
    if(log && log->name)
    {
        fprintf(_sys_log->fp, "[%s] ", log->name);
    }
    vfprintf(_sys_log->fp, text, argptr);
    fflush(_sys_log->fp);

    va_end(argptr);
}
/**
 * @brief logsysAddNMute - 添加日志到系统日志, 非静默模式
 * @param log   现在操作的日志, 主要是为了得到 log->name, 用以区分, 可以为 NULL
 * @param text  内容
 */
void logsysAddNMute(LogPtr log, const char* text, ...)
{
    if(!_logsys_service || !_sys_log || !text || !(*text))  return;

    _logFileShrink(_sys_log);

    va_list argptr;
    va_start(argptr, text);

    /* 写入日志到 系统日志 中 */
    fprintf(_sys_log->fp, "%s", _timeStr(TS_LOG));
    if(log && log->name)
    {
        fprintf(_sys_log->fp, "[%s] ", log->name);
    }
    vfprintf(_sys_log->fp, text, argptr);
    fflush(_sys_log->fp);

    /* 输出日志到 控制台 中 */
    va_start(argptr, text);
    fprintf(stderr, "%s", _timeStr(TS_LOG));
    if(log && log->name)
    {
        fprintf(stderr, "[%s] ", log->name);
    }
    vfprintf(stderr, text, argptr);

    va_end(argptr);
}


/* ----------------------------- API implementation ------------------------- */

/**
 * @brief logShowTime - 输出时间信息到控制台 "[%02d-%02d-%02d %02d:%02d:%02d] "
 */
void logShowTime()
{
    fprintf(stderr, "%s", _timeStr(TS_LOG));
}
/**
 * @brief logShowText - 显示 text 到控制台
 * @param text
 */
void logShowText(const char* text, ...)
{
    if(!text || !(*text)) return;

    va_list argptr;
    va_start(argptr, text);

    vfprintf(stderr, text, argptr);

    va_end(argptr);
}
/**
 * @brief logShow - 输出 时间 和 text 到控制台
 * @param text
 */
void logShow(const char* text, ...)     // 打印时间和内容到控制台
{
    if(!text || !(*text)) return;

    va_list argptr;
    va_start(argptr, text);

    logShowTime();
    vfprintf(stderr, text, argptr);

    va_end(argptr);
}

void _logReset(LogPtr log)
{
    if(log->name)   free(log->name);
    if(log->path)   free(log->path);
    if(log->fp)     fclose(log->fp);
    bzero(log, sizeof(*log));
}

/**
 * @brief _logInit - 根据传入参数初始化 log 结构体
 * @param log       要初始化的log结构体指针
 * @param name      名称
 * @param path      路径
 * @param mutetype  静默模式
 * @return 成功返回 LOG_OK; 失败返回 LOG_ERR
 * @note   如果返回 LOG_OK, 说明 路径一定合法, 并且已成功打开
 */
static int _logInit(LogPtr log, const char* name, const char* path, bool mutetype)
{
    if(name && *name) log->name = strdup(name);
    if(path && *path)
    {
        _mkdir(path, 0755);
        log->path     = strdup(path);
        log->fp       = fopen(log->path, "a+");
        log->maxsize  = DF_LOG_SIZE << 20;          // 默认日志文件大小 DF_LOG_SIZE MB
        log->mutetype = mutetype;
    }
    if(!path || !log->fp)
    {
        logsysAddNMute(log, "%s(%d)-%s:%s\n", __FILE__, __LINE__, __FUNCTION__ ,strerror(errno));
        return LOG_ERR;
    }

    return LOG_OK;
}

/**
 * @brief _logGenerate - 创建一个日志结构
 * @param name      日志名称, 若输出到控制台, 则通过名称进行区分
 * @param path      文件路径, 日志信息会存到此处; 若不可读写, 会在 DF_LOG_DIR 下创建一个临时文件
 * @param mutetype  所创建日志的静默属性
 * @return 创建的日志结构指针, 若失败, 则返回 NULL
 * @note   只要返回值不为 NULL, 那么 path 和 fp 肯定不是 NULL
 */
LogPtr _logGenerate(const char* name, const char* path, bool mutetype)
{
    LogPtr r_log = (LogPtr)calloc(sizeof(*r_log), 1);
    status f_s = _GetFileStatus(path);
    logsysAdd(NULL, "[%s] Generating log struct start\n", name);
    switch(f_s)
    {
        case FILE_CANWRITE:
        case FILE_NOTEXIST:
            /* 使用指定的文件路径初始化, 如果成功, 跳出返回  */
            if(LOG_OK == _logInit(r_log, name, path, mutetype))
                break;
            /* 如果失败, 重置log, 继续执行 FILE_NOTWRITE 分支 */
            _logReset(r_log);
        case FILE_NOTWRITE:
            /* 在程序目录下产生临时文件进行初始化, 如果失败, 那么销毁 log, 返回 null  */
            logsysAddNMute(NULL, "[%s] Generating log struct err: file \"%s\" cannot write -> try to create a temp file...\n", name, path);
            if(LOG_ERR == _logInit(r_log, name, _logPath(DF_LOG_DIR, name), mutetype))
            {
                logsysAddNMute(NULL, "[%s] Generating log struct err: cannot create temp file \"%s\"]", name, r_log->path);
                _logReset(r_log);
                free(r_log);
                return r_log = NULL;
            }
            logsysAddNMute(r_log, "Create temp file \"%s\"\n", r_log->path);
    }
    logsysAdd(r_log, "Generating log struct ok\n");
    return r_log;
}

int logCreate(const char* name, const char* path, bool mutetype)
{
    logsysAdd(NULL, "[%s] --CreateLog... \n", name);
    if(LOG_ERR == _check_logsys(name, "--Creating"))    return LOG_ERR;
    if(LOG_ERR == _check_name(name, "--Creating"))      return LOG_ERR;
    if(LOG_ERR == _check_path(name, "--Creating"))      return LOG_ERR;

    /* key 已存在, 返回 err */
    logdictEntry *entry = _logdictAddRaw(_logsys_dic, name);
    if (!entry)    {
        logsysAdd(NULL, "[%s] --Creating... err: \"%s\" has already exist \n", name, name);
        return logsysShow("[%s] --Creating... err: \"%s\" has already exist \n", name, name);
    }

    /* 获取日志结构失败, 返回 err */
    LogPtr log = _logGenerate(name, path, mutetype);
    if(!log)    {
        // 运行到这里, 说明 name 已经插入到字典中, 所以需要先设置值为 NULL, 再从字典中删除
        logdictSetVal(_logsys_dic, entry, NULL);    // 这一步时必要的, _logdictAddRaw 内部使用 malloc, 不置 NULL 可能引起段错误
        _logdictDelete(_logsys_dic, name);
        logsysAdd(NULL, "[%s] --Creating... err: Generating log struct failed \n", name, name);
        return logsysShow("[%s] --Creating... err: Generating log struct failed \n", name, name);
    }

    /* 设置值 */
    logdictSetVal(_logsys_dic, entry, log);
    logsysAdd(log, "--CreateLog... ok: link file \"%s\" \n", log->path);
    logAddTextMute(log->name, "\n");

    return LOG_OK;
}

/**
 * @brief logDestroy - 销毁一个日志结构, 释放内存
 * @param log   要销毁的日志结构指针
 */
int logDestroy(const char* name)
{
    if(LOG_ERR == _check_logsys(name, "--DestroyLog"))    return LOG_ERR;
    if(LOG_ERR == _check_name(name, "--DestroyLog"))      return LOG_ERR;

    /* 未找到指定的 log, 返回 err */
    if(LOGDICT_ERR == _logdictDelete(_logsys_dic, name)){
        logsysAdd(NULL, "[%s] --DestroyLog... err: log not exist \n", name);
        return logsysShow("[%s] --DestroyLog... err: log not exist \n", name);
    }

    logsysAdd(NULL, "[%s] --DestroyLog... ok: log destroied\n", name);
    return LOG_OK;
}

/**
 * @brief logFileSize - 获取日志文件大小
 * @param name
 * @return 大小, 单位为字节
 */
size_t logFileSize(const char* name)
{
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "--GetFileSize")) return LOG_ERR;
    if(LOG_ERR == _check_name(name, "--GetFileSize")) return LOG_ERR;
    if(!(log = _check_log(name, "--GetFileSize"))) return LOG_ERR;

    return _logFileSize(log);
}

/**
 * @brief logSetFileSize - 设置日志文件大小限制
 * @param log
 * @param size_mb   大小, 单位为 MB
 * @return 失败返回 -1, 成功返回设置后的大小
 */
int logSetFileSize(const char* name, size_t size_mb)
{
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "--SetFileSize")) return LOG_ERR;
    if(LOG_ERR == _check_name(name, "--SetFileSize")) return LOG_ERR;
    if(LOG_ERR == _check_size_mb(size_mb, name, "SetFileSize")) return LOG_ERR;
    if(!(log = _check_log(name, "--SetFileSize"))) return LOG_ERR;

    log->maxsize = size_mb << 20;
    logsysAdd(log, "--SetFileSize... ok: set file max size to %d \n", log->maxsize);
    return log->maxsize;
}

/**
 * @brief logSetMutetype - 设置日志的静默属性
 * @param name
 * @param mutetype
 */
void logSetMutetype(const char* name, bool mutetype)
{
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "--SetMutetype")) return;
    if(LOG_ERR == _check_name(name, "--SetMutetype")) return;
    if(!(log = _check_log(name, "--SetMutetype"))) return;

    log->mutetype = mutetype;
    if(mutetype)
        logsysAdd(log, "--SetMutetype... ok: set mutetype to MUTE \n");
    else
        logsysAdd(log, "--SetMutetype... ok: set mutetype to NMUTE \n");
}

/**
 * @brief logFlieEmpty  - 清空日志结构所指文件
 * @param name
 * @return
 */
int logFlieEmpty(const char* name)
{
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "--EmptyFile")) return LOG_ERR;
    if(LOG_ERR == _check_name(name, "--EmptyFile")) return LOG_ERR;
    if(!(log = _check_log(name, "--EmptyFile"))) return LOG_ERR;

    if(0 == _logFlieEmpty(log)){
        logsysAdd(log, "--EmptyFile... ok: Log file had been truncated \n");
        return LOG_OK;
    }
    else{
        logsysAdd(log, "--EmptyFile... err: %s \n", strerror(errno));
        logsysShow("--EmptyFile... err: %s \n", strerror(errno));
        return LOG_ERR;
    }
}

/**
 * @brief logAddTime - 添加当前时间到 日志 中, 由 (*log).mute 决定是否静默处理
 * @param name
 */
void logAddTime(const char* name)
{
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "AddTimeStr")) return;
    if(LOG_ERR == _check_name(name, "AddTimeStr")) return;
    if(!(log = _check_log(name, "AddTimeStr"))) return;

    _logFileShrink(log);
    fprintf(log->fp, "%s", _timeStr(TS_LOG));
    if(!log->mutetype)
        fprintf(stderr, "%s", _timeStr(TS_LOG));
    logsysAdd(log, "add time\n");
}

/**
 * @brief logAddTimeMute - 添加当前时间到 日志 中, 强制静默处理
 * @param name
 */
void logAddTimeMute(const char* name)
{
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAddTimeMute")) return;
    if(LOG_ERR == _check_name(name, "logAddTimeMute")) return;
    if(!(log = _check_log(name, "logAddTimeMute"))) return;

    _logFileShrink(log);
    fprintf(log->fp, "%s", _timeStr(TS_LOG));
    logsysAdd(log, "add timestr in mute mode\n");
}

/**
 * @brief logAddTimeNMute - 添加当前时间到 日志 中, 强制非静默处理
 * @param name
 */
void logAddTimeNMute(const char* name)
{
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAddTimeNMute")) return;
    if(LOG_ERR == _check_name(name, "logAddTimeNMute")) return;
    if(!(log = _check_log(name, "logAddTimeNMute"))) return;

    _logFileShrink(log);
    fprintf(log->fp, "%s", _timeStr(TS_LOG));
    fprintf(stderr, "%s", _timeStr(TS_LOG));
    logsysAdd(log, "add timestr in nmute mode\n");
}

/**
 * @brief logAddText - 添加 text 到 日志 中, 由 (*log).mute 决定是否静默处理
 * @param name
 * @param text  内容
 */
void logAddText(const char* name, const char* text, ...)
{
    if(!text || !(*text)) return;
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAddText")) return;
    if(LOG_ERR == _check_name(name, "logAddText")) return;
    if(!(log = _check_log(name, "logAddText"))) return;

    _logFileShrink(log);

    va_list argptr;
    va_start(argptr, text);

    vfprintf(log->fp, text, argptr);
    if(!log->mutetype)
    {
        va_start(argptr, text);
        vfprintf(stderr, text, argptr);
    }
    logsysAdd(log, "add a text \n");

    va_end(argptr);
}
/**
 * @brief logAddTextMute - 添加 text 到 日志 中, 强制静默处理
 * @param name
 * @param text
 */
void logAddTextMute(const char* name, const char* text, ...)
{
    if(!text || !(*text)) return;
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAddTextMute")) return;
    if(LOG_ERR == _check_name(name, "logAddTextMute")) return;
    if(!(log = _check_log(name, "logAddTextMute"))) return;

    _logFileShrink(log);

    va_list argptr;
    va_start(argptr, text);

    vfprintf(log->fp, text, argptr);
    logsysAdd(log, "add a text in mute mode\n");

    va_end(argptr);
}
/**
 * @brief logAddTextNMute - 添加 text 到 日志 中, 强制非静默处理
 * @param name
 * @param text
 */
void logAddTextNMute(const char* name, const char* text, ...)
{
    if(!text || !(*text)) return;
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAddTextNMute")) return;
    if(LOG_ERR == _check_name(name, "logAddTextNMute")) return;
    if(!(log = _check_log(name, "logAddTextNMute"))) return;

    _logFileShrink(log);

    va_list argptr;

    va_start(argptr, text);
    vfprintf(log->fp, text, argptr);

    va_start(argptr, text);
    vfprintf(stderr, text, argptr);

    logsysAdd(log, "add a text in nmute mode \n");

    va_end(argptr);
}


/**
 * @brief logAdd - 添加 时间 和 text 到 日志中, 由 (*log).mute 决定是否静默处理
 * @param name
 * @param text
 */
void logAdd(const char* name, const char* text, ...)
{
    if(!text || !(*text))   return;
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAdd")) return;
    if(LOG_ERR == _check_name(name, "logAdd")) return;
    if(!(log = _check_log(name, "logAdd"))) return;

    _logFileShrink(log);

    va_list argptr;

    va_start(argptr, text);
    fprintf(log->fp, "%s", _timeStr(TS_LOG));
    vfprintf(log->fp, text, argptr);

    if(!log->mutetype)
    {
        va_start(argptr, text);
        fprintf(stderr, "%s", _timeStr(TS_LOG));
        fprintf(stderr, "[%s] :", log->name);
        vfprintf(stderr, text, argptr);
    }
    logsysAdd(log, "add a log\n");

    va_end(argptr);
}
void logAddMute(const char* name, const char* text, ...)     // 添加 时间 和 text 到 日志中, 强制静默处理
{
    if(!text || !(*text))   return;
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAddMute")) return;
    if(LOG_ERR == _check_name(name, "logAddMute")) return;
    if(!(log = _check_log(name, "logAddMute"))) return;

    _logFileShrink(log);

    va_list argptr;
    va_start(argptr, text);

    fprintf(log->fp, "%s", _timeStr(TS_LOG));
    vfprintf(log->fp, text, argptr);

    logsysAdd(log, "add a log in mute mode \n");

    va_end(argptr);
}
/**
 * @brief logAddNMute - 添加 时间 和 text 到 日志中, 强制非静默处理
 * @param name
 * @param text
 */
void logAddNMute(const char* name, const char* text, ...)
{
    if(!text || !(*text))   return;
    LogPtr log;
    /* 检测未通过, 返回 err */
    if(LOG_ERR == _check_logsys(name, "logAddNMute")) return;
    if(LOG_ERR == _check_name(name, "logAddNMute")) return;
    if(!(log = _check_log(name, "logAddNMute"))) return;

    _logFileShrink(log);

    va_list argptr;

    // 添加到文件流中
    va_start(argptr, text);
    fprintf(log->fp, "%s", _timeStr(TS_LOG));
    vfprintf(log->fp, text, argptr);

    // 输出到控制台
    va_start(argptr, text);
    fprintf(stderr, "%s", _timeStr(TS_LOG));
    fprintf(stderr, "[%s] :", log->name);
    vfprintf(stderr, text, argptr);

    logsysAdd(log, "add a log in nmute mode\n");

    va_end(argptr);
}

/* ------------------- private functions for logdict ------------------------ */
/**
 * 根据当前节点数量，计算hashtable扩展桶的数量，最大扩展桶的数量为 LONG_MAX 最小为 DICT_HT_INITIAL_SIZE，设置桶的个数为2的N次方大于节点数的最小值
 * unsigned long size 当前节点数
 * return 返回新计算的桶的大小
 */
static unsigned long _logdictNextPower(unsigned long size)
{
    unsigned long i = DICT_HT_INITIAL_SIZE;

    if (size >= LONG_MAX) return LONG_MAX;
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/**
 * 查找指定key存在于哪个桶里
 * dict *d 需要判断的字典
 * void *key 需要查找的key
 * return 返回 桶的索引
 */
int _logdictKeyIndex(logdict *d, const void *key)
{
    unsigned int h, idx, table;
    logdictEntry *he;

    /* Expand the hash table if needed */
    if (_logdictExpandIfNeeded(d) == LOGDICT_ERR)
        return -1;
    /* Compute the key hash value */
    h = logdictHashKey(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        /* Search if this slot does not already contain the given key */
        he = d->ht[table].table[idx];
        while(he) {
            if (logdictCompareKeys(d, key, he->key))
                return -1;
            he = he->next;
        }
        if (!logdictIsRehashing(d)) break;
    }
    return idx;
}

/**
 * 判断字典是否需要进行扩展操作
 * dict *d 需要判断的字典
 * return 返回状态
 */
int _logdictExpandIfNeeded(logdict *d)
{
    /* Incremental rehashing already in progress. Return. */
    if (logdictIsRehashing(d)) return LOGDICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    if (d->ht[0].size == 0) return _logdictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))
    {
        return _logdictExpand(d, d->ht[0].used*2);
    }
    return LOGDICT_OK;
}

/* ------------------------- private functions ------------------------------ */

static status _GetFileStatus(const char* path)
{
    if (access(path, F_OK) == 0)       // 如果文件存在
    {
        if (access(path, W_OK) != 0)    // 如果不可写
        {
            return FILE_NOTWRITE;
        }
        return FILE_CANWRITE;
    }
    // 文件不存在
    return FILE_NOTEXIST;
}

/**
 * 返回一个存储当前本地时间的静态字符串指针
 * @param  type  根据 type 返回不同形式的字串
 * @return char* 指向静态区的字符串指针
 * 注意, 不可 free
*/
char* _timeStr(int type)
{
    // static char* timestr = (char*)calloc(30, 1); // C 不支持
    static char timestr[30];    // 因为不大, 所以直接放到 栈 中
    static struct tm *local;
    static time_t t;

    memset(timestr, 0, 30);
    t = time(NULL);         // 获取日历时间
    local = localtime(&t);  // 将日历时间转化为本地时间，并保存在 struct tm 结构中
    switch(type)
    {
        case TS_LOG:
            sprintf(timestr, "[%02d-%02d-%02d %02d:%02d:%02d] ",local->tm_year+1900, local->tm_mon, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
            break;
        case TS_FILE:
            sprintf(timestr, "-%02d%02d%02d%02d%02d%02d.out",local->tm_year+1900, local->tm_mon, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
            break;
    }
    return timestr;
}

/**
 * 根据传入的 dir 和 filename 获取 path
 * @param  dir      文件夹路径
 * @param  filename 文件名
 * @return 指向一个存储路径的字符串, 不要 free
*/
static char* _logPath(const char* dir, const char* name)
{
    static char/***/ r_path[MAX_PATH_LENGTH + 1]/* = NULL*/;
//    if(!r_path)
//        r_path = (char*)malloc(MAX_PATH_LENGTH + 1);
    bzero(r_path, MAX_PATH_LENGTH + 1);
    if(dir)
        strcat(r_path, dir);
    if(name)
        strcat(r_path, name);
    strcat(r_path, _timeStr(TS_FILE));
    return r_path;
}

/**
 * @brief _mkdir - 根据路径依次创建文件夹, 直到文件的最底层
 * @param path  路径, 文件夹或文件路径均可
 * @param mode  权限, 推荐 0755
 */
void _mkdir(const char* path, mode_t mode)
{
    char* head = strdup(path);
    char* tail = head;

    while(*tail)
    {
        if('/' == *tail)
        {
            *tail = '\0';
            if(mkdir(head, mode))
                logsysAdd(NULL, "%s(%d)-[mkdir]: \"%s\" %s\n", __FILE__, __LINE__, head, strerror(errno));
            else
                logsysAdd(NULL, "mkdir \"%s\" ok\n", head);
            *tail = '/';
        }
        tail++;
    }

    free(head);
}

/**
 * @brief _logFileShrink - 若 日志文件 已达上限, 则清空文件
 * @param log
 */
void _logFileShrink(LogPtr log)
{
    if(0 != log->maxsize && _logFileSize(log) > log->maxsize)
    {
        logsysAdd(log, "Test to reach the upper file limitation ~!, Empty file...\n");
        _logFlieEmpty(log);
    }
}

size_t _logFileSize(LogPtr log)
{
    fseek(log->fp, 0, SEEK_END);
    return ftell(log->fp);
}

int _logFlieEmpty(LogPtr log)
{
    int fd = fileno(log->fp);
    fd = ftruncate(fd, 0);
    rewind(log->fp);
    fflush(log->fp);
    return fd;
}
/* ---------------------- logcheck private prototypes ---------------------------- */
/**
 * @brief _check_logsys - // 检查服务是否开启, 并输出相应提示信息
 * @param name  log 名称
 * @param tag   输出标记, 用以区分不同的操作
 * @return
 */
int _check_logsys(const char* name, const char* tag)
{
    if(!_logsys_service){/* 服务未开启, 返回 err */
        logsysAdd(NULL, "[%s] %s... err: logsys service is off \n", name, tag);
        return logsysShow("[%s] %s... err: logsys service is off \n", name, tag);
    }
    return LOG_OK;
}
int _check_name(const char* name, const char* tag)    // 检查 name 是否合法, 并输出相应提示信息
{
    if(NULL == name || !*name) {/* 字串不合法, 返回 err */
        logsysAdd(NULL, "[%s] %s... err: name is illegal \n", name, tag);
        return logsysShow("[%s] %s... err: name is illegal \n", name, tag);
    }
    return LOG_OK;
}
int _check_path(const char* path, const char* tag)                           // 检查 path 是否合法, 并输出相应提示信息
{
    if(NULL == path || !*path) {/* 字串不合法, 返回 err */
        logsysAdd(NULL, "[%s] %s... err: path is illegal \n", path, tag);
        return logsysShow("[%s] %s... err: path is illegal \n", path, tag);
    }
    return LOG_OK;
}
LogPtr _check_log(const char* name, const char* tag)                            // 检查 log 是否存在, 并输出相应提示信息
{
    /* 未找到指定的 log, 返回 err */
    LogPtr r_log = _logdictFetchValue(_logsys_dic, name);
    if(!r_log){
        logsysAdd(NULL, "[%s] %s... err: log not exist \n", name, tag);
        logsysShow("[%s] %s... err: log not exist \n", name, tag);
    }
    return r_log;
}
int _check_size_mb(size_t size_mb, const char* name, const char* tag)
{
    if(size_mb > INT_MAX>>20){/* 大小不合法, 返回 err */
        logsysAdd(NULL, "[%s] %s... err: too large to set \n", name, tag);
        return logsysShow("[%s] %s... err: too large to set \n", name, tag);
    }
    return LOG_OK;
}

/* ---------------------- test functions ----------------------------------------- */
// ...
