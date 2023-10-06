/*
 * server_flac_encoder.h
 *
 *  Created on: 22.09.2023
 *      Author: carlos
 */

#ifndef MAIN_SERVER_FLAC_ENCODER_H_
#define MAIN_SERVER_FLAC_ENCODER_H_

#include "esp_err.h"

typedef struct flac_codec_header_s {
	size_t size;
	char *payload;
} flac_codec_header_t;

esp_err_t get_codec_header(flac_codec_header_t **p_cHeader);


#endif /* MAIN_SERVER_FLAC_ENCODER_H_ */
