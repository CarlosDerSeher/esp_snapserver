idf_component_register(SRCS "snapserver.c" "wire_chunk_fifo.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer json libbuffer esp_event)
