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



//static snapclient_t *clientList = NULL;

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
esp_err_t free_client(snapclient_t *client) {
	if (client) {
		if (client->helloMsg) {
			free(client->helloMsg);
		}

		if (client->msgQ) {
			vQueueDelete(client->msgQ);
		}

		free(client);
	}

	return ESP_OK;
}

/**
 *
 */
snapclient_t *create_new_client(char *str, const int sock) {//, struct sockaddr_storage *p_source_addr) {
	snapclient_t *client = (snapclient_t *)heap_caps_malloc(sizeof(snapclient_t), MALLOC_CAP_8BIT);

	if (!client) {
		ESP_LOGE(TAG, "couldn't alloc new client");

		return NULL;
	}

	// now switch to new client and add data
	client->helloMsg = cJSON_Parse(str);
	client->latency = 0;
	client->muted = false;
	client->volume = 100;

	client->sock = sock;
	client->id_counter = 0;
	client->msgQ = xQueueCreate(5, sizeof(message_queue_t));

	char *rendered = cJSON_Print(client->helloMsg);
	ESP_LOGI(TAG, "JSON read:");
	ESP_LOGI(TAG, "%s", rendered);
	cJSON_free(rendered);

	// TODO: JSON sanity checking

	return client;


//	snapclient_t *client = clientList;
//
//	if (clientList == NULL) {
//		return NULL;
//	}
//
//	// TODO: first check if we got the client already
//
//	// find next empty slot
//	while(client->next != NULL) {
//		client = client->next;
//	}
//	// allocate new client
//	client->next = (snapclient_t *)malloc(sizeof(snapclient_t));
//	if (client->next == NULL) {
//		return NULL;
//	}
//	client->next->prev = client;
//	client->next->next = NULL;
//
//	// now switch to new client and add data
//	client = client->next;
//	client->helloMsg = cJSON_Parse(str);
//	client->latency = 0;
//	client->muted = false;
//	client->volume = 100;
//
//	client->sock = sock;
//	client->id_counter = 0;
//
//	char *rendered = cJSON_Print(client->helloMsg);
//	ESP_LOGI(TAG, "JSON read:");
//	ESP_LOGI(TAG, "%s", rendered);
//	cJSON_free(rendered);
//
//	// TODO: JSON sanity checking
//
//	return client;
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
 * @brief Utility to log socket errors
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket number
 * @param[in] err Socket errno
 * @param[in] message Message to print
 */
static inline void log_socket_error(const char *tag, const int sock, const int err, const char *message)
{
    ESP_LOGE(tag, "[sock=%d]: %s\n"
                  "error=%d: %s", sock, message, err, strerror(err));
}


/**
 * @brief Tries to receive data from specified sockets in a non-blocking way,
 *        i.e. returns immediately if no data.
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket for reception
 * @param[out] data Data pointer to write the received data
 * @param[in] max_len Maximum size of the allocated space for receiving data
 * @return
 *          >0 : Size of received data
 *          =0 : No data available
 *          -1 : Error occurred during socket read operation
 *          -2 : Socket is not connected, to distinguish between an actual socket error and active disconnection
 */
static int try_receive(const char *tag, const int sock, char * data, size_t max_len)
{
	int rxLen = 0;

	do {
		int len = recv(sock, &data[rxLen], max_len - rxLen, 0);
		if (len < 0) {
			if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
				vTaskDelay(pdMS_TO_TICKS(5));

				continue;
			}
			else {
				if (errno == ENOTCONN) {
					ESP_LOGW(tag, "[sock=%d]: Connection closed", sock);
					return -2;  // Socket has been disconnected
				}

				log_socket_error(tag, sock, errno, "Error occurred during receiving");

				return -1;
			}
		}

		rxLen += len;
	} while(rxLen < max_len);

    return rxLen;
}

/**
 * @brief Sends the specified data to the socket. This function blocks until all bytes got sent.
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket to write data
 * @param[in] data Data to be written
 * @param[in] len Length of the data
 * @return
 *          >0 : Size the written data
 *          -1 : Error occurred during socket write operation
 */
static int socket_send(const char *tag, const int sock, const char * data, const size_t len)
{
	uint32_t count = 0;
    int to_write = len;

    while (to_write > 0) {
        int written = send(sock, data + (len - to_write), to_write, 0);
        if (written < 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_socket_error(tag, sock, errno, "Error occurred during sending");
            return -1;
        }
        else if ((written < 0) && (errno == EAGAIN)) {
        	count++;
        	if (count > 50) {
        		return -1;
        	}
        }

        ESP_LOGI(TAG, "%d %d", written, errno);
        if (written > 0) {
        	to_write -= written;
        }
    }
    return len;
}

/**
 *
 */
esp_err_t send_server_settings(snapclient_t *client, base_message_t *bMsgRx) {
	cJSON *root;
	char *rendered;
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_SERVER_SETTINGS,
			  .id = client->id_counter++,
			  .refersTo = bMsgRx->id,
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
	esp_err_t err = ESP_OK;

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

	char *tmp = (char *)heap_caps_malloc(BASE_MESSAGE_SIZE + baseMsg.size, MALLOC_CAP_8BIT);

	memcpy(&tmp[0], baseMsg_serialized, BASE_MESSAGE_SIZE);
	uint32_t msgLength = strlen(rendered);
	memcpy(&tmp[BASE_MESSAGE_SIZE], &msgLength, sizeof(uint32_t));
	memcpy(&tmp[BASE_MESSAGE_SIZE + sizeof(uint32_t)], rendered, msgLength);

    // send() can return less bytes than supplied length.
    // Walk-around for robust implementation.
//	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
//	while (to_write > 0) {
//		int written = send(client->sock, tmp + (len - to_write), to_write, 0);
//		if (written < 0) {
//			ESP_LOGE(TAG, "Error occurred during sending server settings: %s", strerror(errno));
//
//			break;
//		}
//		to_write -= written;
//	}
	if (socket_send(TAG, client->sock, tmp, len) < 0) {
		ESP_LOGE(TAG, "server settings not sent");

		err = ESP_FAIL;
	}
	else {
		ESP_LOGI(TAG, "server settings sent");
	}

	free(tmp);
	cJSON_free(rendered);
	cJSON_Delete(root);

	return err;
}

/**
 *
 */
esp_err_t send_codec_header(snapclient_t *client, base_message_t *bMsgRx) {
	esp_err_t err = ESP_OK;
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_CODEC_HEADER,
			  .id = client->id_counter++,
			  .refersTo = bMsgRx->id,
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

	codec_header_msg.codec = (char *)heap_caps_malloc(strlen(currentCodec) + 1, MALLOC_CAP_8BIT);
	strcpy(codec_header_msg.codec, currentCodec);
	codec_header_msg.codec_size = strlen(codec_header_msg.codec);
	codec_header_msg.payload = flac_codecHeader->payload;
	codec_header_msg.size = flac_codecHeader->size;

	baseMsg.size =  sizeof(codec_header_msg.codec_size) + codec_header_msg.codec_size +
					sizeof(codec_header_msg.size) + codec_header_msg.size;

	base_message_serialize(&baseMsg, baseMsg_serialized, BASE_MESSAGE_SIZE);

	char *tmp = (char *)heap_caps_malloc(BASE_MESSAGE_SIZE + baseMsg.size, MALLOC_CAP_8BIT);

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
//	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
//	while (to_write > 0) {
//		int written = send(client->sock, tmp + (len - to_write), to_write, 0);
//		if (written < 0) {
//			ESP_LOGE(TAG, "Error occurred during sending codec header: %s", strerror(errno));
//
//			break;
//		}
//		to_write -= written;
//	}
	if (socket_send(TAG, client->sock, tmp, len) < 0) {
		ESP_LOGE(TAG, "codec header not sent");

		err = ESP_FAIL;
	}
	else {
		ESP_LOGI(TAG, "codec header sent");
	}

	free(tmp);

	return err;
}

/**
 *
 */
esp_err_t send_time_message(snapclient_t *client, time_message_t *msg, base_message_t *bMsgRx) {
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_TIME,
			  .id = client->id_counter++,
			  .refersTo = bMsgRx->id,
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
	esp_err_t err = ESP_OK;

	baseMsg.size =  sizeof(time_message_t);

	base_message_serialize(&baseMsg, baseMsg_serialized, BASE_MESSAGE_SIZE);

	char *tmp = (char *)heap_caps_malloc(BASE_MESSAGE_SIZE + baseMsg.size, MALLOC_CAP_8BIT);
	if (!tmp) {
		ESP_LOGE(TAG, "couldn't malloc buffer for time message");

		return ESP_FAIL;
	}

	memcpy(&tmp[0], baseMsg_serialized, BASE_MESSAGE_SIZE);
	memcpy(&tmp[BASE_MESSAGE_SIZE], &(msg->latency), sizeof(msg->latency));

    // send() can return less bytes than supplied length.
    // Walk-around for robust implementation.
//	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
//	while (to_write > 0) {
//		int written = send(client->sock, tmp + (len - to_write), to_write, 0);
//		if (written < 0) {
//			ESP_LOGE(TAG, "Error (%d) occurred during sending time message: %s", errno, strerror(errno));
//
//			break;
//		}
//		to_write -= written;
//	}
	if (socket_send(TAG, client->sock, tmp, len) < 0) {
		ESP_LOGE(TAG, "time message not sent");

		err = ESP_FAIL;
	}
	else {
		ESP_LOGD(TAG, "time message sent");
	}

	free(tmp);

	return err;
}

/**
 *
 */
esp_err_t send_wire_chunk(snapclient_t *client, wire_chunk_message_t *msg) {
	base_message_t baseMsg = {
			  .type = SNAPCAST_MESSAGE_WIRE_CHUNK,
			  .id = client->id_counter++,
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
	esp_err_t err = ESP_OK;

	baseMsg.size = sizeof(msg->timestamp) + sizeof(msg->size) + msg->size;

	char *tmp = (char *)heap_caps_malloc(BASE_MESSAGE_SIZE + baseMsg.size, MALLOC_CAP_8BIT);

	base_message_serialize(&baseMsg, tmp, BASE_MESSAGE_SIZE);

	uint32_t offset = BASE_MESSAGE_SIZE;
	memcpy(&tmp[offset], &(msg->timestamp), sizeof(msg->timestamp));
	offset += sizeof(msg->timestamp);
	memcpy(&tmp[offset], &(msg->size), sizeof(msg->size));
	offset += sizeof(msg->size);
	memcpy(&tmp[offset], msg->payload, msg->size);

    // send() can return less bytes than supplied length.
    // Walk-around for robust implementation.
//	int to_write = BASE_MESSAGE_SIZE + baseMsg.size;
	int len = BASE_MESSAGE_SIZE + baseMsg.size;
//	while (to_write > 0) {
//		int written = send(client->sock, tmp + (len - to_write), to_write, 0);
//		if (written < 0) {
//			ESP_LOGE(TAG, "Error (%d) occurred during sending wire chunk: %s", errno, strerror(errno));
//
//			err = ESP_FAIL;
//
//			break;
//		}
//		to_write -= written;
//	}
	if (socket_send(TAG, client->sock, tmp, len) < 0) {
		ESP_LOGE(TAG, "wire chunk not sent");

		err = ESP_FAIL;
	}
	else {
		ESP_LOGD(TAG, "wire chunk sent");
	}

	free(tmp);

	return err;
}

/**
 *
 */
void send_initial_wire_chunks_all(snapclient_t *client)
{
	wire_chunk_tailq_t *element;

	wire_chunk_fifo_lock();

	element = wire_chunk_fifo_get_oldest();
	while (element) {
		wire_chunk_message_t *newChnk = element->chunk;

		send_wire_chunk(client, newChnk);

		element = wire_chunk_fifo_get_next(element);
	}

	wire_chunk_fifo_unlock();
}

void send_new_wire_chunk_task( void *pvParameter)
{
//	int sock = *((int *)pvParameter);
	snapclient_t *client = (snapclient_t *)pvParameter;
	esp_err_t err = ESP_OK;
//	TaskHandle_t handle = xTaskGetCurrentTaskHandle();
//	TaskStatus_t taskDetails;

	// Use the handle to obtain further information about the task.
//	vTaskGetInfo( handle, &taskDetails, pdTRUE,  eInvalid );

	ESP_LOGI(TAG, "started %s: sock %d", __func__, client->sock);

	send_initial_wire_chunks_all(client);

	wire_chunk_fifio_register_client(client);

	do {
		wire_chunk_tailq_t *element;
		wire_chunk_message_t *newChnk;
//		uint32_t val;
		message_queue_t msg;

		xQueueReceive(client->msgQ, &msg, portMAX_DELAY);

		if (msg.type == SNAPCAST_MESSAGE_WIRE_CHUNK)
		{
			wire_chunk_fifo_lock();
			element = wire_chunk_fifo_get_newest();
			if (element) {
				newChnk = element->chunk;
				err = send_wire_chunk(client, newChnk);
			}
			wire_chunk_fifo_unlock();

//			ESP_LOGI(TAG, "sent wire chunk from task %d", client->sock);
		}
		else if (msg.type == SNAPCAST_MESSAGE_TIME) {
			time_message_t timeMsg;

			// got time message
			// calculate latency and send back
			timeMsg.latency.sec = msg.baseMsg.received.sec - msg.baseMsg.sent.sec;
			timeMsg.latency.usec = msg.baseMsg.received.usec - msg.baseMsg.sent.usec;

			err = send_time_message(client, &timeMsg, &msg.baseMsg);
		}
	} while(err == ESP_OK);
//	ESP_LOGI(TAG, "sent next chunk");

	wire_chunk_fifio_unregister_client(client);

	vTaskDelete(NULL);
}

/**
 *
 */
static void handle_client_task(void *pvParameters)
{
	int len;
    size_t size;
    uint32_t state = 0;
    snapclient_t *client = NULL;
    int sock = (*(int *)pvParameters);
    char *tName = pcTaskGetName(xTaskGetCurrentTaskHandle() );
    TaskHandle_t client_tx_task_handle;

    ESP_LOGI(TAG, "Task %s started", tName);

    do {
    	char baseMsgBuffer[BASE_MESSAGE_SIZE];

    	size = BASE_MESSAGE_SIZE;
//        len = recv(sock, baseMsgBuffer, size, 0);
        len = try_receive(TAG, sock , baseMsgBuffer, size);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: %s", strerror(errno));

            break;
        } else if (len == 0) {
            ESP_LOGI(TAG, "no data to rx 1");

        	vTaskDelay(pdMS_TO_TICKS(50));

//            break;
        } else {
        	base_message_t baseMessage;
        	char *typedMsgBuffer = NULL;

//            ESP_LOGD(TAG, "Received %d bytes", len);

            deserialize_base_msg(baseMsgBuffer, size, &baseMessage);

            size = baseMessage.size;
//            ESP_LOGI(TAG, "base message size %d", size);
            typedMsgBuffer = (char *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
            if (typedMsgBuffer) {
            	do {
					//len = recv(sock, typedMsgBuffer, size, 0);
					len = try_receive(TAG, sock , typedMsgBuffer, size);
					if (len < 0) {
						ESP_LOGE(TAG, "Error occurred during receiving: %s", strerror(errno));
//						free(typedMsgBuffer);

						break;
					} else if (len == 0) {
						ESP_LOGI(TAG, "no data to rx 2");

						vTaskDelay(pdMS_TO_TICKS(50));
					} else {
						ESP_LOGD(TAG, "Received (typed msg) %d bytes from socket %d", len, sock);

						switch (baseMessage.type) {
							case SNAPCAST_MESSAGE_HELLO:
							{
								hello_message_t helloMessage;
								char *tmp = typedMsgBuffer;

								char *str = deserialize_hello_msg(tmp, size, &helloMessage);
								client = create_new_client(str, sock);

								send_server_settings(client, &baseMessage);
								send_codec_header(client, &baseMessage);

								char tName[32];
								sprintf(tName, "nWcT_%d", sock);
								xTaskCreatePinnedToCore(send_new_wire_chunk_task, (const char *)tName, 2 * 2048, (void*)client, 10, &client_tx_task_handle, tskNO_AFFINITY);

								break;
							}

							case SNAPCAST_MESSAGE_TIME:
							{
								message_queue_t msg;

								// got time message
								ESP_LOGD(TAG, "time message received");

								// pass on to tx task to issue response
								msg.type = SNAPCAST_MESSAGE_TIME;
								memcpy(&msg.baseMsg, &baseMessage, sizeof(baseMessage));
								xQueueSend(client->msgQ, &msg, portMAX_DELAY);

								break;
							}

							default:
							{
								ESP_LOGI(TAG, "message received %d", baseMessage.type);

								break;
							}
						}

						break;	// continue with base message decoding now
					}
				} while(len >= 0);

				free(typedMsgBuffer);
            }
            else {
            	ESP_LOGE(TAG, "can't alloc memory (%d bytes) for typed message %d", size, baseMessage.type);
            }
        }
    } while (len >= 0);

    ESP_LOGI(TAG, "shutdown socket %d", sock);

    shutdown(sock, 0);
    close(sock);

    // wait for task to be deleted by itself
    if (client_tx_task_handle) {
		while (eTaskStateGet(client_tx_task_handle) != eDeleted) {
			vTaskDelay(pdMS_TO_TICKS(5));
		}
    }

    free_client(client);

	ESP_LOGW(TAG, "internal free %d, block %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
									            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
	ESP_LOGW(TAG, "external free %d, block %d", heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
												heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    vTaskDelete(NULL);
}

/**
 * @brief Returns the string representation of client's address (accepted on this server)
 */
static char* get_clients_address(struct sockaddr_storage *source_addr)
{
    static char address_str[128];
    char *res = NULL;

    // Convert ip address to string
    if (source_addr->ss_family == PF_INET) {
        res = inet_ntoa_r(((struct sockaddr_in *)source_addr)->sin_addr, address_str, sizeof(address_str) - 1);
    }

    if (!res) {
        address_str[0] = '\0'; // Returns empty string if conversion didn't succeed
    }
    return address_str;
}

/**
 *
 */
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

//	clientList = (snapclient_t *)malloc(sizeof(snapclient_t));
//	if (!clientList) {
//		ESP_LOGE(TAG, "can't alloc memory for clientList");
//
//		return;
//	}
//	clientList->prev = NULL;
//	clientList->next = NULL;

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

    // Marking the socket as non-blocking
    int flags = fcntl(listen_sock, F_GETFL);
    if (fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_socket_error(TAG, listen_sock, errno, "Unable to set socket non blocking");
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket marked as non blocking");

//    int opt = 1;
////    int timeout = 1000;
//    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
////    setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
////    setsockopt(listen_sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
//#if defined(CONFIG_SNAPSERVER_IPV4) && defined(CONFIG_SNAPSERVER_IPV6)
//    // Note that by default IPV6 binds to both protocols, it is must be disabled
//    // if both protocols used at the same time (used in CI)
//    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
//#endif

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

    // We accept a new connection only if we have a free socket
    while (1) {
        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock;

        // Try to accept a new connections
        sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            if (errno == EWOULDBLOCK) { // The listener socket did not accepts any connection
                                        // continue to serve open connections and try to accept again upon the next iteration
                ESP_LOGV(TAG, "No pending connections...");
            } else {
                log_socket_error(TAG, listen_sock, errno, "Error when accepting connection");
//                goto CLEAN_UP;
            }
        } else {
            // We have a new client connected -> print it's address
            ESP_LOGI(TAG, "[sock=%d]: Connection accepted from IP:%s", sock, get_clients_address(&source_addr));

            // ...and set the client's socket non-blocking
            flags = fcntl(sock, F_GETFL);
            if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
                log_socket_error(TAG, sock, errno, "Unable to set socket non blocking");
                goto CLEAN_UP;
            }
            ESP_LOGI(TAG, "[sock=%d]: Socket marked as non blocking", sock);

        	ESP_LOGW(TAG, "internal free %d, block %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        									            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        	ESP_LOGW(TAG, "external free %d, block %d", heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    													heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            char tName[32];
            sprintf(tName, "cTsk_%d", sock);
            if (xTaskCreatePinnedToCore(handle_client_task, (const char *)tName, 4096, (void*)&sock, 10, NULL, tskNO_AFFINITY) != pdPASS) {
            	ESP_LOGE(TAG, "Task %s couldn't be created", tName);

                shutdown(sock, 0);
                close(sock);
            }
        }

        // yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(100));
    }

//    while (1) {
//        ESP_LOGI(TAG, "Socket listening");
//
//        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
//        socklen_t addr_len = sizeof(source_addr);
//        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
//        if (sock < 0) {
//            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
//            continue;
//        }
//
//        // Set tcp keepalive option
//        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
//        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
//        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
//        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
//        // Convert ip address to string
//        if (source_addr.ss_family == PF_INET) {
//            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
//        }
//#ifdef CONFIG_SNAPSERVER_IPV6
//        else if (source_addr.ss_family == PF_INET6) {
//            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
//        }
//#endif
//        ESP_LOGI(TAG, "Socket accepted (%d) ip address: %s", sock, addr_str);
//
//    	ESP_LOGW(TAG, "internal free %d, block %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
//    									            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
//    	ESP_LOGW(TAG, "external free %d, block %d", heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
//													heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
//        char tName[32];
//        sprintf(tName, "cTsk_%d", sock);
//        if (xTaskCreatePinnedToCore(handle_client_task, (const char *)tName, 4096, (void*)&sock, 10, NULL, tskNO_AFFINITY) != pdPASS) {
//        	ESP_LOGE(TAG, "Task %s couldn't be created", tName);
//
//            shutdown(sock, 0);
//            close(sock);
//        }
//    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

void snapserver_init(void)
{
    xTaskCreatePinnedToCore(snapserver_task, "snpsrv_tsk", 4096, (void*)AF_INET, 5, NULL, 1);
}
