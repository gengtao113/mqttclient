/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-09 21:31:25
 * @LastEditTime : 2023-03-26 17:18:35
 * @Description: the code belongs to jiejie, please keep the author information and source code according to the license.
 */
#include "mqttclient.h"

#define     MQTT_MIN_PAYLOAD_SIZE   2               
#define     MQTT_MAX_PAYLOAD_SIZE   268435455       // MQTT imposes a maximum payload size of 268435455 bytes.

static void default_msg_handler(void* client, message_data_t* msg)
{
    MQTT_LOG_I("%s:%d %s()...\ntopic: %s, qos: %d, \nmessage:%s", __FILE__, __LINE__, __FUNCTION__, 
            msg->topic_name, msg->message->qos, (char*)msg->message->payload);
}

static client_state_t mqtt_get_client_state(mqtt_client_t* c)
{
    return c->mqtt_client_state;
}

static void mqtt_set_client_state(mqtt_client_t* c, client_state_t state)
{
    platform_mutex_lock(&c->mqtt_global_lock);
    c->mqtt_client_state = state;
    platform_mutex_unlock(&c->mqtt_global_lock);
}

static int mqtt_is_connected(mqtt_client_t* c)
{
    client_state_t state;

    state = mqtt_get_client_state(c);
    if (CLIENT_STATE_CLEAN_SESSION ==  state) {
        RETURN_ERROR(MQTT_CLEAN_SESSION_ERROR);
    } else if (CLIENT_STATE_CONNECTED != state) {
        RETURN_ERROR(MQTT_NOT_CONNECT_ERROR);
    }
    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}

static int mqtt_set_publish_dup(mqtt_client_t* c, uint8_t dup)
{
    uint8_t *read_data = c->mqtt_write_buf;
    uint8_t *write_data = c->mqtt_write_buf;
    MQTTHeader header = {0};

    if (NULL == c->mqtt_write_buf)
        RETURN_ERROR(MQTT_SET_PUBLISH_DUP_FAILED_ERROR);

    header.byte = readChar(&read_data); /* read header */

    if (header.bits.type != PUBLISH)
        RETURN_ERROR(MQTT_SET_PUBLISH_DUP_FAILED_ERROR);
    
    header.bits.dup = dup;
    writeChar(&write_data, header.byte); /* write header */

    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}

static int mqtt_ack_handler_is_maximum(mqtt_client_t* c)
{
    return (c->mqtt_ack_handler_number >= MQTT_ACK_HANDLER_NUM_MAX) ? 1 : 0;
}

static void mqtt_add_ack_handler_num(mqtt_client_t* c)
{
    platform_mutex_lock(&c->mqtt_global_lock);
    c->mqtt_ack_handler_number++;
    platform_mutex_unlock(&c->mqtt_global_lock);
}

static int mqtt_subtract_ack_handler_num(mqtt_client_t* c)
{
    int rc = MQTT_SUCCESS_ERROR;
    platform_mutex_lock(&c->mqtt_global_lock);
    if (c->mqtt_ack_handler_number <= 0) {
        goto exit;
    }
    
    c->mqtt_ack_handler_number--;
    
exit:
    platform_mutex_unlock(&c->mqtt_global_lock);
    RETURN_ERROR(rc);
}

static uint16_t mqtt_get_next_packet_id(mqtt_client_t *c) 
{
    platform_mutex_lock(&c->mqtt_global_lock);
    c->mqtt_packet_id = (c->mqtt_packet_id == MQTT_MAX_PACKET_ID) ? 1 : c->mqtt_packet_id + 1;
    platform_mutex_unlock(&c->mqtt_global_lock);
    return c->mqtt_packet_id;
}

static int mqtt_decode_packet(mqtt_client_t* c, int* value, int timeout)
{
    uint8_t i;
    int multiplier = 1;
    int len = 0;
    const int MAX_NO_OF_REMAINING_LENGTH_BYTES = 4;

    *value = 0;
    do {
        int rc = MQTTPACKET_READ_ERROR;

        if (++len > MAX_NO_OF_REMAINING_LENGTH_BYTES) {
            rc = MQTTPACKET_READ_ERROR; /* bad data */
            goto exit;
        }
        rc = network_read(c->mqtt_network, &i, 1, timeout);  /* read network data */
        if (rc != 1)
            goto exit;
        *value += (i & 127) * multiplier;   /* decode data length according to mqtt protocol */
        multiplier *= 128;
    } while ((i & 128) != 0);
exit:
    return len;
}

static void mqtt_packet_drain(mqtt_client_t* c, platform_timer_t *timer, int packet_len)
{
    int total_bytes_read = 0, read_len = 0, bytes2read = 0;

    if (packet_len < c->mqtt_read_buf_size) {
        bytes2read = packet_len;
    } else {
        bytes2read = c->mqtt_read_buf_size;
    }

    do {
        read_len = network_read(c->mqtt_network, c->mqtt_read_buf, bytes2read, platform_timer_remain(timer));
        if (0 != read_len) {
            total_bytes_read += read_len;
            if ((packet_len - total_bytes_read) >= c->mqtt_read_buf_size) {
                bytes2read = c->mqtt_read_buf_size;
            } else {
                bytes2read = packet_len - total_bytes_read;
            }
        }
    } while ((total_bytes_read < packet_len) && (0 != read_len));   /* read and discard all corrupted data */
}

static int mqtt_read_packet(mqtt_client_t* c, int* packet_type, platform_timer_t* timer)
{
    MQTTHeader header = {0};
    int rc;
    int len = 1;
    int remain_len = 0;
    
    if (NULL == packet_type)
        RETURN_ERROR(MQTT_NULL_VALUE_ERROR);

    platform_timer_init(timer);
    platform_timer_cutdown(timer, c->mqtt_cmd_timeout);

    /* 1. read the header byte.  This has the packet type in it */
    rc = network_read(c->mqtt_network, c->mqtt_read_buf, len, platform_timer_remain(timer));
    if (rc != len)
        RETURN_ERROR(MQTT_NOTHING_TO_READ_ERROR);

    /* 2. read the remaining length.  This is variable in itself */
    mqtt_decode_packet(c, &remain_len, platform_timer_remain(timer));

    /* put the original remaining length back into the buffer */
    len += MQTTPacket_encode(c->mqtt_read_buf + len, remain_len); 

    if ((len + remain_len) > c->mqtt_read_buf_size) {
        
        /* mqtt buffer is too short, read and discard all corrupted data */
        mqtt_packet_drain(c, timer, remain_len);

        RETURN_ERROR(MQTT_BUFFER_TOO_SHORT_ERROR);
    }

    /* 3. read the rest of the buffer using a callback to supply the rest of the data */
    if ((remain_len > 0) && ((rc = network_read(c->mqtt_network, c->mqtt_read_buf + len, remain_len, platform_timer_remain(timer))) != remain_len))
        RETURN_ERROR(MQTT_NOTHING_TO_READ_ERROR);

    header.byte = c->mqtt_read_buf[0];
    *packet_type = header.bits.type;
    
    platform_timer_cutdown(&c->mqtt_last_received, (c->mqtt_keep_alive_interval * 1000)); 

    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}

/**
 * @brief 以阻塞方式发送 MQTT 报文数据
 *
 * 该函数将已序列化在写缓冲区中的 MQTT 报文通过底层网络接口发送出去。
 * 发送过程是阻塞的，但受超时时间限制（由 c->mqtt_cmd_timeout 指定）。
 * 若在超时前成功发送全部数据，则更新最后发送时间戳；否则返回错误。
 *
 * @param[in]  c       指向 MQTT 客户端实例
 * @param[in]  length  待发送数据的总长度（字节）
 * @param[out] timer   用于控制本次发送操作的超时定时器（临时使用）
 *
 * @return
 *   - MQTT_SUCCESS_ERROR (0): 数据全部成功发送
 *   - MQTT_SEND_PACKET_ERROR: 发送失败（网络错误或超时）
 *
 * @note
 *   - 本函数使用 platform_timer 实现发送超时控制，防止永久阻塞。
 *   - 发送过程中会分片写入（如 TCP 缓冲区满），通过循环重试完成。
 *   - 成功发送后会刷新 mqtt_last_sent 定时器，用于 Keep-Alive 机制。
 *   - 调用前必须持有 mqtt_write_lock，确保写操作互斥。
 *
 * @see network_write(), platform_timer_is_expired(), platform_timer_remain()
 */
static int mqtt_send_packet(mqtt_client_t* c, int length, platform_timer_t* timer)
{
    int len = 0;   // 每次 network_write 实际写入的字节数
    int sent = 0;  // 累计已发送的字节数

    // 初始化定时器，并设置超时时间为 c->mqtt_cmd_timeout（单位：毫秒）
    platform_timer_init(timer);
    platform_timer_cutdown(timer, c->mqtt_cmd_timeout);

    /* 循环发送数据，直到全部发送完成或超时 */
    while ((sent < length) && (!platform_timer_is_expired(timer))) {
        // 调用底层网络接口发送剩余未发送的数据
        // 从 c->mqtt_write_buf + sent 偏移处开始，尝试发送 (length - sent) 字节
        // 平台层可根据剩余时间进行阻塞或非阻塞写操作
        len = network_write(c->mqtt_network, 
                            &c->mqtt_write_buf[sent], 
                            length - sent, 
                            platform_timer_remain(timer));

        // 如果写入失败（返回值 <= 0），跳出循环
        if (len <= 0) {
            break;  // 网络错误或连接断开
        }

        sent += len;  // 更新已发送字节数
    }

    // 判断是否成功发送了全部数据
    if (sent == length) {
        // 发送成功：重置客户端“最后发送时间”定时器
        // 用于后续的 Keep-Alive（PING）机制判断是否需要发送 PINGREQ
        platform_timer_cutdown(&c->mqtt_last_sent, (c->mqtt_keep_alive_interval * 1000));

        RETURN_ERROR(MQTT_SUCCESS_ERROR);  // 返回成功
    }
    
    // 发送失败：未发送完全部数据（超时或网络错误）
    RETURN_ERROR(MQTT_SEND_PACKET_ERROR);
}

static int mqtt_is_topic_equals(const char *topic_filter, const char *topic)
{
    int topic_len = 0;
    
    topic_len = strlen(topic);
    if (strlen(topic_filter) != topic_len) {
        return 0;
    }

    if (strncmp(topic_filter, topic, topic_len) == 0) {
        return 1;
    }

    return 0;
}

static char mqtt_topic_is_matched(char* topic_filter, MQTTString* topic_name)
{
    char* curf = topic_filter;
    char* curn = topic_name->lenstring.data;
    char* curn_end = curn + topic_name->lenstring.len;

    while (*curf && curn < curn_end)
    {
        if (*curn == '/' && *curf != '/')
            break;
        
        /* support wildcards for MQTT topics, such as '#' '+' */
        if (*curf != '+' && *curf != '#' && *curf != *curn) 
            break;
        
        if (*curf == '+') {
            char* nextpos = curn + 1;
            while (nextpos < curn_end && *nextpos != '/')
                nextpos = ++curn + 1;
        }
        else if (*curf == '#')
            curn = curn_end - 1;
        curf++;
        curn++;
    };

    return (curn == curn_end) && (*curf == '\0');
}

static void mqtt_new_message_data(message_data_t* md, MQTTString* topic_name, mqtt_message_t* message)
{
    int len;
    len = (topic_name->lenstring.len < MQTT_TOPIC_LEN_MAX - 1) ? topic_name->lenstring.len : MQTT_TOPIC_LEN_MAX - 1;
    memcpy(md->topic_name, topic_name->lenstring.data, len);
    md->topic_name[len] = '\0';     /* the topic name is too long and will be truncated */
    md->message = message;
}

static message_handlers_t *mqtt_get_msg_handler(mqtt_client_t* c, MQTTString* topic_name)
{
    mqtt_list_t *curr, *next;
    message_handlers_t *msg_handler;

    /* traverse the msg_handler_list to find the matching message handler */
    LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_msg_handler_list) {
        msg_handler = LIST_ENTRY(curr, message_handlers_t, list);

        /* judge topic is equal or match, support wildcard, such as '#' '+' */
        if ((NULL != msg_handler->topic_filter) && ((MQTTPacket_equals(topic_name, (char*)msg_handler->topic_filter)) || 
            (mqtt_topic_is_matched((char*)msg_handler->topic_filter, topic_name)))) {
                return msg_handler;
            }
    }
    return NULL;
}

static int mqtt_deliver_message(mqtt_client_t* c, MQTTString* topic_name, mqtt_message_t* message)
{
    int rc = MQTT_FAILED_ERROR;
    message_handlers_t *msg_handler;
    
    /* get mqtt message handler */
    msg_handler = mqtt_get_msg_handler(c, topic_name);
    
    if (NULL != msg_handler) {
        message_data_t md;
        mqtt_new_message_data(&md, topic_name, message);    /* make a message data */
        msg_handler->handler(c, &md);       /* deliver the message */
        rc = MQTT_SUCCESS_ERROR;
    } else if (NULL != c->mqtt_interceptor_handler) {
        message_data_t md;
        mqtt_new_message_data(&md, topic_name, message);    /* make a message data */
        c->mqtt_interceptor_handler(c, &md);
        rc = MQTT_SUCCESS_ERROR;
    }
    
    memset(message->payload, 0, strlen(message->payload));
    memset(topic_name->lenstring.data, 0, topic_name->lenstring.len);

    RETURN_ERROR(rc);
}

static ack_handlers_t *mqtt_ack_handler_create(mqtt_client_t* c, int type, uint16_t packet_id, uint16_t payload_len, message_handlers_t* handler)
{
    ack_handlers_t *ack_handler = NULL;

    ack_handler = (ack_handlers_t *) platform_memory_alloc(sizeof(ack_handlers_t) + payload_len);
    if (NULL == ack_handler)
        return NULL;

    mqtt_list_init(&ack_handler->list);
    platform_timer_init(&ack_handler->timer);
    platform_timer_cutdown(&ack_handler->timer, c->mqtt_cmd_timeout);    /* No response within timeout will be destroyed or resent */

    ack_handler->type = type;
    ack_handler->packet_id = packet_id;
    ack_handler->payload_len = payload_len;
    ack_handler->payload = (uint8_t *)ack_handler + sizeof(ack_handlers_t);
    ack_handler->handler = handler;
    memcpy(ack_handler->payload, c->mqtt_write_buf, payload_len);    /* save the data in ack handler*/
    
    return ack_handler;
}

static void mqtt_ack_handler_destroy(ack_handlers_t* ack_handler)
{ 
    if (NULL != &ack_handler->list) {
        mqtt_list_del(&ack_handler->list);
        platform_memory_free(ack_handler);  /* delete ack handler from the list, and free memory */
    }
}

static void mqtt_ack_handler_resend(mqtt_client_t* c, ack_handlers_t* ack_handler)
{ 
    platform_timer_t timer;
    platform_timer_init(&timer);
    platform_timer_cutdown(&timer, c->mqtt_cmd_timeout);
    platform_timer_cutdown(&ack_handler->timer, c->mqtt_cmd_timeout); /* timeout, recutdown */

    platform_mutex_lock(&c->mqtt_write_lock);
    memcpy(c->mqtt_write_buf, ack_handler->payload, ack_handler->payload_len);   /* copy data to write buf form ack handler */
    
    mqtt_send_packet(c, ack_handler->payload_len, &timer);      /* resend data */
    platform_mutex_unlock(&c->mqtt_write_lock);
    MQTT_LOG_W("%s:%d %s()... resend %d package, packet_id is %d ", __FILE__, __LINE__, __FUNCTION__, ack_handler->type, ack_handler->packet_id);
}

static int mqtt_ack_list_node_is_exist(mqtt_client_t* c, int type, uint16_t packet_id)
{
    mqtt_list_t *curr, *next;
    ack_handlers_t *ack_handler;

    if (mqtt_list_is_empty(&c->mqtt_ack_handler_list))
        return 0;

    LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_ack_handler_list) {
        ack_handler = LIST_ENTRY(curr, ack_handlers_t, list);

         /* For mqtt packets of qos1 and qos2, you can use the packet id and type as the unique
            identifier to determine whether the node already exists and avoid repeated addition. */
        if ((packet_id  == ack_handler->packet_id) && (type == ack_handler->type))     
            return 1;
    }
    
    return 0;
}

static int mqtt_ack_list_record(mqtt_client_t* c, int type, uint16_t packet_id, uint16_t payload_len, message_handlers_t* handler)
{
    int rc = MQTT_SUCCESS_ERROR;
    ack_handlers_t *ack_handler = NULL;
    
    /* Determine if the node already exists */
    if (mqtt_ack_list_node_is_exist(c, type, packet_id))
        RETURN_ERROR(MQTT_ACK_NODE_IS_EXIST_ERROR);

    /* create a ack handler node */
    ack_handler = mqtt_ack_handler_create(c, type, packet_id, payload_len, handler);
    if (NULL == ack_handler)
        RETURN_ERROR(MQTT_MEM_NOT_ENOUGH_ERROR);

    mqtt_add_ack_handler_num(c);

    mqtt_list_add_tail(&ack_handler->list, &c->mqtt_ack_handler_list);

    RETURN_ERROR(rc);
}

static int mqtt_ack_list_unrecord(mqtt_client_t* c, int type, uint16_t packet_id, message_handlers_t **handler)
{
    mqtt_list_t *curr, *next;
    ack_handlers_t *ack_handler;

    if (mqtt_list_is_empty(&c->mqtt_ack_handler_list))
        RETURN_ERROR(MQTT_SUCCESS_ERROR);

    LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_ack_handler_list) {
        ack_handler = LIST_ENTRY(curr, ack_handlers_t, list);

        if ((packet_id != ack_handler->packet_id) || (type != ack_handler->type))
            continue;

        if (handler)
            *handler = ack_handler->handler;
        
        /* destroy a ack handler node */
        mqtt_ack_handler_destroy(ack_handler);
        mqtt_subtract_ack_handler_num(c);
    }
    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}


static message_handlers_t *mqtt_msg_handler_create(const char* topic_filter, mqtt_qos_t qos, message_handler_t handler)
{
    message_handlers_t *msg_handler = NULL;

    msg_handler = (message_handlers_t *) platform_memory_alloc(sizeof(message_handlers_t));
    if (NULL == msg_handler)
        return NULL;
    
    mqtt_list_init(&msg_handler->list);
    
    msg_handler->qos = qos;
    msg_handler->handler = handler;     /* register  callback handler */
    msg_handler->topic_filter = topic_filter;

    return msg_handler;
}

static void mqtt_msg_handler_destory(message_handlers_t *msg_handler)
{
    if (NULL != &msg_handler->list) {
        mqtt_list_del(&msg_handler->list);
        platform_memory_free(msg_handler);
    }
}

static int mqtt_msg_handler_is_exist(mqtt_client_t* c, message_handlers_t *handler)
{
    mqtt_list_t *curr, *next;
    message_handlers_t *msg_handler;

    if (mqtt_list_is_empty(&c->mqtt_msg_handler_list))
        return 0;

    LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_msg_handler_list) {
        msg_handler = LIST_ENTRY(curr, message_handlers_t, list);

        /* determine whether a node already exists by mqtt topic, but wildcards are not supported */
        if ((NULL != msg_handler->topic_filter) && (mqtt_is_topic_equals(msg_handler->topic_filter, handler->topic_filter))) {
            MQTT_LOG_W("%s:%d %s()...msg_handler->topic_filter: %s, handler->topic_filter: %s", 
                        __FILE__, __LINE__, __FUNCTION__, msg_handler->topic_filter, handler->topic_filter);
            return 1;
        }
    }
    
    return 0;
}

static int mqtt_msg_handlers_install(mqtt_client_t* c, message_handlers_t *handler)
{
    if ((NULL == c) || (NULL == handler))
        RETURN_ERROR(MQTT_NULL_VALUE_ERROR);
    
    if (mqtt_msg_handler_is_exist(c, handler)) {
        mqtt_msg_handler_destory(handler);
        RETURN_ERROR(MQTT_SUCCESS_ERROR);
    }

    /* install to  msg_handler_list*/
    mqtt_list_add_tail(&handler->list, &c->mqtt_msg_handler_list);

    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}


static void mqtt_clean_session(mqtt_client_t* c)
{
    mqtt_list_t *curr, *next;
    ack_handlers_t *ack_handler;
    message_handlers_t *msg_handler;
    
    /* release all ack_handler_list memory */
    if (!(mqtt_list_is_empty(&c->mqtt_ack_handler_list))) {
        LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_ack_handler_list) {
            ack_handler = LIST_ENTRY(curr, ack_handlers_t, list);
            mqtt_list_del(&ack_handler->list);
            //@lchnu, 2020-10-08, avoid socket disconnet when waiting for suback/unsuback....
            if(NULL != ack_handler->handler) {
              mqtt_msg_handler_destory(ack_handler->handler);
              ack_handler->handler = NULL;
            }
            platform_memory_free(ack_handler);
        }
        mqtt_list_del_init(&c->mqtt_ack_handler_list);
    }
    /* need clean mqtt_ack_handler_number value, find the bug by @lchnu */
    c->mqtt_ack_handler_number = 0;

    /* release all msg_handler_list memory */
    if (!(mqtt_list_is_empty(&c->mqtt_msg_handler_list))) {
        LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_msg_handler_list) {
            msg_handler = LIST_ENTRY(curr, message_handlers_t, list);
            mqtt_list_del(&msg_handler->list);
            msg_handler->topic_filter = NULL;
            platform_memory_free(msg_handler);
        }
        // MQTT_LOG_D("%s:%d %s() mqtt_msg_handler_list delete", __FILE__, __LINE__, __FUNCTION__);
        mqtt_list_del_init(&c->mqtt_msg_handler_list);
    }

    mqtt_set_client_state(c, CLIENT_STATE_INVALID);
}


/**
 * see if there is a message waiting for the server to answer in the ack list, if there is, then process it according to the flag.
 * flag : 0 means it does not need to wait for the timeout to process these packets immediately. usually immediately after reconnecting.
 *        1 means it needs to wait for timeout before processing these messages, usually timeout processing in a stable connection.
 */
static void mqtt_ack_list_scan(mqtt_client_t* c, uint8_t flag)
{
    mqtt_list_t *curr, *next;
    ack_handlers_t *ack_handler;

    if ((mqtt_list_is_empty(&c->mqtt_ack_handler_list)) || (CLIENT_STATE_CONNECTED != mqtt_get_client_state(c)))
        return;

    LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_ack_handler_list) {
        ack_handler = LIST_ENTRY(curr, ack_handlers_t, list);
        
        if ((!platform_timer_is_expired(&ack_handler->timer)) && (flag == 1))
            continue;
        
        if ((ack_handler->type ==  PUBACK) || (ack_handler->type ==  PUBREC) || (ack_handler->type ==  PUBREL) || (ack_handler->type ==  PUBCOMP)) {
            
            /* timeout has occurred. for qos1 and qos2 packets, you need to resend them. */
            mqtt_ack_handler_resend(c, ack_handler);
            continue;
        } else if ((ack_handler->type == SUBACK) || (ack_handler->type == UNSUBACK)) {
            
            /*@lchnu, 2020-10-08, destory handler memory, if suback/unsuback is overdue!*/
            if (NULL != ack_handler->handler) {
                mqtt_msg_handler_destory(ack_handler->handler);
                ack_handler->handler = NULL;
            }
        }
        /* if it is not a qos1 or qos2 message, it will be destroyed in every processing */
        mqtt_ack_handler_destroy(ack_handler);
        mqtt_subtract_ack_handler_num(c); /*@lchnu, 2020-10-08 */
    }
}

static int mqtt_try_resubscribe(mqtt_client_t* c)
{
    int rc = MQTT_RESUBSCRIBE_ERROR;
    mqtt_list_t *curr, *next;
    message_handlers_t *msg_handler;

    MQTT_LOG_W("%s:%d %s()... mqtt try resubscribe ...", __FILE__, __LINE__, __FUNCTION__);
    
    if (mqtt_list_is_empty(&c->mqtt_msg_handler_list)) {
        // MQTT_LOG_D("%s:%d %s() mqtt_msg_handler_list is empty", __FILE__, __LINE__, __FUNCTION__);
        RETURN_ERROR(MQTT_SUCCESS_ERROR);
    }
    
    LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_msg_handler_list) {
        msg_handler = LIST_ENTRY(curr, message_handlers_t, list);

        /* resubscribe topic */
        if ((rc = mqtt_subscribe(c, msg_handler->topic_filter, msg_handler->qos, msg_handler->handler)) == MQTT_ACK_HANDLER_NUM_TOO_MUCH_ERROR)
            MQTT_LOG_W("%s:%d %s()... mqtt ack handler num too much ...", __FILE__, __LINE__, __FUNCTION__);

    }

    RETURN_ERROR(rc);
}

static int mqtt_try_do_reconnect(mqtt_client_t* c)
{
    int rc = MQTT_CONNECT_FAILED_ERROR;

    if (CLIENT_STATE_CONNECTED != mqtt_get_client_state(c))
        rc = mqtt_connect(c);       /* reconnect */
    
    if (MQTT_SUCCESS_ERROR == rc) {
        rc = mqtt_try_resubscribe(c);   /* resubscribe */
        /* process these ack messages immediately after reconnecting */
        mqtt_ack_list_scan(c, 0);
    }

    MQTT_LOG_D("%s:%d %s()... mqtt try connect result is -0x%04x", __FILE__, __LINE__, __FUNCTION__, -rc);
    
    RETURN_ERROR(rc);
}

static int mqtt_try_reconnect(mqtt_client_t* c)
{
    int rc = MQTT_SUCCESS_ERROR;

    /*before connect, call reconnect handler, it can used to update the mqtt password, eg: onenet platform need*/
    if (NULL != c->mqtt_reconnect_handler) {
        c->mqtt_reconnect_handler(c, c->mqtt_reconnect_data);
    }

    rc = mqtt_try_do_reconnect(c);

    if(MQTT_SUCCESS_ERROR != rc) {
        /*connect fail must delay reconnect try duration time and let cpu time go out, the lowest priority task can run */
        mqtt_sleep_ms(c->mqtt_reconnect_try_duration);  
        RETURN_ERROR(MQTT_RECONNECT_TIMEOUT_ERROR);    
    }
    
    RETURN_ERROR(rc);
}

static int mqtt_publish_ack_packet(mqtt_client_t *c, uint16_t packet_id, int packet_type)
{
    int len = 0;
    int rc = MQTT_SUCCESS_ERROR;
    platform_timer_t timer;
    platform_timer_init(&timer);
    platform_timer_cutdown(&timer, c->mqtt_cmd_timeout);

    platform_mutex_lock(&c->mqtt_write_lock);

    switch (packet_type) {
        case PUBREC:
            len = MQTTSerialize_ack(c->mqtt_write_buf, c->mqtt_write_buf_size, PUBREL, 0, packet_id); /* make a PUBREL ack packet */
            rc = mqtt_ack_list_record(c, PUBCOMP, packet_id, len, NULL);   /* record ack, expect to receive PUBCOMP*/
            if (MQTT_SUCCESS_ERROR != rc)
                goto exit;
            break;
            
        case PUBREL:
            len = MQTTSerialize_ack(c->mqtt_write_buf, c->mqtt_write_buf_size, PUBCOMP, 0, packet_id); /* make a PUBCOMP ack packet */
            break;
            
        default:
            rc = MQTT_PUBLISH_ACK_TYPE_ERROR;
            goto exit;
    }

    if (len <= 0) {
        rc = MQTT_PUBLISH_ACK_PACKET_ERROR;
        goto exit;
    }

    rc = mqtt_send_packet(c, len, &timer);

exit:
    platform_mutex_unlock(&c->mqtt_write_lock);

    RETURN_ERROR(rc);
}

static int mqtt_puback_and_pubcomp_packet_handle(mqtt_client_t *c, platform_timer_t *timer)
{
    int rc = MQTT_FAILED_ERROR;
    uint16_t packet_id;
    uint8_t dup, packet_type;

    rc = mqtt_is_connected(c);
    if (MQTT_SUCCESS_ERROR != rc)
        RETURN_ERROR(rc);

    if (MQTTDeserialize_ack(&packet_type, &dup, &packet_id, c->mqtt_read_buf, c->mqtt_read_buf_size) != 1)
        rc = MQTT_PUBREC_PACKET_ERROR;
    
    (void) dup;
    rc = mqtt_ack_list_unrecord(c, packet_type, packet_id, NULL);   /* unrecord ack handler */

    RETURN_ERROR(rc);
}

static int mqtt_suback_packet_handle(mqtt_client_t *c, platform_timer_t *timer)
{
    int rc = MQTT_FAILED_ERROR;
    int count = 0;
    int granted_qos = 0;
    uint16_t packet_id;
    int is_nack = 0;
    message_handlers_t *msg_handler = NULL;

    rc = mqtt_is_connected(c);
    if (MQTT_SUCCESS_ERROR != rc)
        RETURN_ERROR(rc);

    /* deserialize subscribe ack packet */
    if (MQTTDeserialize_suback(&packet_id, 1, &count, (int*)&granted_qos, c->mqtt_read_buf, c->mqtt_read_buf_size) != 1) 
        RETURN_ERROR(MQTT_SUBSCRIBE_ACK_PACKET_ERROR);

    is_nack = (granted_qos == SUBFAIL);
    
    rc = mqtt_ack_list_unrecord(c, SUBACK, packet_id, &msg_handler);
    
    if (!msg_handler)
        RETURN_ERROR(MQTT_MEM_NOT_ENOUGH_ERROR);
    
    if (is_nack) {
        mqtt_msg_handler_destory(msg_handler);  /* subscribe topic failed, destory message handler */
        MQTT_LOG_D("subscribe topic failed...");
        RETURN_ERROR(MQTT_SUBSCRIBE_NOT_ACK_ERROR);
    }
    
    rc = mqtt_msg_handlers_install(c, msg_handler);
    
    RETURN_ERROR(rc);
}

static int mqtt_unsuback_packet_handle(mqtt_client_t *c, platform_timer_t *timer)
{
    int rc = MQTT_FAILED_ERROR;
    message_handlers_t *msg_handler;
    uint16_t packet_id = 0;
    
    rc = mqtt_is_connected(c);
    if (MQTT_SUCCESS_ERROR != rc)
        RETURN_ERROR(rc);

    if (MQTTDeserialize_unsuback(&packet_id, c->mqtt_read_buf, c->mqtt_read_buf_size) != 1)
        RETURN_ERROR(MQTT_UNSUBSCRIBE_ACK_PACKET_ERROR);

    rc = mqtt_ack_list_unrecord(c, UNSUBACK, packet_id, &msg_handler);  /* unrecord ack handler, and get message handler */
    
    if (!msg_handler)
        RETURN_ERROR(MQTT_MEM_NOT_ENOUGH_ERROR);
    
    mqtt_msg_handler_destory(msg_handler);  /* destory message handler */

    RETURN_ERROR(rc);
}

static int mqtt_publish_packet_handle(mqtt_client_t *c, platform_timer_t *timer)
{
    int len = 0, rc = MQTT_SUCCESS_ERROR;
    MQTTString topic_name;
    mqtt_message_t msg;
    int qos;
    msg.payloadlen = 0; 
    
    rc = mqtt_is_connected(c);
    if (MQTT_SUCCESS_ERROR != rc)
        RETURN_ERROR(rc);

    if (MQTTDeserialize_publish(&msg.dup, &qos, &msg.retained, &msg.id, &topic_name,
        (uint8_t**)&msg.payload, (int*)&msg.payloadlen, c->mqtt_read_buf, c->mqtt_read_buf_size) != 1)
        RETURN_ERROR(MQTT_PUBLISH_PACKET_ERROR);
    
    msg.qos = (mqtt_qos_t)qos;

    /* for qos1 and qos2, you need to send a ack packet */
    if (msg.qos != QOS0) {
        platform_mutex_lock(&c->mqtt_write_lock);
        
        if (msg.qos == QOS1)
            len = MQTTSerialize_ack(c->mqtt_write_buf, c->mqtt_write_buf_size, PUBACK, 0, msg.id);
        else if (msg.qos == QOS2)
            len = MQTTSerialize_ack(c->mqtt_write_buf, c->mqtt_write_buf_size, PUBREC, 0, msg.id);

        if (len <= 0)
            rc = MQTT_SERIALIZE_PUBLISH_ACK_PACKET_ERROR;
        else
            rc = mqtt_send_packet(c, len, timer);
        
        platform_mutex_unlock(&c->mqtt_write_lock);
    }

    if (rc < 0)
        RETURN_ERROR(rc);

    if (msg.qos != QOS2)
        mqtt_deliver_message(c, &topic_name, &msg);
    else {
        /* record the received of a qos2 message and only processes it when the qos2 message is received for the first time */
        if ((rc = mqtt_ack_list_record(c, PUBREL, msg.id, len, NULL)) != MQTT_ACK_NODE_IS_EXIST_ERROR)
            mqtt_deliver_message(c, &topic_name, &msg);
    }
    
    RETURN_ERROR(rc);
}


static int mqtt_pubrec_and_pubrel_packet_handle(mqtt_client_t *c, platform_timer_t *timer)
{
    int rc = MQTT_FAILED_ERROR;
    uint16_t packet_id;
    uint8_t dup, packet_type;
    
    rc = mqtt_is_connected(c);
    if (MQTT_SUCCESS_ERROR != rc)
        RETURN_ERROR(rc);

    if (MQTTDeserialize_ack(&packet_type, &dup, &packet_id, c->mqtt_read_buf, c->mqtt_read_buf_size) != 1)
        RETURN_ERROR(MQTT_PUBREC_PACKET_ERROR);

    (void) dup;
    rc = mqtt_publish_ack_packet(c, packet_id, packet_type);    /* make a ack packet and send it */
    rc = mqtt_ack_list_unrecord(c, packet_type, packet_id, NULL);

    RETURN_ERROR(rc);
}

/**
 * @brief 处理 MQTT 网络报文的核心函数
 *
 * 该函数负责接收并解析来自 MQTT 服务器的报文，并根据报文类型调用相应的处理逻辑。
 * 它是 MQTT 协议栈中实现“接收-分发”机制的关键环节，支持：
 *   - PUBLISH 消息接收
 *   - QoS 1/2 确认流程（PUBACK, PUBREC, PUBREL, PUBCOMP）
 *   - 订阅确认（SUBACK）
 *   - 取消订阅确认（UNSUBACK）
 *   - 心跳响应（PINGRESP）
 *   - 连接确认（CONNACK，已由连接流程处理）
 *
 * 同时在处理完成后检查并发送保活 PINGREQ（keep-alive）。
 *
 * @param[in,out] c     指向 MQTT 客户端实例的指针
 * @param[in]     timer 指向超时定时器的指针，用于控制读取等待时间
 *
 * @return
 *   - >=0 : 成功处理的报文类型（如 PUBLISH, PUBACK 等）
 *   - < 0 : 错误码（如网络错误、缓冲区不足、协议错误等）
 *
 * @note
 *   - CONNACK 报文在此函数中被忽略（已在连接阶段处理）
 *   - 若读取时缓冲区太小（MQTT_BUFFER_TOO_SHORT_ERROR），不会立即返回，
 *     而是继续运行，以便后续处理（防止数据丢失）
 *   - 处理完报文后会调用 mqtt_keep_alive() 检查是否需要发送 PINGREQ
 *   - 返回值为 packet_type 是为了向上传递“已处理的报文类型”，便于调试和状态判断
 *
 * @see mqtt_yield, mqtt_read_packet, mqtt_keep_alive
 *
 * @example
 *   platform_timer_t timer;
 *   platform_timer_cutdown(&timer, 1000);
 *   int rc = mqtt_packet_handle(&client, &timer);
 *   if (rc == PUBLISH) {
 *       printf("Received a PUBLISH message\n");
 *   }
 */
static int mqtt_packet_handle(mqtt_client_t* c, platform_timer_t* timer)
{
    int rc = MQTT_SUCCESS_ERROR;        // 返回码，初始化为成功
    int packet_type = 0;                // 存储读取到的 MQTT 报文类型

    // 从网络读取一个完整的 MQTT 报文头，解析出报文类型
    // 如果数据未就绪或正在接收，可能阻塞最多到 timer 超时
    rc = mqtt_read_packet(c, &packet_type, timer);

    // 根据读取到的报文类型进行分发处理
    switch (packet_type) {
        case 0: 
            /* 
             * 读取超时或发生错误（如网络断开）
             * 特别地，如果是因为缓冲区太小导致失败，需特殊处理
             */
            if (MQTT_BUFFER_TOO_SHORT_ERROR == rc) {
                MQTT_LOG_E("the client read buffer is too short, please call mqtt_set_read_buf_size() to reset the buffer size");
                /* 
                 * ❗重要：不能直接返回错误
                 * 原因：虽然缓冲区太小，但网络上已有数据可读
                 * 若立即返回，可能导致数据丢失或连接异常
                 * 应继续执行 keep-alive 检查，并在下一轮 yield 中重试
                 */
            }
            break;

        case CONNACK:
            /* 
             * CONNACK 报文通常在连接阶段已被处理
             * 运行时收到 CONNACK 属于协议错误（MQTT 3.1.1 规范）
             * 此处选择忽略，防止状态混乱
             */
            goto exit;

        case PUBACK:
        case PUBCOMP:
            /*
             * 处理 QoS 1 的 PUBACK 或 QoS 2 的 PUBCOMP 确认报文
             * 包括匹配消息 ID、调用回调、释放资源等
             */
            rc = mqtt_puback_and_pubcomp_packet_handle(c, timer);
            break;

        case SUBACK:
            /*
             * 处理 SUBSCRIBE 请求的响应
             * 更新订阅状态，触发订阅完成回调
             */
            rc = mqtt_suback_packet_handle(c, timer);
            break;
            
        case UNSUBACK:
            /*
             * 处理 UNSUBSCRIBE 请求的响应
             * 表示取消订阅成功
             */
            rc = mqtt_unsuback_packet_handle(c, timer);
            break;

        case PUBLISH:
            /*
             * 收到来自服务器的 PUBLISH 消息
             * 根据 QoS 级别进行响应（QoS0: 无响应, QoS1: PUBACK, QoS2: PUBREC/PUBREL）
             * 并调用用户注册的 message callback
             */
            rc = mqtt_publish_packet_handle(c, timer);
            break;

        case PUBREC:
        case PUBREL:
            /*
             * 处理 QoS 2 流程中的 PUBREC 和 PUBREL 报文
             * 实现可靠的双发双确认机制
             */
            rc = mqtt_pubrec_and_pubrel_packet_handle(c, timer);
            break;

        case PINGRESP:
            /*
             * 收到 PING 响应，表示连接正常
             * 清除 ping 标志，表示保活成功
             */
            c->mqtt_ping_outstanding = 0;    /* keep alive ping success */
            break;

        default:
            /*
             * 收到未知或非法报文类型
             * 视为协议错误，退出处理
             */
            goto exit;
    }

    // 在处理完一个报文后，检查是否需要发送 PINGREQ 以维持连接
    rc = mqtt_keep_alive(c);

exit:
    // 如果整体处理成功（rc == 0），则返回实际处理的报文类型
    // 这样上层可以知道“发生了什么”，便于调试和状态跟踪
    if (rc == MQTT_SUCCESS_ERROR)
        rc = packet_type;

    // 返回最终结果
    RETURN_ERROR(rc);
}

static int mqtt_wait_packet(mqtt_client_t* c, int packet_type, platform_timer_t* timer)
{
    int rc = MQTT_FAILED_ERROR;

    do {
        if (platform_timer_is_expired(timer))
            break; 
        rc = mqtt_packet_handle(c, timer);
    } while (rc != packet_type && rc >= 0);

    RETURN_ERROR(rc);
}

/**
 * @brief 执行 MQTT 客户端一次“轮询”操作，处理网络 I/O 与协议逻辑
 *
 * 该函数是 MQTT 客户端的核心通信入口，用于：
 *   - 接收并处理来自服务器的报文（PUBLISH, PINGRESP, PUBACK 等）
 *   - 发送保活 PINGREQ（若需要）
 *   - 重传超时的 QoS 报文
 *   - 处理连接状态变化（断开、重连）
 *
 * 它通常在后台线程（如 mqtt_yield_thread）或主循环中被周期性调用，
 * 实现非阻塞、事件驱动的 MQTT 通信模型。
 *
 * @param[in,out] c           指向 MQTT 客户端实例的指针
 * @param[in]     timeout_ms  最大阻塞等待时间（毫秒）
 *                            若为 0，则使用客户端默认的 c->mqtt_cmd_timeout
 *
 * @return
 *   - MQTT_SUCCESS_ERROR (>=0)        : 正常处理完成（可能有数据收发）
 *   - MQTT_CLEAN_SESSION_ERROR        : 客户端状态为 CLEAN_SESSION，需清理会话
 *   - MQTT_RECONNECT_TIMEOUT_ERROR    : 重连超时
 *   - MQTT_NOT_CONNECT_ERROR          : 网络未连接且无法立即重连
 *   - 其他负值                     : 网络或协议错误
 *
 * @note
 *   - 此函数为非阻塞设计，实际等待时间由内部 select/poll 或平台 I/O 决定
 *   - 在超时时间内持续尝试处理，直到有事件发生或超时
 *   - 调用 mqtt_packet_handle() 处理具体报文
 *   - 定期扫描 ACK 列表，处理超时重传（QoS 1/2）
 *   - 支持自动重连机制（mqtt_try_reconnect）
 *
 * @see mqtt_yield_thread, mqtt_packet_handle, mqtt_try_reconnect, mqtt_ack_list_scan
 *
 * @example
 *   while (1) {
 *       mqtt_yield(&client, 1000);  // 每次最多等待 1s
 *       platform_sleep_ms(10);
 *   }
 */
static int mqtt_yield(mqtt_client_t* c, int timeout_ms)
{
    int rc = MQTT_SUCCESS_ERROR;              // 返回码，初始化为成功
    client_state_t state;                     // 当前客户端状态
    platform_timer_t timer;                   // 用于控制本次 yield 超时的定时器

    // 参数校验：客户端指针不能为空
    if (NULL == c)
        RETURN_ERROR(MQTT_FAILED_ERROR);

    // 如果 timeout_ms 为 0，使用客户端默认命令超时时间
    if (0 == timeout_ms)
        timeout_ms = c->mqtt_cmd_timeout;

    // 初始化定时器
    platform_timer_init(&timer);
    // 设置倒计时：从现在起 timeout_ms 毫秒后超时
    platform_timer_cutdown(&timer, timeout_ms);
    
    // 主循环：在超时时间内持续处理
    while (!platform_timer_is_expired(&timer)) {
        // 获取当前客户端状态
        state = mqtt_get_client_state(c);

        // 如果状态为 CLEAN_SESSION，表示需要清理会话（如服务器要求）
        if (CLIENT_STATE_CLEAN_SESSION == state) {
            RETURN_ERROR(MQTT_CLEAN_SESSION_ERROR);
        } 
        // 如果未连接（且非 CLEAN_SESSION 状态），尝试重连
        else if (CLIENT_STATE_CONNECTED != state) {
            /* mqtt not connect, need reconnect */
            rc = mqtt_try_reconnect(c);

            // 如果重连已超时，直接返回错误
            if (MQTT_RECONNECT_TIMEOUT_ERROR == rc)
                RETURN_ERROR(rc);

            // 否则继续循环（可能正在重连中）
            continue;
        }
        
        /* --- 客户端已连接，处理 MQTT 报文 --- */

        // 调用底层函数处理网络报文（接收、解析、响应）
        rc = mqtt_packet_handle(c, &timer);

        // 如果处理成功（rc >= 0），说明有报文被处理或无错误
        if (rc >= 0) {
            /* 扫描 ACK 列表：处理超时的 QoS 报文（重传或销毁）

/**
 * @brief MQTT 客户端后台主循环线程函数
 *
 * 该函数作为独立线程运行，持续调用 mqtt_yield() 处理 MQTT 协议的底层通信，
 * 包括接收报文、发送保活 PING、触发回调等。它是实现“自动消息处理”的核心机制。
 *
 * 线程启动后：
 *   - 首先检查客户端是否已连接
 *   - 进入无限循环，周期性调用 mqtt_yield()
 *   - 根据返回值判断是否需要断开、重连或清理会话
 *   - 若会话被清理，则销毁自身线程资源并退出
 *
 * @param[in] arg  传递给线程的参数，必须为指向 mqtt_client_t 的指针
 *
 * @note
 *   - 该线程由 mqtt_connect_with_results() 在首次连接成功后自动创建
 *   - 线程在客户端断开且 clean session 时自我销毁
 *   - 不应在外部直接调用此函数
 *   - 使用 platform_thread_stop() 可主动终止该线程
 *
 * @see mqtt_connect_with_results(), mqtt_yield(), platform_thread_init()
 *
 * @example
 *   // 通常由 platform_thread_init 启动：
 *   platform_thread_init("mqtt_yield", mqtt_yield_thread, &client, ...);
 */
static void mqtt_yield_thread(void *arg)
{
    int rc;                                     // 存储 mqtt_yield 的返回码
    client_state_t state;                       // 当前客户端状态
    mqtt_client_t *c = (mqtt_client_t *)arg;    // 将参数转换为客户端指针
    platform_thread_t *thread_to_be_destoried = NULL; // 用于保存将被销毁的线程句柄

    // 获取当前客户端状态
    state = mqtt_get_client_state(c);
    // 如果客户端未处于 CONNECTED 状态，打印警告并停止当前线程
    if (CLIENT_STATE_CONNECTED != state) {
        MQTT_LOG_W("%s:%d %s()..., mqtt is not connected to the server...", 
                   __FILE__, __LINE__, __FUNCTION__);
        platform_thread_stop(c->mqtt_thread);   /* 停止当前线程 */
        // 注意：此处未 return，会继续进入 while(1)，存在逻辑问题（见下方说明）
    }

    // 主循环：持续处理 MQTT 协议事件
    while (1) {
        // 调用 mqtt_yield 处理网络 I/O 和协议逻辑
        // 超时时间由 c->mqtt_cmd_timeout 控制（单位：毫秒）
        rc = mqtt_yield(c, c->mqtt_cmd_timeout);

        // 如果返回 CLEAN_SESSION 错误，表示需要清理会话（如服务器断开）
        if (MQTT_CLEAN_SESSION_ERROR == rc) {
            MQTT_LOG_W("%s:%d %s()..., mqtt clean session....", 
                       __FILE__, __LINE__, __FUNCTION__);
            // 断开网络连接
            network_disconnect(c->mqtt_network);
            // 清理客户端会话状态（清除订阅、消息队列等）
            mqtt_clean_session(c);
            // 跳转到 exit 标签，准备销毁线程
            goto exit;
        } 
        // 如果返回重连超时错误，仅打印日志，继续尝试
        else if (MQTT_RECONNECT_TIMEOUT_ERROR == rc) {
            MQTT_LOG_W("%s:%d %s()..., mqtt reconnect timeout....", 
                       __FILE__, __LINE__, __FUNCTION__);
            // 注意：这里没有采取进一步动作，依赖外部逻辑处理重连
        }
        // 其他情况（如成功、正常读取等）继续循环
    }
    
exit:
    // 准备销毁当前线程资源
    thread_to_be_destoried = c->mqtt_thread;

    // 如果线程句柄有效，则销毁并释放内存
    if (NULL != thread_to_be_destoried) {
        platform_thread_destroy(thread_to_be_destoried);    // 销毁线程对象
        platform_memory_free(thread_to_be_destoried);       // 释放动态分配的内存
        thread_to_be_destoried = NULL;                      // 防止悬空指针
    }

    // 注意：函数返回后，线程结束
}

/**
 * @brief 执行 MQTT 连接流程，并等待 CONNACK 响应，返回连接结果
 *
 * 该函数实现完整的 MQTT CONNECT 流程，包括：
 *   - 网络初始化与连接（TCP 或 TLS）
 *   - 序列化并发送 CONNECT 报文
 *   - 等待并解析 CONNACK 响应
 *   - 成功后启动或恢复 MQTT 主循环线程（用于自动 yield）
 *
 * 此函数是 MQTT 客户端建立连接的核心逻辑，线程安全（通过 write_lock 保护发送过程）。
 *
 * @param[in,out] c  指向 MQTT 客户端实例的指针
 *
 * @return
 *   - MQTT_SUCCESS_ERROR (0) : 连接成功
 *   - 其他错误码         : 连接失败，具体见返回值定义
 *
 * @note
 *   - 必须在调用前正确初始化客户端结构体（host, port, client_id 等）
 *   - 若已连接，直接返回成功（避免重复连接）
 *   - 支持 Clean Session、Will 消息、用户名密码等标准特性
 *   - TLS 支持通过编译宏 MQTT_NETWORK_TYPE_NO_TLS 控制
 *   - 成功连接后自动启动后台 yield 线程（若未启动）
 *
 * @see mqtt_connect, mqtt_reconnect, mqtt_yield_thread, mqtt_disconnect
 *
 * @example
 *   mqtt_client_t client;
 *   mqtt_init(&client, ...);
 *   int rc = mqtt_connect_with_results(&client);
 *   if (rc == MQTT_SUCCESS_ERROR) {
 *       printf("MQTT connected!\n");
 *   }
 */
static int mqtt_connect_with_results(mqtt_client_t* c)
{
    int len = 0;                              // 存储序列化后的 CONNECT 报文长度
    int rc = MQTT_CONNECT_FAILED_ERROR;       // 返回码，初始化为失败
    platform_timer_t connect_timer;           // 用于等待 CONNACK 的超时定时器
    mqtt_connack_data_t connack_data = {0};   // 存储 CONNACK 解析结果
    // 初始化 CONNECT 报文数据结构（来自 Paho MQTT 库）
    MQTTPacket_connectData connect_data = MQTTPacket_connectData_initializer;

    // 参数校验：客户端指针不能为空
    if (NULL == c)
        RETURN_ERROR(MQTT_NULL_VALUE_ERROR);

    // 如果客户端已处于 CONNECTED 状态，直接返回成功
    // 避免重复连接
    if (CLIENT_STATE_CONNECTED == mqtt_get_client_state(c))
        RETURN_ERROR(MQTT_SUCCESS_ERROR);

#ifndef MQTT_NETWORK_TYPE_NO_TLS
    // 初始化网络层（支持 TLS）
    // 传入 CA 证书用于验证服务器
    rc = network_init(c->mqtt_network, c->mqtt_host, c->mqtt_port, c->mqtt_ca);
#else
    // 初始化网络层（纯 TCP，无 TLS）
    rc = network_init(c->mqtt_network, c->mqtt_host, c->mqtt_port, NULL);
#endif

    // 建立底层网络连接（TCP/TLS）
    rc = network_connect(c->mqtt_network);
    if (MQTT_SUCCESS_ERROR != rc) {
        // 连接失败，释放网络资源
        if (NULL != c->mqtt_network) {
            network_release(c->mqtt_network);
            RETURN_ERROR(rc);  // 直接返回网络层错误码
        }  
    }
    
    // 日志：网络连接成功
    MQTT_LOG_I("%s:%d %s()... mqtt connect success...", __FILE__, __LINE__, __FUNCTION__);

    // 设置 CONNECT 报文参数，来自客户端配置
    connect_data.keepAliveInterval = c->mqtt_keep_alive_interval;  // 保活间隔（秒）
    connect_data.cleansession     = c->mqtt_clean_session;          // Clean Session 标志
    connect_data.MQTTVersion      = c->mqtt_version;                // MQTT 协议版本
    connect_data.clientID.cstring = c->mqtt_client_id;              // 客户端 ID
    connect_data.username.cstring = c->mqtt_user_name;              // 用户名
    connect_data.password.cstring = c->mqtt_password;               // 密码

    // 如果启用了遗嘱消息（Will Message）
    if (c->mqtt_will_flag) {
        connect_data.willFlag = c->mqtt_will_flag;
        connect_data.will.qos        = c->mqtt_will_options->will_qos;
        connect_data.will.retained   = c->mqtt_will_options->will_retained;
        connect_data.will.topicName.cstring = c->mqtt_will_options->will_topic;
        connect_data.will.message.cstring   = c->mqtt_will_options->will_message;
    }
    
    // 设置“最后接收时间”为当前时间 + keepAliveInterval
    // 用于后续 PING 操作的判断
    platform_timer_cutdown(&c->mqtt_last_received, (c->mqtt_keep_alive_interval * 1000));

    // 加锁，保护发送过程（避免多线程并发发送）
    platform_mutex_lock(&c->mqtt_write_lock);

    /* --- 发送 CONNECT 报文 --- */

    // 序列化 CONNECT 报文到写缓冲区
    if ((len = MQTTSerialize_connect(c->mqtt_write_buf, c->mqtt_write_buf_size, &connect_data)) <= 0) {
        // 序列化失败，跳转到错误处理
        goto exit;
    }
        
    // 设置连接超时定时器（等待 CONNACK）
    platform_timer_cutdown(&connect_timer, c->mqtt_cmd_timeout);

    // 发送 CONNECT 报文
    if ((rc = mqtt_send_packet(c, len, &connect_timer)) != MQTT_SUCCESS_ERROR) {
        // 发送失败，跳转处理
        goto exit;
    }

    /* --- 等待并处理 CONNACK --- */

    // 等待 CONNACK 报文到达
    if (mqtt_wait_packet(c, CONNACK, &connect_timer) == CONNACK) {
        // 成功收到 CONNACK，尝试反序列化解析
        if (MQTTDeserialize_connack(&connack_data.session_present, &connack_data.rc,
                                    c->mqtt_read_buf, c->mqtt_read_buf_size) == 1) {
            // 解析成功，获取返回码（0=连接成功，其他为错误）
            rc = connack_data.rc;
        } else {
            // CONNACK 报文格式错误
            rc = MQTT_CONNECT_FAILED_ERROR;
        }
    } else {
        // 未收到 CONNACK（超时或收到其他报文）
        rc = MQTT_CONNECT_FAILED_ERROR;
    }

exit:
    // 根据连接结果进行后续处理
    if (rc == MQTT_SUCCESS_ERROR) {
        // 连接成功

        if (NULL == c->mqtt_thread) {
            // 第一次连接：需要创建并启动 MQTT 主循环线程（用于自动 yield）

            c->mqtt_thread = platform_thread_init(
                "mqtt_yield_thread",      // 线程名
                mqtt_yield_thread,        // 线程入口函数
                c,                        // 传入参数：客户端指针
                MQTT_THREAD_STACK_SIZE,   // 栈大小
                MQTT_THREAD_PRIO,         // 优先级
                MQTT_THREAD_TICK          // 时间片
            );

            if (NULL != c->mqtt_thread) {
                // 线程创建成功
                mqtt_set_client_state(c, CLIENT_STATE_CONNECTED);  // 更新状态
                platform_thread_startup(c->mqtt_thread);           // 线程准备启动
                platform_thread_start(c->mqtt_thread);             // 启动线程
            } else {
                // 线程创建失败
                network_release(c->mqtt_network);                  // 断开网络
                rc = MQTT_CONNECT_FAILED_ERROR;                    // 标记失败
                MQTT_LOG_W("%s:%d %s()... mqtt yield thread creat failed...", __FILE__, __LINE__, __FUNCTION__);    
            }
        } else {
            // 重连场景：线程已存在，只需更新状态
            mqtt_set_client_state(c, CLIENT_STATE_CONNECTED);
        }

        c->mqtt_ping_outstanding = 0;  // 重置 PING 请求标志（无未完成的 PING）

    } else {
        // 连接失败
        network_release(c->mqtt_network);                            // 释放网络连接
        mqtt_set_client_state(c, CLIENT_STATE_INITIALIZED);          // 恢复到初始化状态
    }
    
    // 释放发送锁
    platform_mutex_unlock(&c->mqtt_write_lock);

    // 返回最终结果
    RETURN_ERROR(rc);
}



static uint32_t mqtt_read_buf_malloc(mqtt_client_t* c, uint32_t size)
{
    MQTT_ROBUSTNESS_CHECK(c, 0);
    
    if (NULL != c->mqtt_read_buf)
        platform_memory_free(c->mqtt_read_buf);
    
    c->mqtt_read_buf_size = size;

    /* limit the size of the read buffer */
    if ((MQTT_MIN_PAYLOAD_SIZE >= c->mqtt_read_buf_size) || (MQTT_MAX_PAYLOAD_SIZE <= c->mqtt_read_buf_size))
        c->mqtt_read_buf_size = MQTT_DEFAULT_BUF_SIZE;
    
    c->mqtt_read_buf = (uint8_t*) platform_memory_alloc(c->mqtt_read_buf_size);
    
    if (NULL == c->mqtt_read_buf) {
        MQTT_LOG_E("%s:%d %s()... malloc read buf failed...", __FILE__, __LINE__, __FUNCTION__);
        RETURN_ERROR(MQTT_MEM_NOT_ENOUGH_ERROR);
    }
    return c->mqtt_read_buf_size;
}

static uint32_t mqtt_write_buf_malloc(mqtt_client_t* c, uint32_t size)
{
    MQTT_ROBUSTNESS_CHECK(c, 0);
    
    if (NULL != c->mqtt_write_buf)
        platform_memory_free(c->mqtt_write_buf);
    
    c->mqtt_write_buf_size = size;

    /* limit the size of the read buffer */
    if ((MQTT_MIN_PAYLOAD_SIZE >= c->mqtt_write_buf_size) || (MQTT_MAX_PAYLOAD_SIZE <= c->mqtt_write_buf_size))
        c->mqtt_write_buf_size = MQTT_DEFAULT_BUF_SIZE;
    
    c->mqtt_write_buf = (uint8_t*) platform_memory_alloc(c->mqtt_write_buf_size);
    
    if (NULL == c->mqtt_write_buf) {
        MQTT_LOG_E("%s:%d %s()... malloc write buf failed...", __FILE__, __LINE__, __FUNCTION__);
        RETURN_ERROR(MQTT_MEM_NOT_ENOUGH_ERROR);
    }
    return c->mqtt_write_buf_size;
}

static int mqtt_init(mqtt_client_t* c)
{
    /* network init */
    c->mqtt_network = (network_t*) platform_memory_alloc(sizeof(network_t));

    if (NULL == c->mqtt_network) {
        MQTT_LOG_E("%s:%d %s()... malloc memory failed...", __FILE__, __LINE__, __FUNCTION__);
        RETURN_ERROR(MQTT_MEM_NOT_ENOUGH_ERROR);
    }
    memset(c->mqtt_network, 0, sizeof(network_t));

    c->mqtt_packet_id = 1;
    c->mqtt_clean_session = 0;          //no clear session by default
    c->mqtt_will_flag = 0;
    c->mqtt_cmd_timeout = MQTT_DEFAULT_CMD_TIMEOUT;
    c->mqtt_client_state = CLIENT_STATE_INITIALIZED;
    
    c->mqtt_ping_outstanding = 0;
    c->mqtt_ack_handler_number = 0;
    c->mqtt_client_id_len = 0;
    c->mqtt_user_name_len = 0;
    c->mqtt_password_len = 0;
    c->mqtt_keep_alive_interval = MQTT_KEEP_ALIVE_INTERVAL;
    c->mqtt_version = MQTT_VERSION;
    c->mqtt_reconnect_try_duration = MQTT_RECONNECT_DEFAULT_DURATION;

    c->mqtt_will_options = NULL;
    c->mqtt_reconnect_data = NULL;
    c->mqtt_reconnect_handler = NULL;
    c->mqtt_interceptor_handler = NULL;
    
    mqtt_read_buf_malloc(c, MQTT_DEFAULT_BUF_SIZE);
    mqtt_write_buf_malloc(c, MQTT_DEFAULT_BUF_SIZE);

    mqtt_list_init(&c->mqtt_msg_handler_list);
    mqtt_list_init(&c->mqtt_ack_handler_list);
    
    platform_mutex_init(&c->mqtt_write_lock);
    platform_mutex_init(&c->mqtt_global_lock);

    platform_timer_init(&c->mqtt_last_sent);
    platform_timer_init(&c->mqtt_last_received);

    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}

/********************************************************* mqttclient global function ********************************************************/

MQTT_CLIENT_SET_DEFINE(client_id, char*, NULL)
MQTT_CLIENT_SET_DEFINE(user_name, char*, NULL)
MQTT_CLIENT_SET_DEFINE(password, char*, NULL)
MQTT_CLIENT_SET_DEFINE(host, char*, NULL)
MQTT_CLIENT_SET_DEFINE(port, char*, NULL)
MQTT_CLIENT_SET_DEFINE(ca, char*, NULL)
MQTT_CLIENT_SET_DEFINE(reconnect_data, void*, NULL)
MQTT_CLIENT_SET_DEFINE(keep_alive_interval, uint16_t, 0)
MQTT_CLIENT_SET_DEFINE(will_flag, uint32_t, 0)
MQTT_CLIENT_SET_DEFINE(clean_session, uint32_t, 0)
MQTT_CLIENT_SET_DEFINE(version, uint32_t, 0)
MQTT_CLIENT_SET_DEFINE(cmd_timeout, uint32_t, 0)
MQTT_CLIENT_SET_DEFINE(reconnect_try_duration, uint32_t, 0)
MQTT_CLIENT_SET_DEFINE(reconnect_handler, reconnect_handler_t, NULL)
MQTT_CLIENT_SET_DEFINE(interceptor_handler, interceptor_handler_t, NULL)

uint32_t mqtt_set_read_buf_size(mqtt_client_t *c, uint32_t size) 
{ 
    return mqtt_read_buf_malloc(c, size);
}

uint32_t mqtt_set_write_buf_size(mqtt_client_t *c, uint32_t size) 
{ 
    return mqtt_write_buf_malloc(c, size);
}

void mqtt_sleep_ms(int ms)
{
    platform_timer_usleep(ms * 1000);
}

int mqtt_keep_alive(mqtt_client_t* c)
{
    int rc = MQTT_SUCCESS_ERROR;
    
    rc = mqtt_is_connected(c);
    if (MQTT_SUCCESS_ERROR != rc)
        RETURN_ERROR(rc);

    if (platform_timer_is_expired(&c->mqtt_last_sent) || platform_timer_is_expired(&c->mqtt_last_received)) {
        if (c->mqtt_ping_outstanding) {
            MQTT_LOG_W("%s:%d %s()... ping outstanding", __FILE__, __LINE__, __FUNCTION__);
            /*must realse the socket file descriptor zhaoshimin 20200629*/
            network_release(c->mqtt_network);
            
            mqtt_set_client_state(c, CLIENT_STATE_DISCONNECTED);
            rc = MQTT_NOT_CONNECT_ERROR; /* PINGRESP not received in keepalive interval */
        } else {
            platform_timer_t timer;
            int len = MQTTSerialize_pingreq(c->mqtt_write_buf, c->mqtt_write_buf_size);
            if (len > 0)
                rc = mqtt_send_packet(c, len, &timer); // 100ask, send the ping packet
            c->mqtt_ping_outstanding++;
        }
    }

    RETURN_ERROR(rc);
}

mqtt_client_t *mqtt_lease(void)
{
    int rc;
    mqtt_client_t* c;

    c = (mqtt_client_t *)platform_memory_alloc(sizeof(mqtt_client_t));
    if (NULL == c)
        return NULL;

    memset(c, 0, sizeof(mqtt_client_t));

    rc = mqtt_init(c);
    if (MQTT_SUCCESS_ERROR != rc)
        return NULL;
    
    return c;
}

int mqtt_release(mqtt_client_t* c)
{
    platform_timer_t timer;

    if (NULL == c)
        RETURN_ERROR(MQTT_NULL_VALUE_ERROR);

    platform_timer_init(&timer);
    platform_timer_cutdown(&timer, c->mqtt_cmd_timeout);
    
    /* wait for the clean session to complete */
    while ((CLIENT_STATE_INVALID != mqtt_get_client_state(c))) {
        // platform_timer_usleep(1000);            // 1ms avoid compiler optimization.
        if (platform_timer_is_expired(&timer)) {
            MQTT_LOG_E("%s:%d %s()... mqtt release failed...", __FILE__, __LINE__, __FUNCTION__);
            RETURN_ERROR(MQTT_FAILED_ERROR)
        }    
    }
    
    if (NULL != c->mqtt_network) {
        platform_memory_free(c->mqtt_network);
        c->mqtt_network = NULL;
    }

    if (NULL != c->mqtt_read_buf) {
        platform_memory_free(c->mqtt_read_buf);
        c->mqtt_read_buf = NULL;
    }

    if (NULL != c->mqtt_write_buf) {
        platform_memory_free(c->mqtt_write_buf);
        c->mqtt_write_buf = NULL;
    }

    platform_mutex_destroy(&c->mqtt_write_lock);
    platform_mutex_destroy(&c->mqtt_global_lock);

    memset(c, 0, sizeof(mqtt_client_t));

    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}

int mqtt_connect(mqtt_client_t* c)
{
    /* connect server in blocking mode and wait for connection result */
    return mqtt_connect_with_results(c);
}

int mqtt_disconnect(mqtt_client_t* c)
{
    int rc = MQTT_FAILED_ERROR;
    platform_timer_t timer;
    int len = 0;

    platform_timer_init(&timer);
    platform_timer_cutdown(&timer, c->mqtt_cmd_timeout);

    platform_mutex_lock(&c->mqtt_write_lock);

    /* serialize disconnect packet and send it */
    len = MQTTSerialize_disconnect(c->mqtt_write_buf, c->mqtt_write_buf_size);
    if (len > 0)
        rc = mqtt_send_packet(c, len, &timer);

    platform_mutex_unlock(&c->mqtt_write_lock);

    mqtt_set_client_state(c, CLIENT_STATE_CLEAN_SESSION);
    
    RETURN_ERROR(rc);
}

/**
 * @brief 订阅一个或多个 MQTT 主题
 *
 * 该函数向 MQTT Broker 发送 SUBSCRIBE 报文，请求订阅指定的主题过滤器（topic filter）。
 * 当有匹配主题的消息发布时，Broker 会向客户端推送消息，并由注册的回调函数处理。
 *
 * @param[in]  c             指向已连接的 MQTT 客户端实例
 * @param[in]  topic_filter  要订阅的主题过滤器（支持通配符 + 和 #）
 * @param[in]  qos           请求的 QoS 级别（QOS0, QOS1, QOS2）
 * @param[in]  handler       消息到达时的回调处理函数；若为 NULL，则使用默认处理器
 *
 * @return
 *   - MQTT_SUCCESS_ERROR (0): 订阅请求已成功发送（注意：不代表 Broker 已确认）
 *   - MQTT_NOT_CONNECT_ERROR: 客户端未连接
 *   - MQTT_MEM_NOT_ENOUGH_ERROR: 内存不足，无法创建消息处理器或记录 ACK
 *   - MQTT_SUBSCRIBE_ERROR: 序列化或发送失败
 *
 * @note
 *   - 订阅是否成功需等待 Broker 返回 SUBACK 报文，此函数仅表示“发送成功”。
 *   - 若订阅失败（如权限不足、主题非法），错误将在 SUBACK 中体现，需在 ACK 处理流程中处理。
 *   - handler 回调会被异步调用（通常在接收线程或事件循环中），应保证线程安全。
 *   - 若指定 NULL handler，则使用 default_msg_handler 处理该主题的消息。
 *
 * @see mqtt_ack_list_record(), default_msg_handler, mqtt_msg_handler_create()
 */
int mqtt_subscribe(mqtt_client_t* c, const char* topic_filter, mqtt_qos_t qos, message_handler_t handler)
{
    int rc = MQTT_SUBSCRIBE_ERROR;        // 返回码，初始化为订阅错误
    int len = 0;                          // 序列化后的报文长度
    int qos_level = qos;                  // 避免枚举对齐问题（某些编译器 enum != int）
    uint16_t packet_id;                   // 当前订阅请求的报文 ID
    platform_timer_t timer;               // 用于超时控制的定时器
    MQTTString topic = MQTTString_initializer; // MQTT 字符串结构体
    topic.cstring = (char *)topic_filter;      // 设置主题过滤器
    message_handlers_t *msg_handler = NULL;    // 本地消息处理器指针

    // 检查客户端是否已连接
    if (CLIENT_STATE_CONNECTED != mqtt_get_client_state(c))
        RETURN_ERROR(MQTT_NOT_CONNECT_ERROR); // 未连接则直接返回错误

    // 加锁，防止多线程并发写入写缓冲区
    platform_mutex_lock(&c->mqtt_write_lock);

    // 获取下一个可用的报文 ID（用于匹配后续的 SUBACK）
    packet_id = mqtt_get_next_packet_id(c);

    /* 序列化 SUBSCRIBE 报文到写缓冲区 */
    len = MQTTSerialize_subscribe(
              c->mqtt_write_buf,          // 输出缓冲区
              c->mqtt_write_buf_size,     // 缓冲区大小
              0,                          // dup 标志（SUBSCRIBE 报文初始为 0）
              packet_id,                  // 报文 ID
              1,                          // 订阅主题数量（目前只支持单个）
              &topic,                     // 主题过滤器
              (int *)&qos_level           // 请求的 QoS 级别
          );

    // 序列化失败（返回值 <= 0），跳转至清理
    if (len <= 0)
        goto exit;

    // 将序列化后的 SUBSCRIBE 报文通过网络发送
    if ((rc = mqtt_send_packet(c, len, &timer)) != MQTT_SUCCESS_ERROR)
        goto exit; // 发送失败，跳转退出

    // 若未指定回调函数，则使用默认消息处理器
    if (NULL == handler)
        handler = default_msg_handler;

    // 创建一个消息处理器节点，用于保存主题、QoS 和回调函数
    msg_handler = mqtt_msg_handler_create(topic_filter, qos, handler);
    if (NULL == msg_handler) {
        rc = MQTT_MEM_NOT_ENOUGH_ERROR; // 分配失败，内存不足
        goto exit;
    }

    // 将订阅请求记录到 ACK 列表，等待 SUBACK 确认
    // 记录成功后，若超时未收到 SUBACK，将触发重传机制
    rc = mqtt_ack_list_record(c, SUBACK, packet_id, len, msg_handler);

exit:
    // 释放写锁
    platform_mutex_unlock(&c->mqtt_write_lock);

    // 返回最终结果（可能为成功、内存不足、发送失败等）
    RETURN_ERROR(rc);
}


int mqtt_unsubscribe(mqtt_client_t* c, const char* topic_filter)
{
    int len = 0;
    int rc = MQTT_FAILED_ERROR;
    uint16_t packet_id;
    platform_timer_t timer;
    MQTTString topic = MQTTString_initializer;
    topic.cstring = (char *)topic_filter;
    message_handlers_t *msg_handler = NULL;

    if (CLIENT_STATE_CONNECTED != mqtt_get_client_state(c))
        RETURN_ERROR(MQTT_NOT_CONNECT_ERROR);
    
    platform_mutex_lock(&c->mqtt_write_lock);

    packet_id = mqtt_get_next_packet_id(c);
    
    /* serialize unsubscribe packet and send it */
    if ((len = MQTTSerialize_unsubscribe(c->mqtt_write_buf, c->mqtt_write_buf_size, 0, packet_id, 1, &topic)) <= 0)
        goto exit;
    if ((rc = mqtt_send_packet(c, len, &timer)) != MQTT_SUCCESS_ERROR)
        goto exit; 

    /* get a already subscribed message handler */
    msg_handler = mqtt_get_msg_handler(c, &topic);
    if (NULL == msg_handler) {
        rc = MQTT_MEM_NOT_ENOUGH_ERROR;
        goto exit;
    }

    rc = mqtt_ack_list_record(c, UNSUBACK, packet_id, len, msg_handler);

exit:

    platform_mutex_unlock(&c->mqtt_write_lock);

    RETURN_ERROR(rc);
}

/**
 * @brief 发布一条 MQTT 消息到指定主题
 *
 * 该函数将消息发布到指定的 MQTT 主题。根据 QoS 级别，可能需要等待确认（PUBACK 或 PUBREC），
 * 并在未收到确认时进行重传。函数内部会序列化 PUBLISH 报文并通过网络发送。
 *
 * @param[in,out] c             指向 MQTT 客户端实例的指针
 * @param[in]     topic_filter  要发布消息的主题（字符串形式）
 * @param[in]     msg           指向待发布消息结构体的指针，包含 payload、QoS、retain 标志和消息 ID
 *
 * @return
 *   - MQTT_SUCCESS_ERROR (0): 成功发送（不保证对方已接收，取决于 QoS）
 *   - MQTT_NOT_CONNECT_ERROR: 客户端未处于连接状态
 *   - MQTT_BUFFER_TOO_SHORT_ERROR: 消息负载长度超过客户端写缓冲区大小
 *   - MQTT_ACK_HANDLER_NUM_TOO_MUCH_ERROR: 待确认的消息数量已达上限（仅 QoS1/QoS2）
 *   - MQTT_MEM_NOT_ENOUGH_ERROR: 内存不足，无法记录重发消息
 *   - 其他负值: 发送过程中的底层网络或序列化错误
 *
 * @note
 *   - 若 QoS > 0，消息会被记录以便重传，直到收到对应确认。
 *   - 若发布失败且因资源耗尽（如内存或 ack handler 满），客户端状态将被置为断开，
 *     建议上层检测到此类错误后尝试重连。
 *   - 函数执行完成后会清空 msg->payloadlen 字段。
 */
int mqtt_publish(mqtt_client_t* c, const char* topic_filter, mqtt_message_t* msg)
{
    int len = 0;                    // 序列化后的报文长度
    int rc = MQTT_FAILED_ERROR;     // 返回码，初始化为失败
    platform_timer_t timer;         // 用于超时控制的定时器
    MQTTString topic = MQTTString_initializer;  // MQTT 字符串结构体，用于序列化
    topic.cstring = (char *)topic_filter;       // 设置主题字符串

    // 检查客户端是否处于已连接状态
    if (CLIENT_STATE_CONNECTED != mqtt_get_client_state(c)) {
        msg->payloadlen = 0;        // 清空 payload 长度（防御性操作）
        rc = MQTT_NOT_CONNECT_ERROR; // 设置错误码
        RETURN_ERROR(rc);           // 使用宏返回错误（可能包含日志输出）
        // goto exit; /* 100ask */ // 原注释，已被 RETURN_ERROR 替代
    }

    // 如果 payload 非空但长度为 0，则尝试通过 strlen 自动计算长度
    // （适用于以 '\0' 结尾的字符串数据）
    if ((NULL != msg->payload) && (0 == msg->payloadlen))
        msg->payloadlen = strlen((char*)msg->payload);

    // 检查消息负载是否超出客户端写缓冲区容量
    if (msg->payloadlen > c->mqtt_write_buf_size) {
        MQTT_LOG_E("publish payload len is greater than client write buffer..."); // 日志警告
        RETURN_ERROR(MQTT_BUFFER_TOO_SHORT_ERROR); // 缓冲区不足错误
    }

    // 加锁，防止多线程并发写入网络缓冲区
    platform_mutex_lock(&c->mqtt_write_lock);

    // 对于 QoS > 0 的消息，需要记录 ACK 处理器以便重传
    if (QOS0 != msg->qos) {
        // 检查当前待确认的消息数量是否已达最大限制
        if (mqtt_ack_handler_is_maximum(c)) {
            rc = MQTT_ACK_HANDLER_NUM_TOO_MUCH_ERROR; /* 已达到最大记录数 */
            goto exit; // 跳转至清理和解锁
        }
        // 获取下一个可用的报文 ID（用于 QoS1/QoS2 的消息匹配）
        msg->id = mqtt_get_next_packet_id(c);
    }
    
    /* 序列化 PUBLISH 报文到写缓冲区 */
    len = MQTTSerialize_publish(
              c->mqtt_write_buf,      // 输出缓冲区
              c->mqtt_write_buf_size, // 缓冲区大小
              0,                      // dup 标志（初始为 0，后面可能设置）
              msg->qos,               // QoS 级别
              msg->retained,          // retain 标志
              msg->id,                // 报文 ID（QoS0 可为 0）
              topic,                  // 主题
              (uint8_t*)msg->payload, // 负载数据
              msg->payloadlen         // 负载长度
          );

    // 序列化失败（返回值 <= 0），直接跳转退出
    if (len <= 0)
        goto exit;
    
    // 将序列化后的数据通过网络发送
    if ((rc = mqtt_send_packet(c, len, &timer)) != MQTT_SUCCESS_ERROR)
        goto exit; // 发送失败，跳转退出
    
    // 如果是 QoS1 或 QoS2，需要等待对方确认，并准备重传机制
    if (QOS0 != msg->qos) {
        mqtt_set_publish_dup(c, 1);  /* 设置 DUP 标志，表示此消息可能重发 */

        if (QOS1 == msg->qos) {
            /* 期望收到 PUBACK 确认，否则将重发 */
            rc = mqtt_ack_list_record(c, PUBACK, msg->id, len, NULL);  
            
        } else if (QOS2 == msg->qos) {
            /* 期望收到 PUBREC 确认，否则将重发 */
            rc = mqtt_ack_list_record(c, PUBREC, msg->id, len, NULL);   
        }
        // 注意：这里 rc 是记录 ack handler 的结果，若失败仍继续执行
    }
    
exit:
    msg->payloadlen = 0;        // 清空 payload 长度，防止误用

    platform_mutex_unlock(&c->mqtt_write_lock); // 释放写锁

    // 特殊错误处理：若因资源不足导致发布失败
    if ((MQTT_ACK_HANDLER_NUM_TOO_MUCH_ERROR == rc) || 
        (MQTT_MEM_NOT_ENOUGH_ERROR == rc)) {
        MQTT_LOG_W("%s:%d %s()... there is not enough memory space to record...", 
                   __FILE__, __LINE__, __FUNCTION__);

        /* 释放底层网络文件描述符（避免资源泄漏） */
        network_release(c->mqtt_network);
        
        /* 标记客户端为断开状态，建议上层重新连接 */
        mqtt_set_client_state(c, CLIENT_STATE_DISCONNECTED);
    }

    RETURN_ERROR(rc);     // 使用宏返回最终结果（可能包含日志）
}


int mqtt_list_subscribe_topic(mqtt_client_t* c)
{
    int i = 0;
    mqtt_list_t *curr, *next;
    message_handlers_t *msg_handler;
    
    if (NULL == c)
        RETURN_ERROR(MQTT_NULL_VALUE_ERROR);

    if (mqtt_list_is_empty(&c->mqtt_msg_handler_list))
        MQTT_LOG_I("%s:%d %s()... there are no subscribed topics...", __FILE__, __LINE__, __FUNCTION__);

    LIST_FOR_EACH_SAFE(curr, next, &c->mqtt_msg_handler_list) {
        msg_handler = LIST_ENTRY(curr, message_handlers_t, list);
        /* determine whether a node already exists by mqtt topic, but wildcards are not supported */
        if (NULL != msg_handler->topic_filter) {
            MQTT_LOG_I("%s:%d %s()...[%d] subscribe topic: %s", __FILE__, __LINE__, __FUNCTION__, ++i ,msg_handler->topic_filter);
        }
    }
    
    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}

int mqtt_set_will_options(mqtt_client_t* c, char *topic, mqtt_qos_t qos, uint8_t retained, char *message)
{
    if ((NULL == c) || (NULL == topic))
        RETURN_ERROR(MQTT_NULL_VALUE_ERROR);

    if (NULL == c->mqtt_will_options) {
        c->mqtt_will_options = (mqtt_will_options_t *)platform_memory_alloc(sizeof(mqtt_will_options_t));
        MQTT_ROBUSTNESS_CHECK(c->mqtt_will_options, MQTT_MEM_NOT_ENOUGH_ERROR);
    }

    if (0 == c->mqtt_will_flag)
        c->mqtt_will_flag = 1;

    c->mqtt_will_options->will_topic = topic;
    c->mqtt_will_options->will_qos = qos;
    c->mqtt_will_options->will_retained = retained;
    c->mqtt_will_options->will_message = message;

    RETURN_ERROR(MQTT_SUCCESS_ERROR);
}
