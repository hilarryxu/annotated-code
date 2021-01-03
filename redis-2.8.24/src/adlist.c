/* adlist.c - A generic doubly linked list implementation
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


#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */

//---------------------------------------------------------------------
// 创建一个新的链表
//
// 失败时返回 NULL
//---------------------------------------------------------------------
list *listCreate(void)
{
    struct list *list;

    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* Free the whole list.
 *
 * This function can't fail. */

//---------------------------------------------------------------------
// 释放整个链表（包括每个节点的值）
//
// 这里是根据 list->len 来判断链表遍历结束的
//---------------------------------------------------------------------
void listRelease(list *list)
{
    unsigned long len;
    listNode *current, *next;

    current = list->head;
    len = list->len;
    while(len--) {
        next = current->next;
        if (list->free) list->free(current->value);
        zfree(current);
        current = next;
    }
    zfree(list);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */

//---------------------------------------------------------------------
// 新建一个包含给定 valude 的节点，并插入到链表头部
//
// 分配节点内存失败时返回 NULL
//---------------------------------------------------------------------
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        // 空链表特殊处理
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    // 更新链表长度
    list->len++;
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */

//---------------------------------------------------------------------
// 新建一个包含给定 valude 的节点，并插入到链表尾部
//
// 分配节点内存失败时返回 NULL
//---------------------------------------------------------------------
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        // 空链表特殊处理
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}


//---------------------------------------------------------------------
// 新建一个包含给定 valude 的节点
//
// after=1
//   插入到 old_node 之后
//
// after=0
//   插入到 old_node 之前
//---------------------------------------------------------------------
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (after) {
        // 插入到 old_node 之后
        node->prev = old_node;
        node->next = old_node->next;
        // 特殊处理尾部节点的情况
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
        // 插入到 old_node 之前
        node->next = old_node;
        node->prev = old_node->prev;
        // 特殊处理头部节点的情况
        if (list->head == old_node) {
            list->head = node;
        }
    }
    // 更新前置节点和后继节点的指针
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    // 更新链表长度
    list->len++;
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */

//---------------------------------------------------------------------
// 删除并释放链表中的指定节点
//---------------------------------------------------------------------
void listDelNode(list *list, listNode *node)
{
    // 处理前驱节点的指针以及头部节点特殊处理
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;

    // 处理后继节点的指针以及尾部节点特殊处理
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;

    // 释放节点值和节点
    if (list->free) list->free(node->value);
    zfree(node);

    // 更新链表长度
    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */

//---------------------------------------------------------------------
// 创建一个链表迭代器
//---------------------------------------------------------------------
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */

//---------------------------------------------------------------------
// 释放链表迭代器
//---------------------------------------------------------------------
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */

//---------------------------------------------------------------------
// 重置迭代器到链表头部
//---------------------------------------------------------------------
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

//---------------------------------------------------------------------
// 重置迭代器到链表尾部
//---------------------------------------------------------------------
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */

//---------------------------------------------------------------------
// 返回当前节点并步进迭代器到下一个节点
//---------------------------------------------------------------------
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */

//---------------------------------------------------------------------
// 复制整个链表
//
// 内存不够时返回 NULL
//---------------------------------------------------------------------
list *listDup(list *orig)
{
    list *copy;
    listIter *iter;
    listNode *node;

    // 分配内存并拷贝属性
    if ((copy = listCreate()) == NULL)
        return NULL;
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;

    // 创建一个正向迭代器
    iter = listGetIterator(orig, AL_START_HEAD);
    while((node = listNext(iter)) != NULL) {
        void *value;

        // 拷贝 value
        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                listReleaseIterator(iter);
                return NULL;
            }
        } else
            value = node->value;

        // 追加到拷贝链表的尾部
        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            listReleaseIterator(iter);
            return NULL;
        }
    }
    // 释放迭代器
    listReleaseIterator(iter);

    // 返回拷贝的链表
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */

//---------------------------------------------------------------------
// 从链表头部开始查找第一个匹配 key 的节点
//
// 有匹配函数：
//   根据 match 函数返回结果判断
// 无匹配函数：
//   key == node->value 来比较
//
// 查找失败返回 NULL
//---------------------------------------------------------------------
listNode *listSearchKey(list *list, void *key)
{
    listIter *iter;
    listNode *node;

    iter = listGetIterator(list, AL_START_HEAD);
    while((node = listNext(iter)) != NULL) {
        if (list->match) {
            if (list->match(node->value, key)) {
                listReleaseIterator(iter);
                return node;
            }
        } else {
            if (key == node->value) {
                listReleaseIterator(iter);
                return node;
            }
        }
    }
    listReleaseIterator(iter);
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */

//---------------------------------------------------------------------
// 按索引定位节点
//
// 0 头部
// ...
// -1 尾部
// 以此类推
//
// 索引超出范围返回 NULL
//---------------------------------------------------------------------
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {
        // 从尾部往前找
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
        // 从头部往后找
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */

//---------------------------------------------------------------------
// 取出尾部节点头插到链表头部
//---------------------------------------------------------------------
void listRotate(list *list) {
    listNode *tail = list->tail;

    // 不超过 1 个节点时不用操作
    if (listLength(list) <= 1) return;

    /* Detach current tail */
    // 取出尾部节点
    list->tail = tail->prev;
    list->tail->next = NULL;

    /* Move it as head */
    // 插入头部
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}
