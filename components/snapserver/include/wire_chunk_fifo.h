/*
 * wire_chunk_fifo.h
 *
 *  Created on: 25.09.2023
 *      Author: carlos
 */

#ifndef MAIN_WIRE_CHUNK_FIFO_H_
#define MAIN_WIRE_CHUNK_FIFO_H_

#include <stddef.h>
#include <sys/queue.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_event.h"

#include "snapserver.h"

typedef struct wire_chunk_tailq_s wire_chunk_tailq_t;
struct wire_chunk_tailq_s {
    TAILQ_ENTRY(wire_chunk_tailq_s) tailq;
    wire_chunk_message_t *chunk;
};

typedef struct wire_chunk_fifo_s {
	struct wire_chunk_tailq_queue_s *wire_chunk_tail_queue;
	SemaphoreHandle_t countSemaphore;	// equivalent of size which will block if maxSize is reached
	SemaphoreHandle_t mux;
} wire_chunk_fifo_t;

ESP_EVENT_DECLARE_BASE(WIRE_CHUNK_FIFO_EVENTS);         // declaration of the task events family

enum {
	NEW_WIRE_CHUNK_EVENT
};

esp_err_t wire_chunk_fifo_init(size_t mSize, uint32_t buf_ms);
esp_err_t wire_chunk_fifio_register_client(snapclient_t *client);
esp_err_t wire_chunk_fifio_unregister_client(snapclient_t *client);
void wire_chunk_fifio_client_info_new_chnk(void);
esp_err_t wire_chunk_fifo_insert(wire_chunk_tailq_t *element);
bool wire_chunk_fifo_empty(void);
bool wire_chunk_fifo_full(void);
esp_err_t wire_chunk_fifo_clear(void);
wire_chunk_tailq_t *wire_chunk_fifo_get_newest(void);
wire_chunk_tailq_t *wire_chunk_fifo_get_oldest(void);
wire_chunk_tailq_t *wire_chunk_fifo_get_next(wire_chunk_tailq_t *element);

esp_err_t wire_chunk_fifo_lock(void);
esp_err_t wire_chunk_fifo_unlock(void);

esp_err_t wire_chunk_fifo_delete_element(wire_chunk_tailq_t *element);

#endif /* MAIN_WIRE_CHUNK_FIFO_H_ */
