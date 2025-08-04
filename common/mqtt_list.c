/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-11 22:46:33
 * @LastEditTime: 2020-04-27 23:28:12
 * @Description: MQTT 链表实现文件
 *               该文件实现了双向循环链表的核心操作，包括：
 *               - 链表节点的插入和删除
 *               - 链表初始化和状态检查
 *               - 节点移动和重排序
 *               以下代码参考了 TencentOS tiny，请根据许可证保留作者信息和源代码。
 */

# include "mqtt_list.h"

/**
 * @brief 在指定位置插入链表节点
 * 
 * 在 prev 和 next 节点之间插入新节点 node。
 * 这是链表操作的基础函数，其他插入操作都基于此函数实现。
 * 
 * @param[in] node  要插入的节点
 * @param[in] prev  前驱节点
 * @param[in] next  后继节点
 * 
 * @note 此函数为内部函数，不直接对外暴露
 */
static void _mqtt_list_add(mqtt_list_t *node, mqtt_list_t *prev, mqtt_list_t *next)
{
    next->prev = node;    // 后继节点的前驱指向新节点
    node->next = next;    // 新节点的后继指向后继节点
    node->prev = prev;    // 新节点的前驱指向前驱节点
    prev->next = node;    // 前驱节点的后继指向新节点
}

/**
 * @brief 删除指定位置的链表节点
 * 
 * 删除 prev 和 next 节点之间的节点，通过调整指针实现删除操作。
 * 这是链表删除操作的基础函数。
 * 
 * @param[in] prev  要删除节点的前驱节点
 * @param[in] next  要删除节点的后继节点
 * 
 * @note 此函数为内部函数，不直接对外暴露
 */
static void _mqtt_list_del(mqtt_list_t *prev, mqtt_list_t *next)
{
    next->prev = prev;    // 后继节点的前驱指向前驱节点
    prev->next = next;    // 前驱节点的后继指向后继节点
}

/**
 * @brief 删除指定的链表节点
 * 
 * 删除指定的 entry 节点，通过调用 _mqtt_list_del 实现。
 * 
 * @param[in] entry  要删除的节点
 * 
 * @note 此函数为内部函数，不直接对外暴露
 */
static void _mqtt_list_del_entry(mqtt_list_t *entry)
{
    _mqtt_list_del(entry->prev, entry->next);    // 调用基础删除函数
}

/**
 * @brief 初始化链表
 * 
 * 将链表头节点初始化为空链表状态。
 * 空链表的特征是头节点的 next 和 prev 都指向自身。
 * 
 * @param[in,out] list  要初始化的链表头节点
 * 
 * @note 链表头节点本身不存储数据，仅作为链表的入口点
 */
void mqtt_list_init(mqtt_list_t *list)
{
    list->next = list;    // 后继指向自身
    list->prev = list;    // 前驱指向自身
}

/**
 * @brief 在链表头部插入节点
 * 
 * 将新节点插入到链表头部（头节点的下一个位置）。
 * 新插入的节点将成为链表的第一个数据节点。
 * 
 * @param[in] node  要插入的节点
 * @param[in] list  目标链表
 * 
 * @note 插入后，node 成为链表的第一个数据节点
 */
void mqtt_list_add(mqtt_list_t *node, mqtt_list_t *list)
{
    _mqtt_list_add(node, list, list->next);    // 在头节点和第一个数据节点之间插入
}

/**
 * @brief 在链表尾部插入节点
 * 
 * 将新节点插入到链表尾部（头节点的前一个位置）。
 * 新插入的节点将成为链表的最后一个数据节点。
 * 
 * @param[in] node  要插入的节点
 * @param[in] list  目标链表
 * 
 * @note 插入后，node 成为链表的最后一个数据节点
 */
void mqtt_list_add_tail(mqtt_list_t *node, mqtt_list_t *list)
{
    _mqtt_list_add(node, list->prev, list);    // 在最后一个数据节点和头节点之间插入
}

/**
 * @brief 从链表中删除节点
 * 
 * 从链表中删除指定的节点，但不重新初始化该节点。
 * 删除后，该节点可以重新插入到其他位置。
 * 
 * @param[in] entry  要删除的节点
 * 
 * @note 删除后，entry 节点的指针仍然有效，可以重新使用
 */
void mqtt_list_del(mqtt_list_t *entry)
{
    _mqtt_list_del(entry->prev, entry->next);    // 调用基础删除函数
}

/**
 * @brief 从链表中删除节点并重新初始化
 * 
 * 从链表中删除指定的节点，并将该节点重新初始化为空节点。
 * 删除后，该节点处于可重新使用的状态。
 * 
 * @param[in] entry  要删除的节点
 * 
 * @note 删除后，entry 节点被重新初始化，可以安全地重新使用
 */
void mqtt_list_del_init(mqtt_list_t *entry)
{
    _mqtt_list_del_entry(entry);    // 删除节点
    mqtt_list_init(entry);          // 重新初始化节点
}

/**
 * @brief 将节点移动到链表头部
 * 
 * 将指定节点从当前位置删除，然后插入到链表头部。
 * 该节点将成为链表的第一个数据节点。
 * 
 * @param[in] node  要移动的节点
 * @param[in] list  目标链表
 * 
 * @note 移动操作包含删除和插入两个步骤
 */
void mqtt_list_move(mqtt_list_t *node, mqtt_list_t *list)
{
    _mqtt_list_del_entry(node);     // 从当前位置删除
    mqtt_list_add(node, list);      // 插入到链表头部
}

/**
 * @brief 将节点移动到链表尾部
 * 
 * 将指定节点从当前位置删除，然后插入到链表尾部。
 * 该节点将成为链表的最后一个数据节点。
 * 
 * @param[in] node  要移动的节点
 * @param[in] list  目标链表
 * 
 * @note 移动操作包含删除和插入两个步骤
 */
void mqtt_list_move_tail(mqtt_list_t *node, mqtt_list_t *list)
{
    _mqtt_list_del_entry(node);         // 从当前位置删除
    mqtt_list_add_tail(node, list);     // 插入到链表尾部
}

/**
 * @brief 检查链表是否为空
 * 
 * 通过检查头节点的后继是否指向自身来判断链表是否为空。
 * 空链表的特征是头节点的 next 指向自身。
 * 
 * @param[in] list  要检查的链表
 * @return 
 *   - 1: 链表为空
 *   - 0: 链表不为空
 * 
 * @note 此函数是判断链表状态的标准方法
 */
int mqtt_list_is_empty(mqtt_list_t *list)
{
    return list->next == list;    // 检查头节点的后继是否指向自身
}
