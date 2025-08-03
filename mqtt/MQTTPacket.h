/*******************************************************************************
 * Copyright (c) 2014 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Xiang Rong - 442039 Add makefile to Embedded C client
 *******************************************************************************/

#ifndef MQTTPACKET_H_
#define MQTTPACKET_H_

#if defined(__cplusplus) /* If this is a C++ compiler, use C linkage */
extern "C" {
#endif

#if defined(WIN32_DLL) || defined(WIN64_DLL)
  #define DLLImport __declspec(dllimport)
  #define DLLExport __declspec(dllexport)
#elif defined(LINUX_SO)
  #define DLLImport extern
  #define DLLExport  __attribute__ ((visibility ("default")))
#else
  #define DLLImport
  #define DLLExport  
#endif

enum errors
{
	MQTTPACKET_BUFFER_TOO_SHORT = -2,
	MQTTPACKET_READ_ERROR = -1,
	MQTTPACKET_READ_COMPLETE
};

/**
 * @brief MQTT 协议定义的控制报文类型（Message Type）
 *
 * 根据 MQTT v3.1.1 协议规范，所有报文的第一个字节的高 4 位（bit 7~4）
 * 表示报文类型，取值范围为 1~14。该枚举定义了所有合法的报文类型。
 *
 * 每个报文在客户端与服务器之间的交互构成 MQTT 的核心状态机，
 * 实现连接、消息传输、QoS 控制、保活与断开等机制。
 *
 * @note
 *   - 值从 1 开始，0 是非法报文类型（保留）
 *   - 每个报文类型在状态机中有特定的触发条件和处理逻辑
 *   - QoS 1/2 的确认机制依赖 PUBACK, PUBREC, PUBREL, PUBCOMP 等报文
 *   - PINGREQ/PINGRESP 实现 keep-alive 心跳机制
 *
 * @see https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html
 */
typedef enum msgTypes {
    CONNECT     =  1,   /**< 客户端 → 服务器 | 请求建立连接 | 触发：客户端启动 */
    CONNACK     =  2,   /**< 服务器 → 客户端 | 连接确认       | 触发：收到 CONNECT */
    PUBLISH     =  3,   /**< 双向           | 发布消息       | 触发：消息传输（QoS 0/1/2） */
    PUBACK      =  4,   /**< 双向           | QoS 1 确认     | 触发：收到 QoS 1 PUBLISH 或 PUBLISH 响应 */
    PUBREC      =  5,   /**< 双向           | QoS 2 第一阶段确认 | 触发：QoS 2 接收方收到 PUBLISH */
    PUBREL      =  6,   /**< 双向           | QoS 2 发布释放 | 触发：收到 PUBREC 后 */
    PUBCOMP     =  7,   /**< 双向           | QoS 2 完成确认 | 触发：收到 PUBREL 后 */
    SUBSCRIBE   =  8,   /**< 客户端 → 服务器 | 订阅主题       | 触发：客户端需接收某主题消息 */
    SUBACK      =  9,   /**< 服务器 → 客户端 | 订阅确认       | 触发：收到 SUBSCRIBE */
    UNSUBSCRIBE = 10,   /**< 客户端 → 服务器 | 取消订阅       | 触发：客户端不再关心某主题 */
    UNSUBACK    = 11,   /**< 服务器 → 客户端 | 取消订阅确认   | 触发：收到 UNSUBSCRIBE */
    PINGREQ     = 12,   /**< 客户端 → 服务器 | 心跳请求       | 触发：keep-alive 定时器超时 */
    PINGRESP    = 13,   /**< 服务器 → 客户端 | 心跳响应       | 触发：收到 PINGREQ */
    DISCONNECT  = 14    /**< 客户端 → 服务器 | 断开连接       | 触发：客户端正常关闭 */
} msgTypes_t;

/**
 * Bitfields for the MQTT header byte.
 */
typedef union
{
	unsigned char byte;	                /**< the whole byte */
#if defined(REVERSED)
	struct
	{
		unsigned int type : 4;			/**< message type nibble */
		unsigned int dup : 1;				/**< DUP flag bit */
		unsigned int qos : 2;				/**< QoS value, 0, 1 or 2 */
		unsigned int retain : 1;		/**< retained flag bit */
	} bits;
#else
	struct
	{
		unsigned int retain : 1;		/**< retained flag bit */
		unsigned int qos : 2;				/**< QoS value, 0, 1 or 2 */
		unsigned int dup : 1;				/**< DUP flag bit */
		unsigned int type : 4;			/**< message type nibble */
	} bits;
#endif
} MQTTHeader;

typedef struct
{
	int len;
	char* data;
} MQTTLenString;

typedef struct
{
	char* cstring;
	MQTTLenString lenstring;
} MQTTString;

#define MQTTString_initializer {NULL, {0, NULL}}

int MQTTstrlen(MQTTString mqttstring);

#include "MQTTConnect.h"
#include "MQTTPublish.h"
#include "MQTTSubscribe.h"
#include "MQTTUnsubscribe.h"
#include "MQTTFormat.h"

DLLExport int MQTTSerialize_ack(unsigned char* buf, int buflen, unsigned char type, unsigned char dup, unsigned short packetid);
DLLExport int MQTTDeserialize_ack(unsigned char* packettype, unsigned char* dup, unsigned short* packetid, unsigned char* buf, int buflen);

int MQTTPacket_len(int rem_len);
DLLExport int MQTTPacket_equals(MQTTString* a, char* b);

DLLExport int MQTTPacket_encode(unsigned char* buf, int length);
int MQTTPacket_decode(int (*getcharfn)(unsigned char*, int), int* value);
int MQTTPacket_decodeBuf(unsigned char* buf, int* value);

int readInt(unsigned char** pptr);
char readChar(unsigned char** pptr);
void writeChar(unsigned char** pptr, char c);
void writeInt(unsigned char** pptr, int anInt);
int readMQTTLenString(MQTTString* mqttstring, unsigned char** pptr, unsigned char* enddata);
void writeCString(unsigned char** pptr, const char* string);
void writeMQTTString(unsigned char** pptr, MQTTString mqttstring);

DLLExport int MQTTPacket_read(unsigned char* buf, int buflen, int (*getfn)(unsigned char*, int));

typedef struct {
	int (*getfn)(void *, unsigned char*, int); /* must return -1 for error, 0 for call again, or the number of bytes read */
	void *sck;	/* pointer to whatever the system may use to identify the transport */
	int multiplier;
	int rem_len;
	int len;
	char state;
}MQTTTransport;

int MQTTPacket_readnb(unsigned char* buf, int buflen, MQTTTransport *trp);

#ifdef __cplusplus /* If this is a C++ compiler, use C linkage */
}
#endif


#endif /* MQTTPACKET_H_ */
