# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly

set(COMPONENT_SRCS "esp_dlna.c" "esp_dlna_avt.c" "esp_dlna_cmr.c" "esp_dlna_rcs.c")
set(COMPONENT_ADD_INCLUDEDIRS "include")
set(COMPONENT_EMBED_TXTFILES 	xml/ConnectionManager.xml
                                xml/AVTransport.xml
                                xml/RenderingControl.xml
                                xml/EventLastChange.xml
                                xml/EventProtocolCMR.xml)
                                
list(APPEND COMPONENT_ADD_INCLUDEDIRS  
							$ENV{ADF_PATH}/components/esp-adf-libs/esp_media_protocols/include
							$ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/include
                            $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/include/port
                            #$ENV{ADF_PATH}/components/esp-adf-libs/esp_audio/include
                            $ENV{ADF_PATH}/components/audio_sal/include
                            $ENV{ADF_PATH}/components/audio_stream/include
                            $ENV{ADF_PATH}/components/audio_stream/lib/hls/include
                            $ENV{ADF_PATH}/components/audio_stream/lib/gzip/include
                            $ENV{ADF_PATH}/components/audio_pipeline/include
                            $ENV{ADF_PATH}/components/esp-adf-libs/esp_codec/include/codec)
				
list(APPEND COMPONENT_SRCS 
                    $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/media_lib_os.c $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/port/media_lib_os_freertos.c
	                $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/media_lib_adapter.c
					$ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/media_lib_crypt.c  $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/port/media_lib_crypt_default.c
	                $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/media_lib_socket.c $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/port/media_lib_socket_default.c
	                $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/media_lib_tls.c  $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/port/media_lib_tls_default.c
	                $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/media_lib_netif.c  $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/port/media_lib_netif_default.c
	                 $ENV{ADF_PATH}/components/esp-adf-libs/media_lib_sal/media_lib_common.c
	                $ENV{ADF_PATH}/components/audio_sal/audio_mem.c
	                #$ENV{ADF_PATH}/components/audio_stream/i2s_stream.c
	                $ENV{ADF_PATH}/components/audio_stream/raw_stream.c
	                $ENV{ADF_PATH}/components/audio_stream/http_stream.c
	                $ENV{ADF_PATH}/components/audio_stream/http_playlist.c
	                $ENV{ADF_PATH}/components/audio_stream/lib/hls/line_reader.c
	                $ENV{ADF_PATH}/components/audio_stream/lib/hls/hls_playlist.c
	                $ENV{ADF_PATH}/components/audio_stream/lib/hls/hls_parse.c
	                $ENV{ADF_PATH}/components/audio_stream/lib/hls/join_path.c
	                $ENV{ADF_PATH}/components/audio_stream/lib/gzip/gzip_miniz.c
	                $ENV{ADF_PATH}/components/audio_pipeline/audio_element.c
	                $ENV{ADF_PATH}/components/audio_pipeline/audio_event_iface.c
	                $ENV{ADF_PATH}/components/audio_pipeline/audio_pipeline.c
	                $ENV{ADF_PATH}/components/audio_pipeline/ringbuf.c
	                $ENV{ADF_PATH}/components/audio_sal/audio_mutex.c
	                $ENV{ADF_PATH}/components/audio_sal/audio_thread.c)
	                
list(APPEND COMPONENT_REQUIRES esp_http_server esp-tls mbedtls esp_netif esp_ringbuf esp_http_client driver)

register_component()

message(STATUS "Chip is ${CONFIG_IDF_TARGET}, ${CMAKE_CURRENT_SOURCE_DIR}")

list(APPEND TARGET_LIB_PATH  "-L $ENV{ADF_PATH}/components/esp-adf-libs/esp_media_protocols/lib/${CONFIG_IDF_TARGET}"
							 #"-L $ENV{ADF_PATH}/components/esp-adf-libs/esp_audio/lib/${CONFIG_IDF_TARGET}"
							 )

target_link_libraries(${COMPONENT_TARGET} "${TARGET_LIB_PATH}")

# GREATER_EQUAL is only available on CMake +3.7.
# To be totally compatible with older versions, let's use GREATER instead
#IF (IDF_VERSION_MAJOR GREATER 3)
    #add_prebuilt_library(esp_audio "$ENV{ADF_PATH}/components/esp-adf-libs/esp_audio/lib/${CONFIG_IDF_TARGET}/libesp_audio.a")
#ENDIF (IDF_VERSION_MAJOR GREATER 3)

#list(APPEND TARGET_LIB_NAME esp_audio)
list(APPEND TARGET_LIB_NAME esp_media_protocols)

target_link_libraries(${COMPONENT_TARGET}  "-Wl,--start-group" ${TARGET_LIB_NAME} "-Wl,--end-group")

