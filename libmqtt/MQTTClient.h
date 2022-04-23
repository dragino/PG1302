/*******************************************************************************
 * Copyright (c) 2014, 2015 IBM Corp.
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
 *    Allan Stockdill-Mander/Ian Craggs - initial API and implementation and/or initial documentation
 *    Ian Craggs - documentation and platform specific header
 *******************************************************************************/

#if !defined(__MQTT_CLIENT_C_)
#define __MQTT_CLIENT_C_

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdio.h>
#include "MQTTLinux.h"
#include <sys/time.h>

#include "MQTTPacket.h"
#include "MQTTErrors.h"
#include "stdio.h"


#define MAX_PACKET_ID 65535 /* according to the MQTT specification - do not change! */

#if !defined(MAX_MESSAGE_HANDLERS)
#define MAX_MESSAGE_HANDLERS 5 /* redefinable - how many subscriptions do you want? */
#endif

enum QoS
{
   QOS0,
   QOS1,
   QOS2
};

typedef struct MQTTMessage
{
   enum QoS qos;
   unsigned char retained;
   unsigned char dup;
   unsigned short id;
   void *payload;
   size_t payloadlen;
} MQTTMessage;

typedef struct MessageData
{
   MQTTMessage *message;
   MQTTString *topicName;
} MessageData;

typedef void (*messageHandler)(MessageData *, void *);

typedef struct MQTTClient
{
   unsigned int next_packetid,
       command_timeout_ms;
   size_t buf_size,
       readbuf_size;
   unsigned char *buf,
       *readbuf;
   unsigned int keep_alive_interval;
   char ping_outstanding;
#if defined(__linux__) || defined(__APPLE__)
   struct timeval ping_sent;
   long ping_time;
#endif
   int isconnected;

   struct MessageHandlers
   {
      void *arg;
      const char *topicFilter;
      void (*fp)(MessageData *, void *);
   } messageHandlers[MAX_MESSAGE_HANDLERS]; /* Message handlers are indexed by subscription topic */

   void (*defaultMessageHandler)(MessageData *);

   Network *ipstack;
   Timer ping_timer;
#if defined(MQTT_TASK)
   Thread read_thread;
   Queue reply;
   Mutex write_mutex;
#endif
} MQTTClient;

#define DefaultClient                 \
   {                                  \
      0, 0, 0, 0, NULL, NULL, 0, 0, 0 \
   }

/**
 * Create an MQTT client object
 * @param client
 * @param network
 * @param command_timeout_ms
 * @param
 */
void MQTTClientInit(MQTTClient *client, Network *network, unsigned int command_timeout_ms,
                              unsigned char *sendbuf, size_t sendbuf_size, unsigned char *readbuf, size_t readbuf_size);

/**
 * Destroy a MQTT client object
 * @param client
 */
void MQTTClientDestroy(MQTTClient *client);

/** MQTT Connect - send an MQTT connect packet down the network and wait for a Connack
 *  The nework object must be connected to the network endpoint before calling this
 *  @param options - connect options
 *  @return success code
 */
int MQTTConnect(MQTTClient *client, MQTTPacket_connectData *options);

/** MQTT Publish - send an MQTT publish packet and wait for all acks to complete for all QoSs
 *  @param client - the client object to use
 *  @param topic - the topic to publish to
 *  @param message - the message to send
 *  @return success code
 */
int MQTTPublish(MQTTClient *client, const char *, MQTTMessage *);

/** MQTT Subscribe - send an MQTT subscribe packet and wait for suback before returning.
 *  @param client - the client object to use
 *  @param topicFilter - the topic filter to subscribe to
 *  @param message - the message to send
 *  @return success code
 */
int MQTTSubscribe(MQTTClient *client, const char *topicFilter, enum QoS, messageHandler, void *arg);

/** MQTT Subscribe - send an MQTT unsubscribe packet and wait for unsuback before returning.
 *  @param client - the client object to use
 *  @param topicFilter - the topic filter to unsubscribe from
 *  @return success code
 */
int MQTTUnsubscribe(MQTTClient *client, const char *topicFilter);

/** MQTT Disconnect - send an MQTT disconnect packet and close the connection
 *  @param client - the client object to use
 *  @return success code
 */
int MQTTDisconnect(MQTTClient *client);

#if !defined(MQTT_TASK)
/** MQTT Yield - MQTT background
 *  @param client - the client object to use
 *  @param time - the time, in milliseconds, to yield for 
 *  @return success code
 */
int MQTTYield(MQTTClient *client, int time);
#endif

/** MQTT Send Ping - Send Ping Request Packet
 *  @param client - the client object to use
 *  @return success code
 */
int MQTTSendPing(MQTTClient *c);

#if defined(__linux__) || defined(__APPLE__)
/** MQTT Get Ping Time - Get the Return Trip Time of the last PingRequest/PingResponse
 * @param client - the client object to use
 * @return RTT in microseconds
 */
long MQTTGetPingTime(MQTTClient *client);
#endif

#if defined(__cplusplus)
}
#endif

#endif
