/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

// 每个此结构保存着一个键值对
typedef struct dictEntry {
    // 键
    void *key;

    // 值
    // 注意，这是很酷的一个用法，在不同的场景面前能很好的适应，语意非常清晰
    // 譬如 dictEntry 存储的是一个有符号整数，那么 dictEntry.v.s64
    // 譬如 dictEntry 存储的是一个无符号整数，那么 dictEntry.v.us64
    // 譬如 dictEntry 存储的一块数据，那么 dictEntry.v.val
    // 对于需要应付多种数据类型的数据结构，可以采用这种方法
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
    } v;

    // 指向下个哈希节点的指针 将多个哈希值相同的键值对链接在一起 以此来解决键冲突
    // 链地址法解决hash冲突
    struct dictEntry *next;
} dictEntry;

typedef struct dictType {
    // 哈希函数
    unsigned int (*hashFunction)(const void *key);

    // 键拷贝函数
    void *(*keyDup)(void *privdata, const void *key);

    // 值拷贝函数
    void *(*valDup)(void *privdata, const void *obj);

    // 比较函数
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);

    // 键析构函数
    void (*keyDestructor)(void *privdata, void *key);

    // 值析构函数
    void (*valDestructor)(void *privdata, void *obj);
} dictType;


/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
// 哈希表 字典中使用的
typedef struct dictht {
    // 哈希表数组 每个元素都指向dictEntry 即每个dictEntry都代表一个键值对
    // 指针数组: 数组 数组中存放的是指针
    dictEntry **table;

    // 哈希表的大小 table其中存储dictEntry的数量 table数组的大小
    unsigned long size;

    // 哈希表大小掩码 用于计算索引值
    // 总是等于size-1 这个属性和哈希值一起决定了一个键应该被放到table数据的哪个索引上面
    // index = hash(key) & ht[0].sizemask    hash为根据键做哈希函数(murmurHash2函数)计算出的hash值 index为table数组的下标
    unsigned long sizemask;

    // 哈希表中已有数据项数量
    unsigned long used;
} dictht;



// 字典 数据结构，redis 的所有键值对都会存储在这里
// 字典是将哈希表dictht又封装了一层 使用两个哈希表实现渐进哈希
// 当要将一个新的键值添加到字典里面时, 先根据键值对的键计算出哈希值和索引值, 然后再根据索引值,
// 将包含新键值对的哈希表节点放到哈希表数组的指定索引上面;
// 1: hash = dict->type->hashFunction(key)  当被用作数据库底层实现/哈希键的底层实现MurmurHash2算法来计算hash
// 2: index = hash & dict->ht[x].sizemask index是dictht->table中的下标
typedef struct dict {
    // 哈希表的类型特定函数，包括哈希函数，比较函数，键值的内存释放函数
    dictType *type;

    // 存储一些额外的私有数据
    void *privdata;

    // 2个哈希表, 一般只使用ht[0]哈希表, ht[1]只会在对ht[0]哈希表进行rehash时使用
    dictht ht[2];

    // rehash索引 记录了当前rehash的进度 当rehash不再进行时 值为-1
    // 哈希表重置下标，值不为-1时 指向正在进行rehash的ht[0]哈希数组的数组下标
    // rehashidx之前的数组已经完成rehash
    // 详见: dictRehash
    int rehashidx; /* rehashing not in progress if rehashidx == -1 */

    // 绑定到哈希表的迭代器个数
    int iterators; /* number of iterators currently running */
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;
    int table, index, safe;
    dictEntry *entry, *nextEntry;
    long long fingerprint; /* unsafe iterator fingerprint for misuse detection */
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

// define 中 do while(0) 的做法写 C 语言的同学应该非常熟悉。为的是适应下面的
// 用法：
// if(conditions)
//     DO_SOMETHING();
// else
//     DO_OTHERTHING();
// 假如 define 里面有多条语句，上面的用法也不会出现语法或者语意错误。
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        entry->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        entry->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { entry->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { entry->v.u64 = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
#define dictIsRehashing(ht) ((ht)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
dictEntry *dictReplaceRaw(dict *d, void *key);
int dictDelete(dict *d, const void *key);
int dictDeleteNoFree(dict *d, const void *key);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
void dictPrintStats(dict *d);
unsigned int dictGenHashFunction(const void *key, int len);
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);


int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(unsigned int initval);
unsigned int dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
