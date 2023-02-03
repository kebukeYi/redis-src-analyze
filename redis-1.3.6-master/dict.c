/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>

#include "dict.h"
#include "zmalloc.h"

/* ---------------------------- Utility funcitons --------------------------- */

static void _dictPanic(const char *fmt, ...)  /* 发生异常时，打印字典错误信息 ... 表示可以接收多个参数 */
{
    va_list ap;  // 定义变量列表

    va_start(ap, fmt);  // 格式化变量
    fprintf(stderr, "\nDICT LIBRARY PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n\n");
    va_end(ap);  // 
}

/* ------------------------- Heap Management Wrappers ----------------------- */

static void *_dictAlloc(size_t size)  /* 给字典分配内存 */
{
    void *p = zmalloc(size);  // 分配内存，获取指针
    if (p == NULL)
        _dictPanic("Out of memory");  // 如果获取不到指针，则表示内存不足
    return p;
}

static void _dictFree(void *ptr) {  /* 释放字典内存 */
    zfree(ptr);  // 释放字典
}

/* -------------------------- private prototypes ---------------------------- */
/* 私有函数原型，定义了4个静态函数 */
static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static int _dictKeyIndex(dict *ht, const void *key);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

/* Thomas Wang's 32 bit Mix Function */
unsigned int dictIntHashFunction(unsigned int key)  /* 传入key进行位运算获取整型哈希值 */
{
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;  // 返回哈希值
}

/* Identity hash function for integer keys */
unsigned int dictIdentityHashFunction(unsigned int key)  /* 验证传入的key是int型的值 */
{
    return key;
}

/* Generic hash function (a popular one from Bernstein).
 * I tested a few and this was the best. */
unsigned int dictGenHashFunction(const unsigned char *buf, int len) {  /* 通用哈希函数，传入字符数组和长度 */
    unsigned int hash = 5381;

    while (len--)
        hash = ((hash << 5) + hash) + (*buf++); /* hash * 33 + c，依次对字符数组中的每个字符进行哈希运算，生成整型哈希值 */
    return hash;
}

/* ----------------------------- API implementation ------------------------- */

/* Reset an hashtable already initialized with ht_init().
 * NOTE: This function should only called by ht_destroy(). */
static void _dictReset(dict *ht)  /* 初始化字典，传入字典指针 */
{
    ht->table = NULL;  // 初始化哈希表数组为空
    ht->size = 0;  // 初始化哈希表大小
    ht->sizemask = 0;  // 初始化哈希表大小掩码，用于计算索引值
    ht->used = 0;  // 初始化哈希表已有节点的个数0
}

/* Create a new hash table */
dict *dictCreate(dictType *type,  /* 创建1个哈希表，传入类型和私有数据指针 */
        void *privDataPtr)  /* 字典类型再文件末尾定义，分别为dictTypeHeapStringCopyKey、dictTypeHeapStrings、dictTypeHeapStringCopyKeyValue */
{
    dict *ht = _dictAlloc(sizeof(*ht));  // 给哈希表分配空间

    _dictInit(ht,type,privDataPtr);  // 初始化字典，方法内部调用了 _dictReset()
    return ht;  // 返回哈希表
}

/* Initialize the hash table */
int _dictInit(dict *ht, dictType *type,  /* 初始化哈希表，传入哈希表、类型、私有数据指针 */
        void *privDataPtr)
{
    _dictReset(ht);  // 调用 _dictReset()，初始化哈希表的4个成员变量
    ht->type = type;  // 初始化哈希表的类型
    ht->privdata = privDataPtr;  // 初始化哈希表的私有数据
    return DICT_OK;  // 返回成功标识
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USER/BUCKETS ration near to <= 1 */
int dictResize(dict *ht)  /* 扩容字典 */
{
    int minimal = ht->used;  // 获取字典中已有节点的数量

    if (minimal < DICT_HT_INITIAL_SIZE)  // 如果used小于等于4
        minimal = DICT_HT_INITIAL_SIZE;  // 将size扩展到4个
    return dictExpand(ht, minimal);
}

/* Expand or create the hashtable */
int dictExpand(dict *ht, unsigned long size)  /* 扩容字典，传入哈希表、要扩展到的大小 */
{
    dict n; /* the new hashtable */  // 初始化1个临时字典
    unsigned long realsize = _dictNextPower(size), i;  // 

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hashtable */
    if (ht->used > size)  // 如果要扩展到的大小小于已存在的个数
        return DICT_ERR;  // 返回错误

    _dictInit(&n, ht->type, ht->privdata);  // 初始化字典n
    n.size = realsize;  // 将n的size设置为扩容后的大小
    n.sizemask = realsize-1;  // 将n的大小掩码设置为扩容后大小减1
    n.table = _dictAlloc(realsize*sizeof(dictEntry*));  // 分配n的哈希表空间为扩容后的大小

    /* Initialize all the pointers to NULL */
    memset(n.table, 0, realsize*sizeof(dictEntry*));  // 将哈希表中的指针都初始化为空

    /* Copy all the elements from the old to the new table:  把旧哈希表的所有元素拷贝到新哈希表中
     * note that if the old hash table is empty ht->size is zero,  注意：如果旧哈希表是空的、size是0，扩容只会创建一个新的哈希表
     * so dictExpand just creates an hash table. */
    n.used = ht->used;  // 初始化已有节点个数
    for (i = 0; i < ht->size && ht->used > 0; i++) {  // 遍历哈希表的元素
        dictEntry *he, *nextHe;  // 定义entry和下一个entry

        if (ht->table[i] == NULL) continue;  // 如果某个索引的entry为空，继续找下一个
        
        /* For each hash entry on this slot... */
        he = ht->table[i];  // 如果找到entry
        while(he) {
            unsigned int h;  // 初始化索引变量h

            nextHe = he->next;  // 保存下一个entry位置
            /* Get the new element index */
            h = dictHashKey(ht, he->key) & n.sizemask;  // 找到新元素索引
            he->next = n.table[h];
            n.table[h] = he;  // 将找到的entry保存到字典n中的哈希表对应的索引位置
            ht->used--;  // 将旧哈希表待迁移的节点数减1
            /* Pass to the next element */
            he = nextHe;  // 向后继续查找entry
        }
    }
    assert(ht->used == 0);  // 断言待迁移的节点数为0
    _dictFree(ht->table);  // 释放旧哈希表

    /* Remap the new hashtable in the old */
    *ht = n;  // 将哈希表指针指向新的哈希表n
    return DICT_OK;  // 返回成功标识
}

/* Add an element to the target hash table */
int dictAdd(dict *ht, void *key, void *val)  /*  */
{
    int index;
    dictEntry *entry;

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    if ((index = _dictKeyIndex(ht, key)) == -1)
        return DICT_ERR;

    /* Allocates the memory and stores key */
    entry = _dictAlloc(sizeof(*entry));
    entry->next = ht->table[index];
    ht->table[index] = entry;

    /* Set the hash entry fields. */
    dictSetHashKey(ht, entry, key);
    dictSetHashVal(ht, entry, val);
    ht->used++;
    return DICT_OK;
}

/* Add an element, discarding the old if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
int dictReplace(dict *ht, void *key, void *val)
{
    dictEntry *entry, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will suceed. */
    if (dictAdd(ht, key, val) == DICT_OK)
        return 1;
    /* It already exists, get the entry */
    entry = dictFind(ht, key);
    /* Free the old value and set the new one */
    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    auxentry = *entry;
    dictSetHashVal(ht, entry, val);
    dictFreeEntryVal(ht, &auxentry);
    return 0;
}

/* Search and remove an element */
static int dictGenericDelete(dict *ht, const void *key, int nofree)
{
    unsigned int h;
    dictEntry *he, *prevHe;

    if (ht->size == 0)
        return DICT_ERR;
    h = dictHashKey(ht, key) & ht->sizemask;
    he = ht->table[h];

    prevHe = NULL;
    while(he) {
        if (dictCompareHashKeys(ht, key, he->key)) {
            /* Unlink the element from the list */
            if (prevHe)
                prevHe->next = he->next;
            else
                ht->table[h] = he->next;
            if (!nofree) {
                dictFreeEntryKey(ht, he);
                dictFreeEntryVal(ht, he);
            }
            _dictFree(he);
            ht->used--;
            return DICT_OK;
        }
        prevHe = he;
        he = he->next;
    }
    return DICT_ERR; /* not found */
}

int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0);
}

int dictDeleteNoFree(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,1);
}

/* Destroy an entire hash table */
int _dictClear(dict *ht)
{
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;

        if ((he = ht->table[i]) == NULL) continue;
        while(he) {
            nextHe = he->next;
            dictFreeEntryKey(ht, he);
            dictFreeEntryVal(ht, he);
            _dictFree(he);
            ht->used--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    _dictFree(ht->table);
    /* Re-initialize the table */
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *ht)
{
    _dictClear(ht);
    _dictFree(ht);
}

dictEntry *dictFind(dict *ht, const void *key)
{
    dictEntry *he;
    unsigned int h;

    if (ht->size == 0) return NULL;
    h = dictHashKey(ht, key) & ht->sizemask;
    he = ht->table[h];
    while(he) {
        if (dictCompareHashKeys(ht, key, he->key))
            return he;
        he = he->next;
    }
    return NULL;
}

dictIterator *dictGetIterator(dict *ht)
{
    dictIterator *iter = _dictAlloc(sizeof(*iter));

    iter->ht = ht;
    iter->index = -1;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        if (iter->entry == NULL) {
            iter->index++;
            if (iter->index >=
                    (signed)iter->ht->size) break;
            iter->entry = iter->ht->table[iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

void dictReleaseIterator(dictIterator *iter)
{
    _dictFree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *ht)
{
    dictEntry *he;
    unsigned int h;
    int listlen, listele;

    if (ht->used == 0) return NULL;
    do {
        h = random() & ht->sizemask;
        he = ht->table[h];
    } while(he == NULL);

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is to count the element and
     * select a random index. */
    listlen = 0;
    while(he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = ht->table[h];
    while(listele--) he = he->next;
    return he;
}

/* ------------------------- private functions ------------------------------ */

/* Expand the hash table if needed */
static int _dictExpandIfNeeded(dict *ht)
{
    /* If the hash table is empty expand it to the intial size,
     * if the table is "full" dobule its size. */
    if (ht->size == 0)
        return dictExpand(ht, DICT_HT_INITIAL_SIZE);
    if (ht->used == ht->size)
        return dictExpand(ht, ht->size*2);
    return DICT_OK;
}

/* Our hash table capability is a power of two */  /* 哈希表entry的容量是2的幂 */
static unsigned long _dictNextPower(unsigned long size)  /* 获取字典size的下一个幂值 */
{
    unsigned long i = DICT_HT_INITIAL_SIZE;  // 初始化i为4

    if (size >= LONG_MAX) return LONG_MAX;  // 如果size大于等于long型数的最大值，则返回最大值
    while(1) {
        if (i >= size)
            return i;
        i *= 2;  // 如果i小于size，则乘以2
    }
}

/* Returns the index of a free slot that can be populated with
 * an hash entry for the given 'key'.
 * If the key already exists, -1 is returned. */
static int _dictKeyIndex(dict *ht, const void *key)
{
    unsigned int h;
    dictEntry *he;

    /* Expand the hashtable if needed */
    if (_dictExpandIfNeeded(ht) == DICT_ERR)
        return -1;
    /* Compute the key hash value */
    h = dictHashKey(ht, key) & ht->sizemask;
    /* Search if this slot does not already contain the given key */
    he = ht->table[h];
    while(he) {
        if (dictCompareHashKeys(ht, key, he->key))
            return -1;
        he = he->next;
    }
    return h;
}

void dictEmpty(dict *ht) {
    _dictClear(ht);
}

#define DICT_STATS_VECTLEN 50
void dictPrintStats(dict *ht) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];

    if (ht->used == 0) {
        printf("No stats available for empty dictionaries\n");
        return;
    }

    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }
    printf("Hash table stats:\n");
    printf(" table size: %ld\n", ht->size);
    printf(" number of elements: %ld\n", ht->used);
    printf(" different slots: %ld\n", slots);
    printf(" max chain length: %ld\n", maxchainlen);
    printf(" avg chain length (counted): %.02f\n", (float)totchainlen/slots);
    printf(" avg chain length (computed): %.02f\n", (float)ht->used/slots);
    printf(" Chain length distribution:\n");
    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        printf("   %s%ld: %ld (%.02f%%)\n",(i == DICT_STATS_VECTLEN-1)?">= ":"", i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }
}

/* ----------------------- StringCopy Hash Table Type ------------------------*/

static unsigned int _dictStringCopyHTHashFunction(const void *key)
{
    return dictGenHashFunction(key, strlen(key));
}

static void *_dictStringCopyHTKeyDup(void *privdata, const void *key)
{
    int len = strlen(key);
    char *copy = _dictAlloc(len+1);
    DICT_NOTUSED(privdata);

    memcpy(copy, key, len);
    copy[len] = '\0';
    return copy;
}

static void *_dictStringKeyValCopyHTValDup(void *privdata, const void *val)
{
    int len = strlen(val);
    char *copy = _dictAlloc(len+1);
    DICT_NOTUSED(privdata);

    memcpy(copy, val, len);
    copy[len] = '\0';
    return copy;
}

static int _dictStringCopyHTKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcmp(key1, key2) == 0;
}

static void _dictStringCopyHTKeyDestructor(void *privdata, void *key)
{
    DICT_NOTUSED(privdata);

    _dictFree((void*)key); /* ATTENTION: const cast */
}

static void _dictStringKeyValCopyHTValDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    _dictFree((void*)val); /* ATTENTION: const cast */
}

dictType dictTypeHeapStringCopyKey = {  /* 字典类型 */
    _dictStringCopyHTHashFunction,        /* hash function */
    _dictStringCopyHTKeyDup,              /* key dup */
    NULL,                               /* val dup */
    _dictStringCopyHTKeyCompare,          /* key compare */
    _dictStringCopyHTKeyDestructor,       /* key destructor */
    NULL                                /* val destructor */
};

/* This is like StringCopy but does not auto-duplicate the key.
 * It's used for intepreter's shared strings. */
dictType dictTypeHeapStrings = {
    _dictStringCopyHTHashFunction,        /* hash function */
    NULL,                               /* key dup */
    NULL,                               /* val dup */
    _dictStringCopyHTKeyCompare,          /* key compare */
    _dictStringCopyHTKeyDestructor,       /* key destructor */
    NULL                                /* val destructor */
};

/* This is like StringCopy but also automatically handle dynamic
 * allocated C strings as values. */
dictType dictTypeHeapStringCopyKeyValue = {
    _dictStringCopyHTHashFunction,        /* hash function */
    _dictStringCopyHTKeyDup,              /* key dup */
    _dictStringKeyValCopyHTValDup,        /* val dup */
    _dictStringCopyHTKeyCompare,          /* key compare */
    _dictStringCopyHTKeyDestructor,       /* key destructor */
    _dictStringKeyValCopyHTValDestructor, /* val destructor */
};
