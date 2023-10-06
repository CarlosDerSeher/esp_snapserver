/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "cJSON.h"

#include "wire_chunk_fifo.h"
#include "server_flac_encoder.h"
#include "snapserver.h"

#include "buffer.h"

#define PORT                        CONFIG_SNAPSERVER_PORT
#define KEEPALIVE_IDLE              CONFIG_SNAPSERVER_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_SNAPSERVER_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_SNAPSERVER_KEEPALIVE_COUNT

static const char *TAG = "snapserver";


static const uint32_t buffer_ms = 758;




#define BASE_MESSAGE_SIZE 26
#define TIME_MESSAGE_SIZE 8



static snapclient_t *clientList = NULL;

/**
 *
 */
esp_err_t deserialize_base_msg(char *p_data, size_t len, base_message_t *p_baseMsg) {
	uint16_t tmp16;
	uint32_t tmp_u32;
	int32_t tmp_i32;
	int64_t timestamp;

	if ((p_data == NULL) || (len > BASE_MESSAGE_SIZE)) {
		return ESP_ERR_INVALID_ARG;
	}

	if ((uint8_t)p_data[0] > SNAPCAST_MESSAGE_STREAM_TAGS) {
		return ESP_FAIL;
	}

	memcpy(&tmp16, &p_data[0], sizeof(uint16_t));
	p_baseMsg->type = (tmp16);
	memcpy(&tmp16, &p_data[2], sizeof(uint16_t));
	p_baseMsg->id = (tmp16);
	memcpy(&tmp16, &p_data[4], sizeof(uint16_t));
	p_baseMsg->refersTo = (tmp16);
	memcpy(&tmp_i32, &p_data[6], sizeof(int32_t));
	p_baseMsg->sent.sec = (tmp_i32);
	memcpy(&tmp_i32, &p_data[10], sizeof(int32_t));
	p_baseMsg->sent.usec = (tmp_i32);
	timestamp = esp_timer_get_time();
	p_baseMsg->received.sec = timestamp / 1000000;
	p_baseMsg->received.usec = timestamp % 1000000;
	memcpy(&tmp_u32, &p_data[22], sizeof(uint32_t));
	p_baseMsg->size = (tmp_u32);

	ESP_LOGD(TAG, "base message received");
	ESP_LOGD(TAG, "\r\n{\r\n\ttype: %d\r\n\tid: %d\r\n\trefersTo: %d\r\n\tsent: %f"
				  "\r\n\treceived: %f\r\n\tsize: %lu\r\n}", 	p_baseMsg->type,
															p_baseMsg->id,
															p_baseMsg->refersTo,
															(float)p_baseMsg->sent.sec + (float)p_baseMsg->sent.usec/1000000.0,
															(float)p_baseMsg->received.sec + (float)p_baseMsg->received.usec/1000000.0,
															p_baseMsg->size);

	return ESP_OK;
}

/**
 *
 */
snapclient_t *insert_new_client(char *str, const int sock) {//, struct sockaddr_storage *p_source_addr) {
	snapclient_t *client = clientList;

	if (clientList == NULL) {
		return NULL;
	}

	// TODO: first check if we got the client already

	// find next empty slot
	while(client->next != NULL) {
		client = client->next;
	}
	// allocate new client
	client->next = (snapclient_t *)malloc(sizeof(snapclient_t));
	if (client->next == NULL) {
		return NULL;
	}
	client->next->prev = client;
	client->next->next = NULL;

	// now switch to new client and add data
	client = client->next;
	client->helloMsg = cJSON_Parse(str);
	client->latency = 0;
	client->muted = false;
	client->volume = 100;

	client->sock = sock;
//	memcpy(&client->p_source_addr, p_source_addr, sizeof(struct sockaddr_storage));

	char *rendered = cJSON_Print(client->helloMsg);
	ESP_LOGI(TAG, "JSON read:");
	ESP_LOGI(TAG, "%s", rendered);
	cJSON_free(rendered);

	// TODO: JSON sanity checking

	return client;
}

/**
 *
 */
char *deserialize_hello_msg(char *p_data, size_t len, hello_message_t *p_helloMsg) {
	uint32_t size;

	if (p_data == NULL) {
		return NULL;
	}

	memcpy(&size, &p_data[0], sizeof(uint32_t));

	ESP_LOGI(TAG, "hello message received. Size: %lu", size);

	return &p_data[4];
}

/**
 *
 */
int base_message_serialize(base_message_t *msg, char *data, uint32_t size) {
  write_buffer_t buffer;
  int result = 0;

  buffer_write_init(&buffer, data, size);

  result |= buffer_write_uint16(&buffer, msg->type);
  result |= buffer_write_uint16(&buffer, msg->id);
  result |= buffer_write_uint16(&buffer, msg->refersTo);
  result |= buffer_write_int32(&buffer, msg->sent.sec);
  result |= buffer_write_int32(&buffer, msg->sent.usec);
  result |= buffer_write_int32(&buffer, msg->received.sec);
  result |= buffer_write_int32(&buffer, msg->received.usec);
  result |= buffer_write_uint32(&buffer, msg->size);

  return result;
}

/**
 *
 */
int time_message_deserialize(time_message_t *msg, const char *data,
                             uint32_t size) {
  read_buffer_t buffer;
  int result = 0;

  buffer_read_init(&buffer, data, size);

  result |= buffer_read_int32(&buffer, &(msg->latency.sec));
  result |= buffer_read_int32(&buffer, &(msg->latency.usec));

  return result;
}

/**
 *
 */
void send_server_settings(snapclient_t *client) {
	cJSON *root;
	char *rendered;
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_SERVER_SETTINGS,
			  .id = 1,
			  .refersTo = 0,
			  .sent = {
			    .sec = esp_timer_get_time()/1000000,
			    .usec = esp_timer_get_time()%1000000,
			  },
			  .received = {
				.sec = 0,
				.usec = 0,
			  },
			  .size = 0,
	};
	char baseMsg_serialized[BASE_MESSAGE_SIZE];

	root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "bufferMs", buffer_ms);
	cJSON_AddNumberToObject(root, "latency", client->latency);
	cJSON_AddBoolToObject(root, "muted", client->muted);
	cJSON_AddNumberToObject(root, "volume", client->volume);

	rendered = cJSON_Print(root);

	ESP_LOGD(TAG, "send server settings");
	ESP_LOGD(TAG, "%s", rendered);

	baseMsg.size = sizeof(uint32_t) + strlen(rendered);

	base_message_serialize(&baseMsg, baseMsg_serialized, BASE_MESSAGE_SIZE);

	char *tmp = (char *)malloc(BASE_MESSAGE_SIZE + baseMsg.size);

	memcpy(&tmp[0], baseMsg_serialized, BASE_MESSAGE_SIZE);
	uint32_t msgLength = strlen(rendered);
	memcpy(&tmp[BASE_MESSAGE_SIZE], &msgLength, sizeof(uint32_t));
	memcpy(&tmp[BASE_MESSAGE_SIZE + sizeof(uint32_t)], rendered, msgLength);

    // send() can return less bytes than supplied length.
    // Walk-around for robust implementation.
	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
	while (to_write > 0) {
		int written = send(client->sock, tmp + (len - to_write), to_write, 0);
		if (written < 0) {
			ESP_LOGE(TAG, "Error occurred during sending: %s", strerror(errno));

			break;
		}
		to_write -= written;
	}

	free(tmp);
	cJSON_free(rendered);
	cJSON_Delete(root);

	if (errno == 0) {
		ESP_LOGI(TAG, "server settings sent");
	}
}

/**
 *
 */
esp_err_t send_codec_header(int sock) {
	esp_err_t err;
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_CODEC_HEADER,
			  .id = 2,
			  .refersTo = 0,
			  .sent = {
			    .sec = esp_timer_get_time()/1000000,
			    .usec = esp_timer_get_time()%1000000,
			  },
			  .received = {
				.sec = 0,
				.usec = 0,
			  },
			  .size = 0,
	};
	char baseMsg_serialized[BASE_MESSAGE_SIZE];
	codec_header_message_t codec_header_msg = {0, NULL, 0, NULL};
	flac_codec_header_t *flac_codecHeader = NULL;
	char *currentCodec = "flac";

	err = get_codec_header(&flac_codecHeader);
	if (err != ESP_OK) {
		return err;
	}

	codec_header_msg.codec = (char *)malloc(strlen(currentCodec) + 1);
	strcpy(codec_header_msg.codec, currentCodec);
	codec_header_msg.codec_size = strlen(codec_header_msg.codec);
	codec_header_msg.payload = flac_codecHeader->payload;
	codec_header_msg.size = flac_codecHeader->size;

	baseMsg.size =  sizeof(codec_header_msg.codec_size) + codec_header_msg.codec_size +
					sizeof(codec_header_msg.size) + codec_header_msg.size;

	base_message_serialize(&baseMsg, baseMsg_serialized, BASE_MESSAGE_SIZE);

	char *tmp = (char *)malloc(BASE_MESSAGE_SIZE + baseMsg.size);

	size_t offset = 0;
	memcpy(&tmp[offset], baseMsg_serialized, BASE_MESSAGE_SIZE);
	offset += BASE_MESSAGE_SIZE;
	memcpy(&tmp[offset], &codec_header_msg.codec_size, sizeof(codec_header_msg.codec_size));
	offset += sizeof(codec_header_msg.codec_size);
	memcpy(&tmp[offset], codec_header_msg.codec, codec_header_msg.codec_size);
	offset += codec_header_msg.codec_size;
	memcpy(&tmp[offset], &codec_header_msg.size, sizeof(codec_header_msg.size));
	offset += sizeof(codec_header_msg.size);
	memcpy(&tmp[offset], codec_header_msg.payload, codec_header_msg.size);

	free(codec_header_msg.codec);

    // send() can return less bytes than supplied length.
    // Walk-around for robust implementation.
	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
	while (to_write > 0) {
		int written = send(sock, tmp + (len - to_write), to_write, 0);
		if (written < 0) {
			ESP_LOGE(TAG, "Error occurred during sending: %s", strerror(errno));

			break;
		}
		to_write -= written;
	}

	free(tmp);

	if (errno == 0) {
		ESP_LOGI(TAG, "codec header sent");
	}

	return ESP_OK;
}

/**
 *
 */
void send_time_message(snapclient_t *client, time_message_t *msg) {
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_TIME,
			  .id = 2,
			  .refersTo = 0,
			  .sent = {
			    .sec = esp_timer_get_time()/1000000,
			    .usec = esp_timer_get_time()%1000000,
			  },
			  .received = {
				.sec = 0,
				.usec = 0,
			  },
			  .size = 0,
	};
	char baseMsg_serialized[BASE_MESSAGE_SIZE];

	baseMsg.size =  sizeof(time_message_t);

	base_message_serialize(&baseMsg, baseMsg_serialized, BASE_MESSAGE_SIZE);

	char *tmp = (char *)malloc(BASE_MESSAGE_SIZE + baseMsg.size);

	memcpy(&tmp[0], baseMsg_serialized, BASE_MESSAGE_SIZE);
	memcpy(&tmp[BASE_MESSAGE_SIZE], &(msg->latency), sizeof(msg->latency));

    // send() can return less bytes than supplied length.
    // Walk-around for robust implementation.
	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
	while (to_write > 0) {
		int written = send(client->sock, tmp + (len - to_write), to_write, 0);
		if (written < 0) {
			ESP_LOGE(TAG, "Error occurred during sending: %s", strerror(errno));

			break;
		}
		to_write -= written;
	}

	free(tmp);

	if (errno == 0) {
		ESP_LOGD(TAG, "time message sent");
	}
}

/**
 *
 */
void send_wire_chunk(int sock, wire_chunk_message_t *msg) {
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_WIRE_CHUNK,
			  .id = 0,
			  .refersTo = 0,
			  .sent = {
			    .sec = esp_timer_get_time()/1000000,
			    .usec = esp_timer_get_time()%1000000,
			  },
			  .received = {
				.sec = 0,
				.usec = 0,
			  },
			  .size = 0,
	};

	baseMsg.size = sizeof(msg->timestamp) + sizeof(msg->size) + msg->size;

	char *tmp = (char *)malloc(BASE_MESSAGE_SIZE + baseMsg.size);

	base_message_serialize(&baseMsg, tmp, BASE_MESSAGE_SIZE);

	uint32_t offset = BASE_MESSAGE_SIZE;
	memcpy(&tmp[offset], &(msg->timestamp), sizeof(msg->timestamp));
	offset += sizeof(msg->timestamp);
	memcpy(&tmp[offset], &(msg->size), sizeof(msg->size));
	offset += sizeof(msg->size);
	memcpy(&tmp[offset], msg->payload, msg->size);

    // send() can return less bytes than supplied length.
    // Walk-around for robust implementation.
	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
	while (to_write > 0) {
		int written = send(sock, tmp + (len - to_write), to_write, 0);
		if (written < 0) {
			ESP_LOGE(TAG, "Error occurred during sending: %s", strerror(errno));

			break;
		}
		to_write -= written;
	}

	free(tmp);

	if (errno == 0) {
		ESP_LOGD(TAG, "wire chunk sent");
	}
}

/**
 *
 */
void send_new_wire_chunk( void* event_handler_arg,
						  esp_event_base_t event_base,
						  int32_t event_id,
						  void* event_data)
{
	int *sock = (int *)event_handler_arg;
	wire_chunk_tailq_t *element = wire_chunk_fifo_get_newest();
	wire_chunk_message_t *newChnk = element->chunk;

	send_wire_chunk(*sock, newChnk);

//	ESP_LOGI(TAG, "sent next chunk");
}

/**
 *
 */
void send_initial_wire_chunks_all( int sock )
{
	wire_chunk_tailq_t *element = wire_chunk_fifo_get_oldest();

	while (element) {
		wire_chunk_message_t *newChnk = element->chunk;

		send_wire_chunk(sock, newChnk);

		element = wire_chunk_fifo_get_next(element);
	}
}


/**
 *
 */
static void handle_client_task(void *pvParameters)
{
	int len;
    size_t size;
    uint32_t state = 0;
    snapclient_t *newClient = NULL;
    int sock = (*(int *)pvParameters);

    do {
    	char baseMsgBuffer[BASE_MESSAGE_SIZE];

    	size = BASE_MESSAGE_SIZE;
        len = recv(sock, baseMsgBuffer, size, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: %s", strerror(errno));

            break;
        } else if (len == 0) {
            ESP_LOGW(TAG, "Connection closed");

            break;
        } else {
        	base_message_t baseMessage;
        	char *typedMsgBuffer = NULL;

//            ESP_LOGD(TAG, "Received %d bytes", len);

            deserialize_base_msg(baseMsgBuffer, size, &baseMessage);

            size = baseMessage.size;
//            ESP_LOGI(TAG, "base message size %d", size);
            typedMsgBuffer = (char *)malloc(size);
            if (typedMsgBuffer) {
				len = recv(sock, typedMsgBuffer, size, 0);
				if (len < 0) {
					ESP_LOGE(TAG, "Error occurred during receiving: %s", strerror(errno));
					free(typedMsgBuffer);

					break;
				} else if (len == 0) {
					ESP_LOGW(TAG, "Connection closed");
					free(typedMsgBuffer);

					break;
				} else {
//					ESP_LOGD(TAG, "Received %d bytes from socket %d", len, sock);

					if (state == 0) {
						hello_message_t helloMessage;
						char *tmp = typedMsgBuffer;

						char *str = deserialize_hello_msg(tmp, size, &helloMessage);
						newClient = insert_new_client(str, sock);

						send_server_settings(newClient);
						send_codec_header(sock);
//						send_initial_wire_chunks_all(sock);

						wire_chunk_fifo_register_handler(send_new_wire_chunk, &sock);

						state = 1;
					}
					else if (state == 1) {
						if (baseMessage.type == SNAPCAST_MESSAGE_TIME) {
							time_message_t msg;

							// got time message
							// calculate latency and send back
							msg.latency.sec = baseMessage.received.sec - baseMessage.sent.sec;
							msg.latency.usec = baseMessage.received.usec - baseMessage.sent.usec;

							ESP_LOGD(TAG, "time message received");

							send_time_message(newClient, &msg);
						}
					}
				}

				free(typedMsgBuffer);
            }
            else {
            	ESP_LOGE(TAG, "can't alloc memory for typed message");
            }
        }
    } while (len > 0);

    shutdown(sock, 0);
    close(sock);

    wire_chunk_fifo_unregister_handler(send_new_wire_chunk);

    vTaskDelete(NULL);
}

static void snapserver_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

	clientList = (snapclient_t *)malloc(sizeof(snapclient_t));
	if (!clientList) {
		ESP_LOGE(TAG, "can't alloc memory for clientList");

		return;
	}
	clientList->prev = NULL;
	clientList->next = NULL;

    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;
    }
#ifdef CONFIG_SNAPSERVER_IPV6
    else if (addr_family == AF_INET6) {
        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
        dest_addr_ip6->sin6_family = AF_INET6;
        dest_addr_ip6->sin6_port = htons(PORT);
        ip_protocol = IPPROTO_IPV6;
    }
#endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_SNAPSERVER_IPV4) && defined(CONFIG_SNAPSERVER_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {
        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }
#ifdef CONFIG_SNAPSERVER_IPV6
        else if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
        ESP_LOGI(TAG, "Socket accepted (%d) ip address: %s", sock, addr_str);


        char tName[32];
        sprintf(tName, "cTsk_%d", sock);
        xTaskCreate(handle_client_task, (const char *)tName, 4096, (void*)&sock, 15, NULL);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

void snapserver_init(void)
{
#ifdef CONFIG_SNAPSERVER_IPV4
    xTaskCreate(snapserver_task, "snpsrv_tsk", 4096, (void*)AF_INET, 5, NULL);
#endif
#ifdef CONFIG_SNAPSERVER_IPV6
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET6, 5, NULL);
#endif
}
