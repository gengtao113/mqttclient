/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-11 22:47:55
 * @LastEditTime: 2020-10-17 14:18:02
 * @Description: MQTT 链表头文件
 *               定义了双向循环链表的数据结构和操作接口，包括：
 *               - 链表节点结构定义
 *               - 链表操作宏定义
 *               - 链表遍历宏定义
 *               代码属于 jiejie，请根据许可证保留作者信息和源代码。
 */
#ifndef _MQTT_LIST_H_
#define _MQTT_LIST_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 链表节点结构体
 * 
 * 定义了双向循环链表的节点结构，包含前驱和后继指针。
 * 每个节点都可以作为链表的一部分，也可以作为链表头。
 * 
 * @note 这是一个通用的链表实现，可以嵌入到任何结构体中使用
 */
typedef struct mqtt_list_node {
    struct mqtt_list_node *next;    // 指向后继节点的指针
    struct mqtt_list_node *prev;    // 指向前驱节点的指针
} mqtt_list_t;

/**
 * @brief 计算结构体成员相对于结构体起始地址的偏移量
 * 
 * @param[in] type  结构体类型
 * @param[in] field 成员字段名
 * @return 成员字段在结构体中的偏移量（字节）
 * 
 * @note 使用 (type *)0 技巧计算偏移量，避免编译器警告
 */
#define OFFSET_OF_FIELD(type, field) \
    ((size_t)&(((type *)0)->field))

/**
 * @brief 根据成员指针获取包含该成员的结构体指针
 * 
 * @param[in] ptr   成员指针
 * @param[in] type  结构体类型
 * @param[in] field 成员字段名
 * @return 包含该成员的结构体指针
 * 
 * @note 这是 container_of 宏的实现，用于从链表节点获取包含它的结构体
 */
#define CONTAINER_OF_FIELD(ptr, type, field) \
    ((type *)((unsigned char *)(ptr) - OFFSET_OF_FIELD(type, field)))

/**
 * @brief 创建链表节点初始化器
 * 
 * 用于静态初始化链表节点，使其指向自身形成空节点。
 * 
 * @param[in] node 节点变量名
 * @return 链表节点初始化器
 */
#define LIST_NODE(node) \
    { &(node), &(node) }

/**
 * @brief 定义并初始化链表
 * 
 * 定义一个新的链表变量并初始化为空链表状态。
 * 
 * @param[in] list 链表变量名
 * @return 链表定义和初始化语句
 */
#define LIST_DEFINE(list) \
    mqtt_list_t list = { &(list), &(list) }

/**
 * @brief 从链表节点获取包含结构体
 * 
 * 根据链表节点指针获取包含该节点的结构体指针。
 * 这是实现链表与数据结构分离的关键宏。
 * 
 * @param[in] list  链表节点指针
 * @param[in] type  结构体类型
 * @param[in] field 链表成员在结构体中的字段名
 * @return 包含链表节点的结构体指针
 * 
 * @note 使用示例：LIST_ENTRY(node, my_struct_t, list)
 */
#define LIST_ENTRY(list, type, field) \
    CONTAINER_OF_FIELD(list, type, field)

/**
 * @brief 获取链表的第一个元素
 * 
 * 获取链表中第一个数据节点对应的结构体指针。
 * 
 * @param[in] list  链表头指针
 * @param[in] type  结构体类型
 * @param[in] field 链表成员字段名
 * @return 第一个数据节点的结构体指针
 * 
 * @note 如果链表为空，返回的是头节点对应的结构体
 */
#define LIST_FIRST_ENTRY(list, type, field) \
    LIST_ENTRY((list)->next, type, field)

/**
 * @brief 获取链表的第一个元素（安全版本）
 * 
 * 获取链表中第一个数据节点对应的结构体指针。
 * 如果链表为空，返回 NULL。
 * 
 * @param[in] list  链表头指针
 * @param[in] type  结构体类型
 * @param[in] field 链表成员字段名
 * @return 第一个数据节点的结构体指针，链表为空时返回 NULL
 */
#define LIST_FIRST_ENTRY_OR_NULL(list, type, field) \
    (mqtt_list_is_empty(list) ? NULL : LIST_FIRST_ENTRY(list, type, field))

/**
 * @brief 正向遍历链表
 * 
 * 从链表头部开始，正向遍历所有数据节点。
 * 
 * @param[in] curr  当前节点指针变量
 * @param[in] list  链表头指针
 * 
 * @note 使用示例：
 * @code
 * mqtt_list_t *curr;
 * LIST_FOR_EACH(curr, &my_list) {
 *     my_struct_t *entry = LIST_ENTRY(curr, my_struct_t, list);
 *     // 处理 entry
 * }
 * @endcode
 */
#define LIST_FOR_EACH(curr, list) \
    for (curr = (list)->next; curr != (list); curr = curr->next)

/**
 * @brief 反向遍历链表
 * 
 * 从链表尾部开始，反向遍历所有数据节点。
 * 
 * @param[in] curr  当前节点指针变量
 * @param[in] list  链表头指针
 */
#define LIST_FOR_EACH_PREV(curr, list) \
    for (curr = (list)->prev; curr != (list); curr = curr->prev)

/**
 * @brief 安全正向遍历链表
 * 
 * 正向遍历链表，支持在遍历过程中删除当前节点。
 * 
 * @param[in] curr  当前节点指针变量
 * @param[in] next  下一个节点指针变量
 * @param[in] list  链表头指针
 * 
 * @note 使用示例：
 * @code
 * mqtt_list_t *curr, *next;
 * LIST_FOR_EACH_SAFE(curr, next, &my_list) {
 *     my_struct_t *entry = LIST_ENTRY(curr, my_struct_t, list);
 *     if (need_delete(entry)) {
 *         mqtt_list_del(curr);
 *         free(entry);
 *     }
 * }
 * @endcode
 */
#define LIST_FOR_EACH_SAFE(curr, next, list) \
    for (curr = (list)->next, next = curr->next; curr != (list); \
            curr = next, next = curr->next)

/**
 * @brief 安全反向遍历链表
 * 
 * 反向遍历链表，支持在遍历过程中删除当前节点。
 * 
 * @param[in] curr  当前节点指针变量
 * @param[in] next  下一个节点指针变量
 * @param[in] list  链表头指针
 */
#define LIST_FOR_EACH_PREV_SAFE(curr, next, list) \
    for (curr = (list)->prev, next = curr->prev; \
            curr != (list); \
            curr = next, next = curr->prev)

/* ==================== 函数声明 ==================== */

/**
 * @brief 初始化链表
 * 
 * @param[in,out] list  要初始化的链表头节点
 */
void mqtt_list_init(mqtt_list_t *list);

/**
 * @brief 在链表头部插入节点
 * 
 * @param[in] node  要插入的节点
 * @param[in] list  目标链表
 */
void mqtt_list_add(mqtt_list_t *node, mqtt_list_t *list);

/**
 * @brief 在链表尾部插入节点
 * 
 * @param[in] node  要插入的节点
 * @param[in] list  目标链表
 */
void mqtt_list_add_tail(mqtt_list_t *node, mqtt_list_t *list);

/**
 * @brief 从链表中删除节点
 * 
 * @param[in] entry  要删除的节点
 */
void mqtt_list_del(mqtt_list_t *entry);

/**
 * @brief 从链表中删除节点并重新初始化
 * 
 * @param[in] entry  要删除的节点
 */
void mqtt_list_del_init(mqtt_list_t *entry);

/**
 * @brief 将节点移动到链表头部
 * 
 * @param[in] node  要移动的节点
 * @param[in] list  目标链表
 */
void mqtt_list_move(mqtt_list_t *node, mqtt_list_t *list);

/**
 * @brief 将节点移动到链表尾部
 * 
 * @param[in] node  要移动的节点
 * @param[in] list  目标链表
 */
void mqtt_list_move_tail(mqtt_list_t *node, mqtt_list_t *list);

/**
 * @brief 检查链表是否为空
 * 
 * @param[in] list  要检查的链表
 * @return 
 *   - 1: 链表为空
 *   - 0: 链表不为空
 */
int mqtt_list_is_empty(mqtt_list_t *list);

#ifdef __cplusplus
}
#endif

#endif /* _LIST_H_ */

