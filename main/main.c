/* DLNA Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
//#include "board.h"
//#include "esp_peripherals.h"
//#include "periph_wifi.h"

#include "audio_event_iface.h"
#include "audio_mem.h"
#include "esp_wifi.h"
#include "esp_ssdp.h"
#include "esp_dlna.h"

//#include "esp_audio.h"
//#include "esp_decoder.h"
#include "http_stream.h"
#include "raw_stream.h"
#include "audio_pipeline.h"
//#include "i2s_stream.h"
#include "media_lib_adapter.h"
#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "FLAC/metadata.h"
#include "FLAC/stream_encoder.h"

#include "wire_chunk_fifo.h"
#include "server_flac_encoder.h"
#include "snapserver.h"

static const char *TAG = "dlna";

#define DLNA_UNIQUE_DEVICE_NAME "ESP32_DMR_8db0797a"
#define DLNA_DEVICE_UUID "8db0797a-f01a-4949-8f59-51188b181809"
#define DLNA_ROOT_PATH "/rootDesc.xml"

static esp_dlna_handle_t  dlna_handle = NULL;

static int vol, mute;
static char *track_uri = NULL;
static const char *trans_state = "STOPPED";

static audio_pipeline_handle_t pipeline = NULL;
static audio_element_handle_t raw_stream_reader = NULL;
static audio_element_handle_t http_stream_reader = NULL;

static audio_event_iface_handle_t evt;

#define VERSION	"x.x.x"

#define READSIZE 1152

//static unsigned total_samples = 0; /* can use a 32-bit number due to WAVE size limitations */
//static FLAC__byte buffer[READSIZE/*samples*/ * 2/*bytes_per_sample*/ * 2/*channels*/]; /* we read the WAVE data into here */
static FLAC__int32 pcmBuffer[READSIZE/*samples*/ * 2/*channels*/];

static int64_t lastTimestamp = 0;

#define PORT                        CONFIG_EXAMPLE_PORT
#define KEEPALIVE_IDLE              CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_EXAMPLE_KEEPALIVE_COUNT

#define WIFI_SSID							CONFIG_WIFI_SSID
#define WIFI_PASSWORD						CONFIG_WIFI_PASSWORD
#define ESP_MAXIMUM_RETRY					CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static int s_retry_num = 0;

static uint32_t cnt = 0;

static int sock = -1;

static bool connected = false;

static char rx_buffer[READSIZE*4];

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define SNAPSERVER_BUFFER_MS	500

#define CODEC_HEADER_PAYLOAD_LEN	1361	// TODO: is this really always this big, how to find this value dynamically?

static uint32_t sample_rate = 44100;
static uint32_t channels = 2;
static uint32_t bps = 16;

static uint32_t codecHeaderCounter = 0;
static FLAC__StreamEncoder *encoder = NULL;
static FLAC__StreamMetadata *metadata[2] = {NULL, NULL};
static flac_codec_header_t flac_codecHeader = {0, NULL};
static TaskHandle_t flac_encoder_task_handle = NULL;

void flac_encoder_deinit(void);
void flac_encoder_init(void);



/**
 *
 */
esp_err_t get_codec_header(flac_codec_header_t **p_cHeader) {
	if ((flac_codecHeader.payload == NULL) || (flac_codecHeader.size == 0)) {
		*p_cHeader = NULL;

		return ESP_ERR_NOT_FOUND;
	}
	else {
		*p_cHeader = &flac_codecHeader;
//		ESP_LOGI(TAG, "%s", __func__);
//		ESP_LOG_BUFFER_HEX(TAG, p_cHeader->payload, p_cHeader->size);

		return ESP_OK;
	}
}

/**
 *
 */
FLAC__StreamEncoderWriteStatus write_callback(const FLAC__StreamEncoder* encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples,
		uint32_t current_frame, void* client_data)
{
	uint32_t flackBlockSize = FLAC__stream_encoder_get_blocksize(encoder);

	if (codecHeaderCounter < CODEC_HEADER_PAYLOAD_LEN) {
		char *newHeader;
		uint32_t prevHeaderCounter = codecHeaderCounter;

		codecHeaderCounter += bytes;

		if (flac_codecHeader.payload == NULL) {
			flac_codecHeader.payload = (char *)malloc(codecHeaderCounter);
			memcpy(flac_codecHeader.payload, buffer, bytes);
		}
		else {
			newHeader = (char *)malloc(codecHeaderCounter);
			memcpy(&newHeader[0], flac_codecHeader.payload, prevHeaderCounter);
			free(flac_codecHeader.payload);
			flac_codecHeader.payload = newHeader;
			memcpy(&flac_codecHeader.payload[prevHeaderCounter], buffer, bytes);
		}

		flac_codecHeader.size = codecHeaderCounter;

//		if (codecHeaderCounter == CODEC_HEADER_PAYLOAD_LEN) {
//			ESP_LOG_BUFFER_HEX(TAG, flac_codecHeader.payload, codecHeaderCounter);
//		}
	}
	else {
		wire_chunk_message_t *newChnk;

		newChnk = (wire_chunk_message_t *)malloc(sizeof(wire_chunk_message_t));
		newChnk->size = bytes;
		newChnk->payload = (char *)malloc(bytes);
		memcpy(newChnk->payload, buffer, bytes);

		wire_chunk_tailq_t *newElement = (wire_chunk_tailq_t *)malloc( sizeof(wire_chunk_tailq_t) );
		newElement->chunk = newChnk;

		if (wire_chunk_fifo_empty() == true) {
			newElement->chunk->timestamp.sec = (esp_timer_get_time() + 100000) / 1000000;
			newElement->chunk->timestamp.usec = (esp_timer_get_time() + 100000) % 1000000;

//			ESP_LOGI(TAG, "first time: %ld.%ld", newElement->chunk->timestamp.sec, newElement->chunk->timestamp.usec);
		}
		else {
			wire_chunk_tailq_t *element = wire_chunk_fifo_get_newest();
			int64_t usec, offset_us;

			offset_us = ((int64_t)flackBlockSize * 1000000LL) / (int64_t)sample_rate;	// TODO: could be calculated during init phase
			usec = (int64_t)element->chunk->timestamp.sec * 1000000LL +  (int64_t)element->chunk->timestamp.usec + offset_us;

			newElement->chunk->timestamp.sec = usec / 1000000;
			newElement->chunk->timestamp.usec = usec % 1000000;

			//ESP_LOGI(TAG, "time: %ld.%ld, offset %lldus", newElement->chunk->timestamp.sec, newElement->chunk->timestamp.usec, offset_us);
		}

		ESP_ERROR_CHECK( wire_chunk_fifo_insert(newElement) );


//		xQueueSend(flacChkQHdl, &newChnk, portMAX_DELAY);
	//	ESP_LOGI(TAG, "%s: got %d bytes of encoded data, current_frame %ld, samples %ld", __func__, bytes, current_frame, samples);
//		ESP_LOGI(TAG, "bytes: %d, %lld", bytes, esp_timer_get_time() - lastTimestamp);
//		lastTimestamp = esp_timer_get_time();
	}

    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static void flac_encoder_task(void *pvParameters) {
	int64_t localLastTimestamp = 0;
	FLAC__bool ok = true;

	if (!raw_stream_reader) {
		ESP_LOGE(TAG, "raw_stream_reader == NULL");

		return;
	}

	if (! encoder) {
		ESP_LOGE(TAG, "flac encoder not created");

		return;
	}

	ESP_LOGW(TAG, "frames: %ld", FLAC__stream_encoder_get_blocksize(encoder));

	uint32_t rounds = 0;

//	ringbuf_handle_t rb = audio_element_get_input_ringbuf(raw_stream_reader);
//	ESP_LOGI(TAG, "got rb %p handle", rb);
//	bool start = false;

	while (1) {
	    int16_t *item;
    	int16_t buffer[READSIZE * 2];
    	int length;

//    	ESP_LOGI(TAG, "rb_bytes_filled == %d", rb_bytes_filled(rb));
//    	if (start == false) {
//    		// wait for buffer to be about 0.5s full
//    		if (rb_bytes_filled(rb) >= ((sample_rate * (bps / 8) * channels) * 1 / 2)) {
//    			start = true;
//
//    			// TODO: find restart condition if dlna gets disconnected this needs to be refilled again
//    		}
//
//    		vTaskDelay(pdMS_TO_TICKS(100));
//
//    		continue;
//    	}

		length = raw_stream_read(raw_stream_reader, (char *)buffer, sizeof(buffer));
//		ESP_LOGI(TAG, "got %d bytes of data", length);
//		ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, sizeof(buffer), ESP_LOG_INFO);
		if (length > 0) {
			item = buffer;
//			ESP_LOGI(TAG, "bytes: %d, %lld", length, esp_timer_get_time() - localLastTimestamp);
			localLastTimestamp = esp_timer_get_time();

	//	    rounds++;

			//Check received data
			if (item != NULL) {
				for (int i=0; i<length/2; i+=2*4){
					pcmBuffer[i] = (FLAC__int32)item[i];		// ch1
					pcmBuffer[i+1] = (FLAC__int32)item[i+1];	// ch2
					pcmBuffer[i+2] = (FLAC__int32)item[i+2];	// ch1
					pcmBuffer[i+3] = (FLAC__int32)item[i+3];	// ch2
					pcmBuffer[i+4] = (FLAC__int32)item[i+4];	// ch1
					pcmBuffer[i+5] = (FLAC__int32)item[i+5];	// ch2
					pcmBuffer[i+6] = (FLAC__int32)item[i+6];	// ch1
					pcmBuffer[i+7] = (FLAC__int32)item[i+7];	// ch2
				}


				if (FLAC__stream_encoder_process_interleaved(encoder, pcmBuffer, length/4) == false)
				{
					ESP_LOGE(TAG, "error state: %s", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder)]);
				}

	//	        if (rounds > 3) {
	//	        	rounds = 0;
	//	        	vTaskDelay(1);
	//	    	}
			}

//			vTaskDelay(pdMS_TO_TICKS(26));
	    } else {
	        //Failed to receive item
//	    	ESP_LOGE(TAG, "Failed to receive item\n");
	    }
	}

	ok &= FLAC__stream_encoder_finish(encoder);

	ESP_LOGI(TAG, "encoding: %s\n", ok? "succeeded" : "FAILED");
	ESP_LOGI(TAG, "   state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder)]);

	/* now that encoding is finished, the metadata can be freed */
	FLAC__metadata_object_delete(metadata[0]);
	FLAC__metadata_object_delete(metadata[1]);

	FLAC__stream_encoder_delete(encoder);
}

//static void tcp_server_tx_task(void *pvParameters) {
//	while (1) {
//	    size_t bytes;
//
////	    while(connected == false) {
////	    	vTaskDelay(1);
////	    }
//
//	    FLAC__byte *item = (FLAC__byte *)xRingbufferReceiveUpTo(ringbuf_handle_flac_data, &bytes, portMAX_DELAY, READSIZE / 2);
//
//	    //Check received data
//	    if (item != NULL) {
//            // send() can return less bytes than supplied length.
//            // Walk-around for robust implementation.
//            int to_write = bytes;
//            while ((to_write > 0) && (connected == true)) {
//                int written = send(sock, item + (bytes - to_write), to_write, 0);
//                if (written < 0) {
//                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
//
//                    connected = false;
//                }
//                else {
////                	ESP_LOGI(TAG, "wrote %d bytes of data", written);
//
//                	to_write -= written;
//                }
//            }
//
//	    	vRingbufferReturnItem(ringbuf_handle_flac_data, (void *)item);
//
//	    } else {
//	        //Failed to receive item
//	    	ESP_LOGE(TAG, "%s: Failed to receive item", __func__);
//	    }
//	}
//}
//
//static void do_retransmit(const int sock) {
//    int len;
//
//    do {
//        len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
//        if (len < 0) {
//            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
//            cnt = 0;
//        } else if (len == 0) {
//            ESP_LOGW(TAG, "Connection closed");
//            cnt = 0;
//        } else {
//
////            //Send an item
////            UBaseType_t res = xRingbufferSend(ringbuf_handle_pcm_data, rx_buffer, sizeof(rx_buffer), portMAX_DELAY);
////            if (res != pdTRUE) {
////            	ESP_LOGE(TAG, "Failed to send item to ringbuf_handle_pcm_data");
////            }
//        }
//    } while (len > 0);
//}
//
//
//
//static void tcp_server_rx_task(void *pvParameters)
//{
//    char addr_str[128];
//    int addr_family = (int)pvParameters;
//    int ip_protocol = 0;
//    int keepAlive = 1;
//    int keepIdle = KEEPALIVE_IDLE;
//    int keepInterval = KEEPALIVE_INTERVAL;
//    int keepCount = KEEPALIVE_COUNT;
//    struct sockaddr_storage dest_addr;
//
//    if (addr_family == AF_INET) {
//        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
//        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
//        dest_addr_ip4->sin_family = AF_INET;
//        dest_addr_ip4->sin_port = htons(PORT);
//        ip_protocol = IPPROTO_IP;
//    }
//#ifdef CONFIG_EXAMPLE_IPV6
//    else if (addr_family == AF_INET6) {
//        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
//        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
//        dest_addr_ip6->sin6_family = AF_INET6;
//        dest_addr_ip6->sin6_port = htons(PORT);
//        ip_protocol = IPPROTO_IPV6;
//    }
//#endif
//
//    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
//    if (listen_sock < 0) {
//        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
//        vTaskDelete(NULL);
//        return;
//    }
//    int opt = 1;
//    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
//#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
//    // Note that by default IPV6 binds to both protocols, it is must be disabled
//    // if both protocols used at the same time (used in CI)
//    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
//#endif
//
//    ESP_LOGI(TAG, "Socket created");
//
//    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
//    if (err != 0) {
//        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
//        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
//        goto CLEAN_UP;
//    }
//    ESP_LOGI(TAG, "Socket bound, port %d", PORT);
//
//    err = listen(listen_sock, 1);
//    if (err != 0) {
//        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
//        goto CLEAN_UP;
//    }
//
//    while (1) {
//
//        ESP_LOGI(TAG, "Socket listening");
//
//        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
//        socklen_t addr_len = sizeof(source_addr);
//        sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
//        if (sock < 0) {
//            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
//            break;
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
//#ifdef CONFIG_EXAMPLE_IPV6
//        else if (source_addr.ss_family == PF_INET6) {
//            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
//        }
//#endif
//        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);
//
//        connected = true;
//
//        do_retransmit(sock);
//
//        shutdown(sock, 0);
//        close(sock);
//        sock = -1;
//        connected = false;
//    }
//
//CLEAN_UP:
//    close(listen_sock);
//    vTaskDelete(NULL);Semaphore count %d, diff %lld", uxSemaphoreGetCount(wire_chunk_fifo.countSemaphore), esp_timer_get_time() - oldTime);
//	oldTime = esp_timer_get_time();

//}

int renderer_request(esp_dlna_handle_t dlna, const upnp_attr_t *attr, int attr_num, char *buffer, int max_buffer_len)
{
    int req_type;
    int tmp_data = 0, buffer_len = 0;
    int hour = 0, min = 0, sec = 0;

    if (attr_num != 1) {
        return 0;
    }

    req_type = attr->type & 0xFF;
    switch (req_type) {
        case RCS_GET_MUTE:
            ESP_LOGD(TAG, "get mute = %d", mute);
            return snprintf(buffer, max_buffer_len, "%d", mute);
        case RCS_SET_MUTE:
            mute = atoi(buffer);
            ESP_LOGD(TAG, "set mute = %d", mute);
            if (mute) {
//                esp_audio_vol_set(player, 0);
            } else {
//                esp_audio_vol_set(player, vol);
            }
            esp_dlna_notify(dlna, "RenderingControl");
            return 0;
        case RCS_GET_VOL:
//            esp_audio_vol_get(player, &vol);
            ESP_LOGI(TAG, "get vol = %d", vol);
            return snprintf(buffer, max_buffer_len, "%d", vol);
        case RCS_SET_VOL:
            vol = atoi(buffer);
            ESP_LOGI(TAG, "set vol = %d", vol);
//            esp_audio_vol_set(player, vol);
            esp_dlna_notify(dlna, "RenderingControl");
            return 0;
        case AVT_PLAY:
            ESP_LOGI(TAG, "Play with speed=%s trans_state %s", buffer, trans_state);

//            esp_audio_state_t state = { 0 };
//            esp_audio_state_get(player, &state);
            //if (state.status == AUDIO_STATUS_PAUSED)
//            {
//                esp_audio_resume(player);
//                esp_dlna_notify(dlna, "AVTransport");
//                break;
//            } else if (track_uri != NULL) {
//                esp_audio_play(player, AUDIO_CODEC_TYPE_DECODER, track_uri, 0);
//                esp_dlna_notify(dlna, "AVTransport");
//            }

            audio_pipeline_wait_for_stop(pipeline);
            audio_element_info_t info;
            audio_element_getinfo(http_stream_reader, &info);
            info.uri = track_uri;
            audio_element_setinfo(http_stream_reader, &info);
            audio_pipeline_run(pipeline);

            esp_dlna_notify(dlna, "AVTransport");
            trans_state = "PLAYING";
            esp_dlna_notify_avt_by_action(dlna_handle, "TransportState");
            return 0;
        case AVT_STOP:
            ESP_LOGI(TAG, "Stop instance=%s", buffer);
//            esp_audio_stop(player, TERMINATION_TYPE_NOW);

            audio_pipeline_pause(pipeline);
            audio_pipeline_reset_elements(pipeline);
            audio_pipeline_reset_ringbuffer(pipeline);

            trans_state = "STOPPED";
            esp_dlna_notify_avt_by_action(dlna_handle, "TransportState");

            return 0;
        case AVT_PAUSE:
            ESP_LOGI(TAG, "Pause instance=%s", buffer);
            audio_pipeline_pause(pipeline);
//            esp_audio_pause(player);
//            esp_dlna_notify_avt_by_action(dlna_handle, "TransportState");
			trans_state = "PAUSED_PLAYBACK";
			esp_dlna_notify_avt_by_action(dlna_handle, "TransportState");
            return 0;
        case AVT_NEXT:
        case AVT_PREV:
//            esp_audio_stop(player, TERMINATION_TYPE_NOW);
            return 0;
        case AVT_SEEK:
            sscanf(buffer, "%d:%d:%d", &hour, &min, &sec);
            tmp_data = hour*3600 + min*60 + sec;
            ESP_LOGI(TAG, "Seekto %d s", tmp_data);
//            esp_audio_seek(player, tmp_data);
            return 0;
        case AVT_SET_TRACK_URI:
            ESP_LOGI(TAG, "SetAVTransportURI=%s", buffer);

            flac_encoder_init();

//            esp_audio_state_t state = { 0 };
//            esp_audio_state_get(player, &state);
//            if ((track_uri != NULL) && (state.status == AUDIO_STATUS_RUNNING) && strcasecmp(track_uri, buffer)) {
//                esp_audio_stop(player, TERMINATION_TYPE_NOW);
                esp_dlna_notify(dlna, "AVTransport");
//            }
            free(track_uri);
            track_uri = NULL;
            if (track_uri == NULL) {
                asprintf(&track_uri, "%s", buffer);
            }
            return 0;
        case AVT_SET_TRACK_METADATA:
            ESP_LOGD(TAG, "CurrentURIMetaData=%s", buffer);
            return 0;
        case AVT_GET_TRACK_URI:
            if (track_uri != NULL) {
                return snprintf(buffer, max_buffer_len, "%s", track_uri);
            } else {
                return 0;
            }
        case AVT_GET_PLAY_SPEED:    /* ["1"] */
            return snprintf(buffer, max_buffer_len, "%d", 1);
        case AVT_GET_PLAY_MODE:
            return snprintf(buffer, max_buffer_len, "NORMAL");
        case AVT_GET_TRANS_STATUS:  /* ["ERROR_OCCURRED", "OK"] */
            return snprintf(buffer, max_buffer_len, "OK");
        case AVT_GET_TRANS_STATE:   /* ["STOPPED", "PAUSED_PLAYBACK", "TRANSITIONING", "NO_MEDIA_PRESENT"] */
            ESP_LOGI(TAG, "_avt_get_trans_state %s", trans_state);
            return snprintf(buffer, max_buffer_len, trans_state);
        case AVT_GET_TRACK_DURATION:
        case AVT_GET_MEDIA_DURATION:
//            esp_audio_duration_get(player, &tmp_data);
//            tmp_data /= 1000;
        	tmp_data = 0;
            buffer_len = snprintf(buffer, max_buffer_len, "%02d:%02d:%02d", tmp_data / 3600, tmp_data / 60, tmp_data % 60);
            ESP_LOGD(TAG, "_avt_get_duration %s", buffer);
            return buffer_len;
        case AVT_GET_TRACK_NO:
            return snprintf(buffer, max_buffer_len, "%d", 1);
        case AVT_GET_TRACK_METADATA:
            return 0;
        // case AVT_GET_POS_ABSTIME:
        case AVT_GET_POS_RELTIME:
//            esp_audio_time_get(player, &tmp_data);
//            tmp_data /= 1000;
        	tmp_data = 0;
            buffer_len = snprintf(buffer, max_buffer_len, "%02d:%02d:%02d", tmp_data / 3600, tmp_data / 60, tmp_data % 60);
            ESP_LOGD(TAG, "_avt_get_time %s", buffer);
            return buffer_len;
        // case AVT_GET_POS_ABSCOUNT:
        case AVT_GET_POS_RELCOUNT:
//            esp_audio_pos_get(player, &tmp_data);
        	tmp_data = 0;
            buffer_len = snprintf(buffer, max_buffer_len, "%d", tmp_data);
            ESP_LOGD(TAG, "_avt_get_pos %s", buffer);
            return buffer_len;
    }
    return 0;
}

/**
 *
 */
static int _http_stream_event_handle(http_stream_event_msg_t *msg)
{
	if (msg->event_id == HTTP_STREAM_RESOLVE_ALL_TRACKS) {
        return ESP_OK;
    }
	else if (msg->event_id == HTTP_STREAM_FINISH_TRACK) {
        return http_stream_next_track(msg->el);
    }
    else if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST) {
        return http_stream_restart(msg->el);
    }

    return ESP_OK;
}

/**
 *
 */
static void audio_player_init(void)
{
//    audio_board_handle_t board_handle = audio_board_init();
//    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    // Create readers and add to esp_audio
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.event_handle = _http_stream_event_handle;
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = true;
//    http_cfg.task_prio = 10;
    http_cfg.task_core = tskNO_AFFINITY;
    http_cfg.stack_in_ext = true;
    http_cfg.out_rb_size = (44100 * 4 * 4) / 4;	// buffer for 4 * 0.25s
    http_stream_reader = http_stream_init(&http_cfg);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = (44100 * 4 * 4) / 4;	// buffer for 4 * 0.25s
    raw_stream_reader = raw_stream_init(&raw_cfg);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
//    pipeline_cfg.rb_size = READSIZE*16;
	pipeline = audio_pipeline_init(&pipeline_cfg);
	audio_pipeline_register(pipeline, http_stream_reader, "http");
	audio_pipeline_register(pipeline, raw_stream_reader, "raw");
    audio_pipeline_link(pipeline, (const char *[]) {"http", "raw"}, 2);

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);


//    audio_decoder_t auto_decode[] = {
//        DEFAULT_ESP_MP3_DECODER_CONFIG(),
//        DEFAULT_ESP_WAV_DECODER_CONFIG(),
//        DEFAULT_ESP_AAC_DECODER_CONFIG(),
//        DEFAULT_ESP_M4A_DECODER_CONFIG(),
//        DEFAULT_ESP_TS_DECODER_CONFIG(),
//    };
//    esp_decoder_cfg_t auto_dec_cfg = DEFAULT_ESP_DECODER_CONFIG();
//    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER, esp_decoder_init(&auto_dec_cfg, auto_decode, 5));
//
//    // Create writers and add to esp_audio
//    i2s_stream_cfg_t i2s_writer = I2S_STREAM_CFG_DEFAULT();
//    i2s_writer.type = AUDIO_STREAM_WRITER;
//    i2s_writer.stack_in_ext = true;
//    i2s_writer.i2s_config.sample_rate = 48000;
//    i2s_writer.task_core = 1;
//    esp_audio_output_stream_add(player, i2s_stream_init(&i2s_writer));
//
//    // Set default volume
//    esp_audio_vol_set(player, 35);
}

/**
 *
 */
static void start_dlna(void) {
    const ssdp_service_t ssdp_service[] = {
        { DLNA_DEVICE_UUID, "upnp:rootdevice",                                  NULL },
        { DLNA_DEVICE_UUID, "urn:schemas-upnp-org:device:MediaRenderer:1",      NULL },
        { DLNA_DEVICE_UUID, "urn:schemas-upnp-org:service:ConnectionManager:1", NULL },
        { DLNA_DEVICE_UUID, "urn:schemas-upnp-org:service:RenderingControl:1",  NULL },
        { DLNA_DEVICE_UUID, "urn:schemas-upnp-org:service:AVTransport:1",       NULL },
        { NULL, NULL, NULL },
    };

    ssdp_config_t ssdp_config = SSDP_DEFAULT_CONFIG();
    ssdp_config.udn = DLNA_UNIQUE_DEVICE_NAME;
    ssdp_config.location = "http://${ip}"DLNA_ROOT_PATH;
    esp_ssdp_start(&ssdp_config, ssdp_service);

    static httpd_handle_t httpd = NULL;
    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.max_uri_handlers = 25;
    httpd_config.stack_size = 6 * 1024;
    if (httpd_start(&httpd, &httpd_config) != ESP_OK) {
        ESP_LOGI(TAG, "Error starting httpd");
    }

    extern const uint8_t logo_png_start[] asm("_binary_logo_png_start");
    extern const uint8_t logo_png_end[] asm("_binary_logo_png_end");

    dlna_config_t dlna_config = {
        .friendly_name = "ESP32 MD (ESP32 Renderer)",
        .uuid = (const char *)DLNA_DEVICE_UUID,
        .logo               = {
            .mime_type  = "image/png",
            .data       = (const char *)logo_png_start,
            .size       = logo_png_end - logo_png_start,
        },
        .httpd          = httpd,
        .httpd_port     = httpd_config.server_port,
        .renderer_req   = renderer_request,
        .root_path      = DLNA_ROOT_PATH,
        .device_list    = false
    };

    dlna_handle = esp_dlna_start(&dlna_config);

    ESP_LOGI(TAG, "DLNA Started...");
}

//static void encoder_task(void *pvParameters) {
//	if (!raw_stream_reader) {
//		ESP_LOGE(TAG, "raw_stream_reader == NULL");
//
//		return;
//	}
//
//	while(1) {
//    	int16_t buffer[128];
//    	int length;
//
//		length = raw_stream_read(raw_stream_reader, (char *)buffer, sizeof(buffer));
////		ESP_LOGI(TAG, "got %d bytes of data", length);
////		ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, sizeof(buffer), ESP_LOG_INFO);
//	}
//}

void flac_encoder_deinit(void)  {
	if (flac_encoder_task_handle) {
		vTaskDelete(flac_encoder_task_handle);
		flac_encoder_task_handle = NULL;
	}

	codecHeaderCounter = 0;

	if (flac_codecHeader.payload) {
		free(flac_codecHeader.payload);
		flac_codecHeader.payload = NULL;
		flac_codecHeader.size = 0;
	}

	if (encoder) {
		FLAC__stream_encoder_finish(encoder);

		/* now that encoding is finished, the metadata can be freed */
		if (metadata[0]) {
			FLAC__metadata_object_delete(metadata[0]);
			metadata[0] = NULL;
		}
		if (metadata[1]) {
			FLAC__metadata_object_delete(metadata[1]);
			metadata[1] = NULL;
		}

		FLAC__stream_encoder_delete(encoder);

		encoder = NULL;
	}

	wire_chunk_fifo_clear();
}

/**
 *
 */
void flac_encoder_init(void) {
	uint32_t flackBlockSize;
	// need to store #SNAPSERVER_BUFFER_MS duration of samples
	size_t queue_length;// = SNAPSERVER_BUFFER_MS / (1);
	FLAC__bool ok = true;
	FLAC__StreamEncoderInitStatus init_status;
	FLAC__StreamMetadata_VorbisComment_Entry entry;

	flac_encoder_deinit();

	/* allocate the encoder */
	if((encoder = FLAC__stream_encoder_new()) == NULL) {
		ESP_LOGE(TAG, "failed allocating flac encoder");

		return;
	}

	ok &= FLAC__stream_encoder_set_verify(encoder, false);//true);	// true generates FLAC__STREAM_ENCODER_VERIFY_MISMATCH_IN_AUDIO_DATA with -O2
	ok &= FLAC__stream_encoder_set_compression_level(encoder, 1);	// 1 fastest, 8 slowest, 3 is the maximum esp32 can do in real time with default block sizes
																	// 						 5 with block size of 2*READSIZE
																	// 						 5 with block size of READSIZE
	ok &= FLAC__stream_encoder_set_channels(encoder, channels);
	ok &= FLAC__stream_encoder_set_bits_per_sample(encoder, bps);
	ok &= FLAC__stream_encoder_set_sample_rate(encoder, sample_rate);
	ok &= FLAC__stream_encoder_set_total_samples_estimate(encoder, 0);	// unknown
	ok &= FLAC__stream_encoder_set_blocksize(encoder, READSIZE);

	// now add some metadata, we'll add some tags and a padding block
	if(ok) {
		 // now add some metadata; we'll add some tags and a padding block
		    if ((metadata[0] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT)) == NULL ||
				(metadata[1] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING)) == NULL ||
		        // there are many tag (vorbiscomment) functions but these are convenient for this particular use:
		        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "TITLE", "SnapStream") ||
		        !FLAC__metadata_object_vorbiscomment_append_comment(metadata[0], entry, 0) ||
		        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "VERSION", VERSION) ||
		        !FLAC__metadata_object_vorbiscomment_append_comment(metadata[0], entry, 0))
		    {
		    	ESP_LOGE(TAG, "ERROR: out of memory or tag error");

				ok = false;

				return;
			} else {

				metadata[1]->length = 1234; /* set the padding length */

				ok = FLAC__stream_encoder_set_metadata(encoder, metadata, 2);

			}
	}

	/* initialize encoder */
	if(ok) {
		init_status = FLAC__stream_encoder_init_stream(encoder, write_callback, NULL, NULL, NULL, NULL);
		if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
			ESP_LOGE(TAG, "ERROR: initializing encoder: %s\n", FLAC__StreamEncoderInitStatusString[init_status]);
			ok = false;

			return;
		}
	}

	flackBlockSize = FLAC__stream_encoder_get_blocksize(encoder);
	queue_length = ceil((float)SNAPSERVER_BUFFER_MS / (((float)flackBlockSize / (float)sample_rate) * 1000.0));
	ESP_ERROR_CHECK( wire_chunk_fifo_init(queue_length, SNAPSERVER_BUFFER_MS) );

	xTaskCreatePinnedToCore(flac_encoder_task, "encoder", 4096*4, NULL, 9, &flac_encoder_task_handle, tskNO_AFFINITY);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
	     * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
        		WIFI_SSID, WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
        		WIFI_SSID, WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

/**
 *
 */
void app_main()
{
    esp_log_level_set("*", ESP_LOG_INFO);
//    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_WARN);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
    esp_log_level_set("ESP_AUDIO_CTRL", ESP_LOG_WARN);

    media_lib_add_default_adapter();
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    wifi_init_sta();

    audio_player_init();

    start_dlna();

    flac_encoder_init();

    snapserver_init();

//#ifdef CONFIG_EXAMPLE_IPV4
//    xTaskCreatePinnedToCore(tcp_server_rx_task, "tcp_rx", 4096, (void*)AF_INET, 5, NULL, tskNO_AFFINITY);
//    xTaskCreatePinnedToCore(tcp_server_tx_task, "tcp_tx", 4096, (void*)AF_INET, 5, NULL, tskNO_AFFINITY);
//#endif

    int16_t a = 0;
    int16_t b = 0;
    uint16_t c = 0;
    ESP_LOGE(TAG, "Sizes: %hd, %hi, %hu", a,b,c);
    ESP_LOGE(TAG, "Sizes: %u, %u, %u, %u", sizeof(void *), sizeof(long), sizeof(size_t), sizeof(ssize_t));

    while(1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
        	if ((msg.source == (void *) http_stream_reader) && (msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)) {
        		audio_element_info_t music_info = {0};

				audio_element_getinfo(http_stream_reader, &music_info);
				ESP_LOGI(TAG, "[ * ] Receive music info from http_stream_reader, sample_rates=%d, bits=%d, ch=%d",
				                     music_info.sample_rates, music_info.bits, music_info.channels);
        	}
        	else if ((msg.source == (void *) http_stream_reader) && (msg.cmd == AEL_MSG_CMD_REPORT_CODEC_FMT)) {
        		audio_element_info_t music_info = {0};

				audio_element_getinfo(http_stream_reader, &music_info);
				ESP_LOGI(TAG, "[ * ] Codec format: %d", music_info.codec_fmt);
        	}
    	}
    }
}
