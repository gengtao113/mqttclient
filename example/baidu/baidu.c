/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-11 21:53:07
 * @LastEditTime: 2020-06-08 20:38:41
 * @Description: 百度云 MQTT 客户端示例程序
 *               该文件演示了如何使用 MQTT 客户端连接百度云 IoT 平台，
 *               包括连接配置、消息发布、主题订阅等功能。
 *               代码属于 jiejie，请根据许可证保留作者信息和源代码。
 */
#include <stdio.h>      // 标准输入输出库
#include <unistd.h>     // UNIX 标准函数库（sleep 等）
#include <fcntl.h>      // 文件控制定义
#include <stdlib.h>     // 标准库函数（exit 等）
#include <pthread.h>    // POSIX 线程库
#include "mqttclient.h" // MQTT 客户端库

/**
 * @brief TLS 连接测试开关
 * 
 * 取消注释此行以启用 TLS 加密连接测试。
 * 默认使用非加密的 TCP 连接。
 */
// #define TEST_USEING_TLS  

/**
 * @brief 百度云 CA 证书
 * 
 * 用于 TLS 连接的根证书，确保与百度云服务器的安全通信。
 * 这是 GlobalSign 的根证书，用于验证服务器身份。
 * 
 * @note 仅在使用 TLS 连接时需要此证书
 */
static const char *test_baidu_ca_crt = {
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G\r\n"
    "A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp\r\n"
    "Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4\r\n"
    "MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG\r\n"
    "A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\r\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8\r\n"
    "RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsT\r\n"
    "gHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmm\r\n"
    "KPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zd\r\n"
    "QQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZ\r\n"
    "XriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCAwEAAaNCMEAw\r\n"
    "DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+o\r\n"
    "LkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZU\r\n"
    "RUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMp\r\n"
    "jjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK\r\n"
    "6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQX\r\n"
    "mcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecs\r\n"
    "Mx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpH\r\n"
    "WD9f\r\n"
    "-----END CERTIFICATE-----"
};

/**
 * @brief 主题1消息处理函数
 * 
 * 当收到 "topic1" 主题的消息时，此函数会被自动调用。
 * 函数会打印接收到的消息内容，包括主题名称和消息负载。
 * 
 * @param[in] client  MQTT 客户端实例指针（未使用）
 * @param[in] msg     接收到的消息数据
 * 
 * @note 这是一个回调函数，由 MQTT 客户端框架自动调用
 */
static void topic1_handler(void* client, message_data_t* msg)
{
    (void) client;    // 避免未使用参数警告
    MQTT_LOG_I("-----------------------------------------------------------------------------------");
    MQTT_LOG_I("%s:%d %s()...\ntopic: %s\nmessage:%s", __FILE__, __LINE__, __FUNCTION__, msg->topic_name, (char*)msg->message->payload);
    MQTT_LOG_I("-----------------------------------------------------------------------------------");
}

/**
 * @brief MQTT 消息发布线程函数
 * 
 * 在独立线程中运行，定期向 "topic1" 主题发布消息。
 * 消息内容包含欢迎信息和随机数，每4秒发布一次。
 * 
 * @param[in] arg  传递给线程的参数，必须为 mqtt_client_t 指针
 * @return 线程返回值（此函数永不返回）
 * 
 * @note 此函数在独立线程中运行，避免阻塞主线程
 */
void *mqtt_publish_thread(void *arg)
{
    mqtt_client_t *client = (mqtt_client_t *)arg;    // 将参数转换为客户端指针

    char buf[100] = { 0 };    // 消息缓冲区
    mqtt_message_t msg;       // MQTT 消息结构体
    memset(&msg, 0, sizeof(msg));    // 初始化消息结构体
    sprintf(buf, "welcome to mqttclient, this is a publish test...");    // 初始化消息内容

    sleep(2);    // 等待2秒，确保连接稳定

    mqtt_list_subscribe_topic(client);    // 列出当前订阅的主题

    msg.payload = (void *) buf;    // 设置消息负载
    msg.qos = 0;                   // 设置 QoS 级别为 0（最多一次）
    
    while(1) {    // 无限循环，持续发布消息
        sprintf(buf, "welcome to mqttclient, this is a publish test, a rand number: %d ...", random_number());    // 生成包含随机数的消息
        mqtt_publish(client, "topic1", &msg);    // 发布消息到 "topic1" 主题
        sleep(4);    // 等待4秒后发布下一条消息
    }
}

/**
 * @brief 主函数
 * 
 * 程序入口点，负责初始化 MQTT 客户端、建立连接、
 * 订阅主题、创建发布线程等。
 * 
 * @return 程序退出码
 */
int main(void)
{
    int res;                    // 线程创建结果
    pthread_t thread1;          // 发布线程句柄
    mqtt_client_t *client = NULL;    // MQTT 客户端实例
    char client_id[32];         // 客户端 ID 缓冲区

    printf("\nwelcome to mqttclient test...\n");    // 打印欢迎信息

    random_string(client_id, 10);    // 生成随机客户端 ID

    mqtt_log_init();    // 初始化日志系统

    client = mqtt_lease();    // 创建并初始化 MQTT 客户端实例

#ifdef TEST_USEING_TLS
    mqtt_set_port(client, "1884");    // 设置 TLS 端口
    mqtt_set_ca(client, (char*)test_baidu_ca_crt);    // 设置 CA 证书
#else
    mqtt_set_port(client, "1883");    // 设置普通 TCP 端口
#endif

    mqtt_set_host(client, "j6npr4w.mqtt.iot.gz.baidubce.com");    // 设置百度云服务器地址
    mqtt_set_client_id(client, client_id);    // 设置客户端 ID
    mqtt_set_user_name(client, "j6npr4w/mqtt-client-dev");    // 设置用户名
    mqtt_set_password(client, "lcUhUs5VYLMSbrnB");    // 设置密码
    mqtt_set_clean_session(client, 1);    // 启用清理会话

    mqtt_connect(client);    // 连接到 MQTT 服务器
    
    mqtt_subscribe(client, "topic1", QOS0, topic1_handler);    // 订阅 "topic1" 主题，设置回调函数
    
    res = pthread_create(&thread1, NULL, mqtt_publish_thread, client);    // 创建发布线程
    if(res != 0) {    // 检查线程创建是否成功
        MQTT_LOG_E("create mqtt publish thread fail");    // 打印错误日志
        exit(res);    // 退出程序
    }

    while (1) {    // 主线程无限循环
        sleep(100);    // 每100秒检查一次，保持程序运行
    }
}
