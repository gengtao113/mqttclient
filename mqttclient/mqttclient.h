/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-09 21:31:25
 * @LastEditTime : 2022-06-11 22:45:02
 * @Description: the code belongs to jiejie, please keep the author information and source code according to the license.
 */
#ifndef _MQTTCLIENT_H_
#define _MQTTCLIENT_H_

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "MQTTPacket.h"
#include "mqtt_list.h"
#include "platform_timer.h"
#include "platform_memory.h"
#include "platform_mutex.h"
#include "platform_thread.h"
#include "mqtt_defconfig.h"
#include "network.h"
#include "random.h"
#include "mqtt_error.h"
#include "mqtt_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mqtt_qos {
    QOS0 = 0,
    QOS1 = 1,
    QOS2 = 2,
    SUBFAIL = 0x80
} mqtt_qos_t;

typedef enum client_state {
	CLIENT_STATE_INVALID = -1,
	CLIENT_STATE_INITIALIZED = 0,
	CLIENT_STATE_CONNECTED = 1,
	CLIENT_STATE_DISCONNECTED = 2,
    CLIENT_STATE_CLEAN_SESSION = 3
}client_state_t;

/**
 * @brief MQTT CONNACK 响应报文的有效载荷数据结构
 *
 * 该结构体用于解析或表示服务器返回的连接确认（CONNACK）报文中的关键信息。
 */
typedef struct mqtt_connack_data {
    uint8_t rc;                   ///< 连接返回码（0=连接成功，非0表示失败原因）
    uint8_t session_present;      ///< 会话是否存在标志（0=新会话，1=已有会话恢复）
} mqtt_connack_data_t;

/**
 * @brief MQTT 消息内容结构体
 *
 * 表示一条完整的 MQTT 消息（PUBLISH 报文内容），包括头部属性和有效载荷。
 */
typedef struct mqtt_message {
    mqtt_qos_t          qos;           ///< 消息服务质量等级（QOS0/QOS1/QOS2）
    uint8_t             retained;      ///< 保留标志（1=该消息为保留消息）
    uint8_t             dup;           ///< 重复标志（1=这是重发的消息，仅 QoS1/QoS2 有效）
    uint16_t            id;            ///< 报文标识符（Packet ID），QoS0 时为 0
    size_t              payloadlen;    ///< 有效载荷数据长度（字节）
    void                *payload;      ///< 指向有效载荷数据的指针（原始字节流）
} mqtt_message_t;

/**
 * @brief 消息数据包装结构体
 *
 * 将消息的主题名称与消息内容打包在一起，便于传递给消息处理回调函数。
 */
typedef struct message_data {
    char                topic_name[MQTT_TOPIC_LEN_MAX];  ///< 主题名称（以 '\0' 结尾的字符串）
    mqtt_message_t      *message;                        ///< 指向实际消息内容的指针
} message_data_t;

typedef void (*interceptor_handler_t)(void* client, message_data_t* msg);
typedef void (*message_handler_t)(void* client, message_data_t* msg);
typedef void (*reconnect_handler_t)(void* client, void* reconnect_date);

/**
 * @brief MQTT 消息处理器结构体
 *
 * 该结构体用于注册和管理针对特定主题过滤器（topic filter）的消息回调函数。
 * 客户端通过该结构将主题与对应的处理函数（handler）关联起来，当收到匹配主题的消息时，
 * 会遍历处理器列表并调用相应的回调函数。
 *
 * 此结构通常作为节点插入链表（由 mqtt_list_t 管理），实现多主题订阅的回调分发机制。
 *
 * @note
 *   - `topic_filter` 支持 MQTT 通配符：`+`（单层通配）、`#`（多层通配）
 *   - `handler` 回调在接收线程中执行，应保证线程安全且不阻塞太久
 *   - 内存生命周期由客户端管理，通常在取消订阅或断开连接时释放
 *
 * @see mqtt_list_t, message_handler_t, mqtt_msg_handler_create()
 */
typedef struct message_handlers {
    mqtt_list_t         list;            ///< 链表节点，用于将多个处理器链接在一起
    mqtt_qos_t          qos;             ///< 订阅时请求的 QoS 级别（实际 QoS 以 Broker 返回为准）
    const char*         topic_filter;    ///< 主题过滤器（字符串常量指针，如 "sensor/+/temp"）
    message_handler_t   handler;         ///< 消息到达时调用的回调函数指针
} message_handlers_t;

/**
 * @brief MQTT 确认（ACK）处理句柄结构体
 *
 * 该结构体用于跟踪需要远程确认的 MQTT 报文（如 QoS1 消息、SUBSCRIBE 等）。
 * 在发送此类报文后，客户端会创建一个 ack_handlers_t 节点并插入待确认列表，
 * 启动超时定时器，等待来自 Broker 的确认（PUBACK、SUBACK 等）。若超时未收到确认，
 * 将触发重传机制或错误回调。
 *
 * 此结构通常作为节点挂载在双向链表上，由客户端统一管理所有待确认请求。
 *
 * @note
 *   - 必须在收到对应 ACK 报文或超时后及时释放资源，防止内存泄漏
 *   - payload 可能为 NULL（如 PUBLISH 不带载荷，但 SUBSCRIBE 需要）
 *   - packet_id 必须全局唯一（在当前会话中）
 *   - timer 用于实现超时重传机制
 *
 * @see mqtt_list_t, platform_timer_t, message_handlers_t
 */
typedef struct ack_handlers {
    mqtt_list_t         list;            ///< 链表节点，用于将多个 ACK 处理器链接成待确认列表
    platform_timer_t    timer;           ///< 超时定时器，用于控制重传或失败判定
    uint32_t            type;            ///< 报文类型（如 PUBACK_TYPE、SUBACK_TYPE 等），用于匹配响应
    uint16_t            packet_id;       ///< 报文标识符（Packet ID），用于匹配请求与响应
    message_handlers_t  *handler;        ///< 关联的消息处理器（主要用于 SUBSCRIBE/UNSUBSCRIBE）
    uint16_t            payload_len;     ///< 有效载荷长度（用于重传时重新发送原始数据）
    uint8_t             *payload;        ///< 指向原始报文有效载荷的指针（用于重传）
} ack_handlers_t;

/**
 * @brief MQTT 遗嘱（Will）消息配置选项
 *
 * 该结构体用于设置客户端在意外断开连接时，由 Broker 代为发布的“遗嘱消息”。
 * 遗嘱机制是 MQTT 协议的重要特性，用于通知其他客户端该设备已离线或异常终止。
 *
 * 客户端在 CONNECT 报文中发送遗嘱信息，Broker 会在以下情况发布该消息：
 *   - 客户端未发送 DISCONNECT 报文即断开连接（TCP 连接关闭）
 *   - Keep-Alive 超时
 *
 * @note
 *   - 遗嘱消息的主题和内容在 CONNECT 时发送给 Broker，后续修改需重新连接
 *   - will_topic 和 will_message 字符串必须在连接建立期间保持有效
 *   - 若不使用遗嘱功能，应将 will_topic 设为 NULL
 *
 * @see mqtt_connect(), mqtt_client_t
 */
typedef struct mqtt_will_options {
    mqtt_qos_t          will_qos;        ///< 遗嘱消息的 QoS 等级（QOS0、QOS1、QOS2）
    uint8_t             will_retained;   ///< 遗嘱消息是否为保留消息（1 = 是，0 = 否）
    char                *will_topic;     ///< 遗嘱消息的主题名称（如 "device/status"）
    char                *will_message;   ///< 遗嘱消息的内容（如 "offline"）
} mqtt_will_options_t;

/**
 * @brief MQTT 客户端实例结构体
 *
 * 该结构体代表一个 MQTT 客户端实例，封装了连接参数、运行状态、网络资源、
 * 消息处理机制、重连策略以及多线程/定时器同步原语。它是整个 MQTT 协议栈的核心，
 * 所有操作（连接、发布、订阅、接收）均基于此结构体进行。
 *
 * @note
 *   - 必须在使用前通过 mqtt_client_init() 或类似函数正确初始化
 *   - 多线程环境下访问需加锁（如 mqtt_global_lock）
 *   - 结构体内存通常由调用方分配（静态或堆），生命周期应长于客户端运行时间
 *
 * @see mqtt_connect(), mqtt_disconnect(), mqtt_publish(), mqtt_subscribe()
 */
typedef struct mqtt_client {
    char                        *mqtt_client_id;           ///< 客户端标识符（Client ID），用于唯一标识设备
    char                        *mqtt_user_name;           ///< 用户名（可选认证）
    char                        *mqtt_password;            ///< 密码（可选认证）
    char                        *mqtt_host;                ///< Broker 主机地址（IP 或域名）
    char                        *mqtt_port;                ///< Broker 端口号（通常为 1883 或 8883）
    char                        *mqtt_ca;                  ///< CA 证书路径或内容（用于 TLS 连接）
    void                        *mqtt_reconnect_data;      ///< 重连时传递给回调函数的用户数据

    uint8_t                     *mqtt_read_buf;            ///< 接收缓冲区，用于存储从网络读取的原始 MQTT 报文
    uint8_t                     *mqtt_write_buf;           ///< 发送缓冲区，用于序列化 MQTT 报文后再发送
    uint16_t                    mqtt_keep_alive_interval;  ///< Keep-Alive 间隔（秒），用于检测连接健康状态
    uint16_t                    mqtt_packet_id;            ///< 当前使用的报文 ID（Packet ID），用于 QoS1+ 报文匹配
    uint32_t                    mqtt_will_flag          : 1;  ///< 遗嘱标志（1=启用遗嘱消息）
    uint32_t                    mqtt_clean_session      : 1;  ///< Clean Session 标志（1=连接时清除会话状态）
    uint32_t                    mqtt_ping_outstanding   : 2;  ///< PING 请求未确认计数（防止重复发送 PINGREQ）
    uint32_t                    mqtt_version            : 4;  ///< MQTT 协议版本（如 3=MQTT v3.1.1, 5=MQTT v5.0）
    uint32_t                    mqtt_ack_handler_number : 24; ///< 当前待确认（ACK）处理器数量（用于限制并发）
    uint32_t                    mqtt_cmd_timeout;           ///< 命令超时时间（毫秒），用于网络读写、等待 ACK 等
    uint32_t                    mqtt_read_buf_size;         ///< 接收缓冲区大小（字节）
    uint32_t                    mqtt_write_buf_size;        ///< 发送缓冲区大小（字节）
    uint32_t                    mqtt_reconnect_try_duration;///< 重连尝试总时长上限（毫秒）
    size_t                      mqtt_client_id_len;         ///< 客户端 ID 字符串长度（缓存，避免重复计算）
    size_t                      mqtt_user_name_len;         ///< 用户名长度
    size_t                      mqtt_password_len;          ///< 密码长度

    mqtt_will_options_t         *mqtt_will_options;         ///< 指向遗嘱消息配置的指针（可为 NULL 表示无遗嘱）
    client_state_t              mqtt_client_state;          ///< 当前客户端状态（如 CONNECTING, CONNECTED, DISCONNECTED）

    platform_mutex_t            mqtt_write_lock;            ///< 写操作互斥锁，防止多线程并发写网络
    platform_mutex_t            mqtt_global_lock;           ///< 全局锁，保护客户端内部状态一致性

    mqtt_list_t                 mqtt_msg_handler_list;      ///< 消息处理器链表：存储所有订阅主题及其回调函数
    mqtt_list_t                 mqtt_ack_handler_list;      ///< ACK 处理器链表：管理待确认的 QoS1+ 报文（PUB/SUB/UNSUB）

    network_t                   *mqtt_network;              ///< 网络接口抽象层指针（TCP/TLS 实现）
    platform_thread_t           *mqtt_thread;               ///< 后台工作线程指针（运行 mqtt_yield_thread）
    platform_timer_t            mqtt_last_sent;             ///< 最后一次发送数据的时间戳（用于 Keep-Alive 判断）
    platform_timer_t            mqtt_last_received;         ///< 最后一次接收到数据的时间戳（用于 Keep-Alive 判断）

    reconnect_handler_t         mqtt_reconnect_handler;     ///< 重连成功后的回调函数（通知上层）
    interceptor_handler_t       mqtt_interceptor_handler;   ///< 消息拦截器（可在发送/接收前修改或记录消息）

} mqtt_client_t;


/**
 * @brief 健壮性检查宏：用于验证指针或条件的有效性
 *
 * 该宏用于在函数入口处对关键参数（如指针）进行有效性检查。
 * 如果检查失败（即表达式为假），则输出错误日志，并立即返回指定的错误码，
 * 防止后续空指针解引用或非法操作，提升系统的稳定性和可调试性。
 *
 * @param item  要检查的表达式（通常为指针，如 c、buf 等）
 *              若为 NULL 或 false，则触发错误处理
 * @param err   检查失败时返回的错误值（如 0, NULL, -1 等）
 *              类型应与函数返回值匹配
 *
 * @note
 *   - 常用于函数开头进行参数校验
 *   - 依赖 MQTT_LOG_E 宏进行日志输出（需提前定义）
 *   - 使用 `return` 语句，因此只能在函数中使用，不能在独立作用域块中单独使用
 *   - 日志包含文件名、行号和函数名，便于定位问题
 *
 * @see MQTT_CLIENT_SET_DEFINE, MQTT_LOG_E
 *
 * @example
 *   void* my_func(mqtt_client_t *c) {
 *       MQTT_ROBUSTNESS_CHECK(c, NULL);  // 若 c 为 NULL，输出日志并返回 NULL
 *       // 正常逻辑...
 *   }
 */
#define MQTT_ROBUSTNESS_CHECK(item, err)                         \
    if (!(item)) {                                               \
        MQTT_LOG_E("%s:%d %s()... check for error.",             \
                   __FILE__, __LINE__, __FUNCTION__);            \
        return err;                                              \
    }

/**
 * @brief MQTT 客户端属性设置宏（生成类型安全的 setter 函数）
 *
 * 该宏用于为 mqtt_client_t 结构体中的字段自动生成类型安全的设置函数（setter）。
 * 它通过宏展开生成一个独立的函数，用于设置客户端实例的某个属性值，并包含空指针健壮性检查。
 *
 * 使用此宏可以避免重复编写简单的赋值函数，同时保证：
 *   - 类型安全（函数参数与返回值为指定 type）
 *   - 健壮性检查（防止传入 NULL 客户端指针）
 *   - 一致性命名（函数名为 mqtt_set_xxx）
 *   - 可链式调用（返回设置后的值）
 *
 * @param name      mqtt_client_t 中字段的“名称后缀”（不含前缀 mqtt_）
 *                  例如：若字段为 mqtt_keep_alive_interval，则 name = keep_alive_interval
 *
 * @param type      该字段的数据类型（如 uint16_t, char*, mqtt_qos_t 等）
 *
 * @param res       错误返回值（当 c == NULL 时返回此值）
 *                  通常与 type 匹配，如 0、NULL 等
 *
 * @return          成功时返回设置后的属性值；失败时（c 为 NULL）返回 res
 *
 * @note
 *   - 生成的函数名格式为：mqtt_set_<name>
 *   - 必须确保 c 指向有效的 mqtt_client_t 实例
 *   - 不会复制字符串或复杂数据，仅进行指针或值赋值
 *
 * @see MQTT_ROBUSTNESS_CHECK
 *
 * @example
 *   // 定义一个设置 keep_alive 的函数
 *   MQTT_CLIENT_SET_DEFINE(keep_alive_interval, uint16_t, 0)
 *
 *   // 展开后等价于：
 *   // uint16_t mqtt_set_keep_alive_interval(mqtt_client_t *c, uint16_t t) {
 *   //     MQTT_ROBUSTNESS_CHECK((c), 0);
 *   //     c->mqtt_keep_alive_interval = t;
 *   //     return c->mqtt_keep_alive_interval;
 *   // }
 */
#define MQTT_CLIENT_SET_DEFINE(name, type, res)         \
    type mqtt_set_##name(mqtt_client_t *c, type t) {    \
        MQTT_ROBUSTNESS_CHECK((c), res);                \
        c->mqtt_##name = t;                             \
        return c->mqtt_##name;                          \
    }

#define MQTT_CLIENT_SET_STATEMENT(name, type)           \
    type mqtt_set_##name(mqtt_client_t *, type);

MQTT_CLIENT_SET_STATEMENT(client_id, char*)
MQTT_CLIENT_SET_STATEMENT(user_name, char*)
MQTT_CLIENT_SET_STATEMENT(password, char*)
MQTT_CLIENT_SET_STATEMENT(host, char*)
MQTT_CLIENT_SET_STATEMENT(port, char*)
MQTT_CLIENT_SET_STATEMENT(ca, char*)
MQTT_CLIENT_SET_STATEMENT(reconnect_data, void*)
MQTT_CLIENT_SET_STATEMENT(keep_alive_interval, uint16_t)
MQTT_CLIENT_SET_STATEMENT(will_flag, uint32_t)
MQTT_CLIENT_SET_STATEMENT(clean_session, uint32_t)
MQTT_CLIENT_SET_STATEMENT(version, uint32_t)
MQTT_CLIENT_SET_STATEMENT(cmd_timeout, uint32_t)
MQTT_CLIENT_SET_STATEMENT(read_buf_size, uint32_t)
MQTT_CLIENT_SET_STATEMENT(write_buf_size, uint32_t)
MQTT_CLIENT_SET_STATEMENT(reconnect_try_duration, uint32_t)
MQTT_CLIENT_SET_STATEMENT(reconnect_handler, reconnect_handler_t)
MQTT_CLIENT_SET_STATEMENT(interceptor_handler, interceptor_handler_t)

void mqtt_sleep_ms(int ms);
mqtt_client_t *mqtt_lease(void);
int mqtt_release(mqtt_client_t* c);
int mqtt_connect(mqtt_client_t* c);
int mqtt_disconnect(mqtt_client_t* c);
int mqtt_keep_alive(mqtt_client_t* c);
int mqtt_subscribe(mqtt_client_t* c, const char* topic_filter, mqtt_qos_t qos, message_handler_t msg_handler);
int mqtt_unsubscribe(mqtt_client_t* c, const char* topic_filter);
int mqtt_publish(mqtt_client_t* c, const char* topic_filter, mqtt_message_t* msg);
int mqtt_list_subscribe_topic(mqtt_client_t* c);
int mqtt_set_will_options(mqtt_client_t* c, char *topic, mqtt_qos_t qos, uint8_t retained, char *message);

#ifdef __cplusplus
}
#endif

#endif /* _MQTTCLIENT_H_ */
