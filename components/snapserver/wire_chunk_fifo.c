/*
 * wire_chunk_fifo.c
 *
 *  Created on: 25.09.2023
 *      Author: carlos
 */

#include <math.h>
#include "wire_chunk_fifo.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_event.h"

TAILQ_HEAD(wire_chunk_tailq_queue_s, wire_chunk_tailq_s);

const char *TAG = "CHK_FIFO";

static struct wire_chunk_tailq_queue_s wire_chunk_tail_queue;
static wire_chunk_fifo_t wire_chunk_fifo = {NULL, NULL, NULL};
static TaskHandle_t wire_chunk_fifo_remove_oldest_taskhandle = NULL;

static uint32_t buffer_us = 0;
static size_t maxSize;

static int64_t oldTime = 0;

/* Event source task related definitions */
ESP_EVENT_DEFINE_BASE(WIRE_CHUNK_FIFO_EVENTS);

static TaskHandle_t tasksToNotifiy = NULL;
static SemaphoreHandle_t oldestRemoved;

static bool init = false;

/**
 *
 */
esp_err_t wire_chunk_fifo_insert(wire_chunk_tailq_t *element) {
	if (!element ) {
		return ESP_FAIL;
	}

	if (!wire_chunk_fifo.countSemaphore || !wire_chunk_fifo.mux) {
		return ESP_FAIL;
	}

	xSemaphoreTake(wire_chunk_fifo.countSemaphore, portMAX_DELAY);
	xSemaphoreTake(wire_chunk_fifo.mux, portMAX_DELAY);
	TAILQ_INSERT_TAIL(wire_chunk_fifo.wire_chunk_tail_queue, element, tailq);
	xSemaphoreGive(wire_chunk_fifo.mux);

	wire_chunk_fifio_notify_new_chnk();

#if 0
	ESP_LOGI(TAG, "inserted new chunk. Semaphore count %d, diff %lld, free %d, block %d", uxSemaphoreGetCount(wire_chunk_fifo.countSemaphore),
																						  esp_timer_get_time() - oldTime,
																						  heap_caps_get_free_size(MALLOC_CAP_8BIT),
																						  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
	oldTime = esp_timer_get_time();
#endif

	return ESP_OK;
}

/**
 *
 */
bool wire_chunk_fifo_empty(void) {
	bool ret;

	if (!wire_chunk_fifo.mux) {
		return ESP_FAIL;
	}

	xSemaphoreTake(wire_chunk_fifo.mux, portMAX_DELAY);
	ret = TAILQ_EMPTY(wire_chunk_fifo.wire_chunk_tail_queue);
	xSemaphoreGive(wire_chunk_fifo.mux);

	return ret;
}

/**
 *
 */
bool wire_chunk_fifo_full(void) {
	if (!wire_chunk_fifo.countSemaphore) {
		return ESP_FAIL;
	}

	if (uxSemaphoreGetCount(wire_chunk_fifo.countSemaphore) == 0) {
		return true;
	}

	return false;
}

/**
 *
 */
esp_err_t wire_chunk_fifo_clear(void) {
	wire_chunk_tailq_t *element = wire_chunk_fifo_get_oldest();

	if (!wire_chunk_fifo.mux) {
		return ESP_FAIL;
	}

	xSemaphoreTake(wire_chunk_fifo.mux, portMAX_DELAY);
	while(element) {
		TAILQ_REMOVE(wire_chunk_fifo.wire_chunk_tail_queue, element, tailq);
		free(element->chunk->payload);
		element->chunk->payload = NULL;
		free(element->chunk);
		element->chunk = NULL;
		free(element);
		xSemaphoreGive(wire_chunk_fifo.countSemaphore);
		element = TAILQ_FIRST(wire_chunk_fifo.wire_chunk_tail_queue);
	}
	xSemaphoreGive(wire_chunk_fifo.mux);

	return ESP_OK;
}

/**
 *
 */
esp_err_t wire_chunk_fifo_remove_oldest(void) {
	wire_chunk_tailq_t *element = wire_chunk_fifo_get_oldest();

	if (!wire_chunk_fifo.countSemaphore || !wire_chunk_fifo.mux) {
		return ESP_FAIL;
	}

	xSemaphoreTake(wire_chunk_fifo.mux, portMAX_DELAY);
	//element = TAILQ_LAST(wire_chunk_fifo.wire_chunk_tail_queue, wire_chunk_tailq_queue_s);
	if (element) {
		TAILQ_REMOVE(wire_chunk_fifo.wire_chunk_tail_queue, element, tailq);
		free(element->chunk->payload);
		element->chunk->payload = NULL;
		free(element->chunk);
		element->chunk = NULL;
		free(element);
		element = NULL;

	}
	xSemaphoreGive(wire_chunk_fifo.mux);
	xSemaphoreGive(wire_chunk_fifo.countSemaphore);

	return ESP_OK;
}

/**
 *
 */
wire_chunk_tailq_t *wire_chunk_fifo_get_newest(void) {
	wire_chunk_tailq_t *element;

	if (!wire_chunk_fifo.mux) {
		return NULL;
	}

	xSemaphoreTake(wire_chunk_fifo.mux, portMAX_DELAY);
	element = TAILQ_LAST(wire_chunk_fifo.wire_chunk_tail_queue, wire_chunk_tailq_queue_s);
	xSemaphoreGive(wire_chunk_fifo.mux);

	return element;
}

/**
 *
 */
wire_chunk_tailq_t *wire_chunk_fifo_get_oldest(void) {
	wire_chunk_tailq_t *element;

	if (!wire_chunk_fifo.mux) {
		return NULL;
	}

	xSemaphoreTake(wire_chunk_fifo.mux, portMAX_DELAY);
	element = TAILQ_FIRST(wire_chunk_fifo.wire_chunk_tail_queue);
	xSemaphoreGive(wire_chunk_fifo.mux);

	return element;
}

/**
 *
 */
wire_chunk_tailq_t *wire_chunk_fifo_get_next(wire_chunk_tailq_t *element) {
	if (!wire_chunk_fifo.mux) {
		return NULL;
	}

	xSemaphoreTake(wire_chunk_fifo.mux, portMAX_DELAY);
	element = TAILQ_NEXT(element, tailq);
	xSemaphoreGive(wire_chunk_fifo.mux);

	return element;
}

/**
 *
 */
void remove_oldest_timer_callback(void *pvArguments) {
	wire_chunk_fifo_remove_oldest();
	xSemaphoreGive(oldestRemoved);

//	ESP_LOGI(TAG, "oldest removed. diff %lld", esp_timer_get_time() - oldTime);
//	oldTime = esp_timer_get_time();
}

/**
 *
 */
void wire_chunk_fifo_remove_oldest_task(void *pvParameters) {
	esp_timer_handle_t rem_timer_handle;
	esp_timer_create_args_t cfg = {
		    .callback = remove_oldest_timer_callback,        //!< Function to call when timer expires
		    .arg = NULL,                      //!< Argument to pass to the callback
		    .dispatch_method = ESP_TIMER_TASK,   //!< Call the callback from task or from ISR
		    .name = "t_rem_cb",               //!< Timer name, used in esp_timer_dump function
		    .skip_unhandled_events = true,     //!< Skip unhandled events for periodic timers
	};

	esp_timer_create(&cfg, &rem_timer_handle);

	oldestRemoved = xSemaphoreCreateBinary();
	xSemaphoreGive(oldestRemoved);

	while(1) {
		if (wire_chunk_fifo_full() == true) {
			int64_t currentTime_us = esp_timer_get_time();
			wire_chunk_tailq_t *oldest = wire_chunk_fifo_get_oldest();
			int64_t timestamp_us = (int64_t)oldest->chunk->timestamp.sec * 1000000LL + (int64_t)oldest->chunk->timestamp.usec;

			if (currentTime_us > (timestamp_us + buffer_us)) {
				wire_chunk_fifo_remove_oldest();

//				ESP_LOGI(TAG, "remove oldest. Obsolete since %lldus", currentTime_us - (timestamp_us + buffer_us));
			}
			else {
//				portTickType wait_ms = ceil((float)(timestamp_us + buffer_us - currentTime_us) / 1000.0);
//
//				ESP_LOGI(TAG, "wait for oldest to become obsolete in %lldus", timestamp_us + buffer_us - currentTime_us);
//
//				vTaskDelay(pdMS_TO_TICKS(wait_ms));
				esp_timer_start_once(rem_timer_handle, timestamp_us + buffer_us - currentTime_us);
				xSemaphoreTake(oldestRemoved, portMAX_DELAY);
			}
		}
		else {
			// TODO: find a better way to do this
			vTaskDelay(pdMS_TO_TICKS(3));

			//ESP_LOGI(TAG, "wait for fifo to fill");
		}
	}
}

/**
 *
 */
void wire_chunk_fifio_notify_register_task(TaskHandle_t handle) {
	tasksToNotifiy = handle;
}

/**
 *
 */
void wire_chunk_fifio_notify_unregister_task(TaskHandle_t handle) {
	if (handle) {
		tasksToNotifiy = NULL;
	}
}

/**
 *
 */
void wire_chunk_fifio_notify_new_chnk(void) {
	if (tasksToNotifiy) {
		xTaskNotify(tasksToNotifiy, 1, eSetValueWithOverwrite);
	}
}

/**
 *
 */
esp_err_t wire_chunk_fifo_init(size_t mSize, uint32_t buf_ms) {
	if (init)  {
		return ESP_OK;
	}

	if (!mSize) {
		return ESP_FAIL;
	}

	TAILQ_INIT(&wire_chunk_tail_queue);
	wire_chunk_fifo.wire_chunk_tail_queue = &wire_chunk_tail_queue;
	wire_chunk_fifo.countSemaphore = xSemaphoreCreateCounting(mSize, mSize);
	if (!wire_chunk_fifo.countSemaphore) {
		ESP_LOGE(TAG, "Count Semaphore not created");

		return ESP_FAIL;
	}
	wire_chunk_fifo.mux = xSemaphoreCreateMutex();
	if (!wire_chunk_fifo.mux) {
		ESP_LOGE(TAG, "Mutex not created");

		return ESP_FAIL;
	}

	maxSize = mSize;
	buffer_us = buf_ms * 1000;

	xTaskCreatePinnedToCore(wire_chunk_fifo_remove_oldest_task, "chk_rem_tsk", 2048, NULL, 17, &wire_chunk_fifo_remove_oldest_taskhandle, 1);

	init = true;

	return ESP_OK;
}
