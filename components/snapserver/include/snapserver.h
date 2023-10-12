/*
 * snapserver.h
 *
 *  Created on: 20.09.2023
 *      Author: carlos
 */

#ifndef COMPONENTS_SNAPSERVER_INCLUDE_SNAPSERVER_H_
#define COMPONENTS_SNAPSERVER_INCLUDE_SNAPSERVER_H_

#include "cJSON.h"

typedef struct tv
{
  int32_t sec;
  int32_t usec;
} tv_t;

typedef struct base_message
{
  uint16_t type;
  uint16_t id;
  uint16_t refersTo;
  tv_t sent;
  tv_t received;
  uint32_t size;
} base_message_t;

typedef struct hello_message
{
  char *mac;
  char *hostname;
  char *version;
  char *client_name;
  char *os;
  char *arch;
  int instance;
  char *id;
  int protocol_version;
} hello_message_t;

typedef struct server_settings_message
{
  int32_t buffer_ms;
  int32_t latency;
  uint32_t volume;
  bool muted;
} server_settings_message_t;

typedef struct codec_header_message
{
  uint32_t codec_size;
  char *codec;
  uint32_t size;
  char *payload;
} codec_header_message_t;

typedef struct wire_chunk_message
{
  tv_t timestamp;
  size_t size;
  char *payload;
} wire_chunk_message_t;

typedef struct time_message
{
  tv_t latency;
} time_message_t;

typedef enum message_type_e
{
  SNAPCAST_MESSAGE_BASE = 0,
  SNAPCAST_MESSAGE_CODEC_HEADER = 1,
  SNAPCAST_MESSAGE_WIRE_CHUNK = 2,
  SNAPCAST_MESSAGE_SERVER_SETTINGS = 3,
  SNAPCAST_MESSAGE_TIME = 4,
  SNAPCAST_MESSAGE_HELLO = 5,
  SNAPCAST_MESSAGE_STREAM_TAGS = 6,

  SNAPCAST_MESSAGE_FIRST = SNAPCAST_MESSAGE_BASE,
  SNAPCAST_MESSAGE_LAST = SNAPCAST_MESSAGE_STREAM_TAGS
} message_type_t;

typedef struct snapclient_s snapclient_t;
typedef struct snapclient_s {
	cJSON *helloMsg;
    int32_t latency;
    bool muted;
    uint8_t volume;

    int sock;
    uint32_t id_counter;
    QueueHandle_t msgQ;

//	snapclient_t *prev;
//	snapclient_t *next;
} snapclient_t;

typedef struct message_queue_s {
	message_type_t type;
	base_message_t baseMsg;
} message_queue_t;

void snapserver_init(void);


#endif /* COMPONENTS_SNAPSERVER_INCLUDE_SNAPSERVER_H_ */
