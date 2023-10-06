# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS ./components 
								 #./managed_components
								 #$ENV{IDF_PATH}/examples/common_components/protocol_examples_common
								 #$ENV{ADF_PATH}/components
								 )

list(APPEND COMPONENTS	app_trace
						app_update
						bootloader
						bootloader_support
						bt
						cmock
						console
						cxx
						driver
						efuse
						esp-tls
						esp_adc
						esp_app_format
						esp_common
						esp_eth
						esp_event
						esp_gdbstub
						esp_hid
						esp_http_client
						esp_http_server
						esp_https_ota
						esp_https_server
						esp_hw_support
						esp_lcd
						esp_local_ctrl
						esp_netif
						esp_partition
						esp_phy
						esp_pm
						esp_psram
						esp_ringbuf
						esp_rom
						esp_system
						esp_timer
						esp_wifi
						espcoredump
						esptool_py
						fatfs
						freertos
						hal
						heap
						http_parser
						idf_test
						ieee802154
						json
						linux
						log
						lwip
						mbedtls
						mqtt
						newlib
						nvs_flash
						openthread
						partition_table
						perfmon
						protobuf-c
						protocomm
						pthread
						riscv
						sdmmc
						soc
						spi_flash
						spiffs
						tcp_transport
						touch_element
						ulp
						unity
						usb
						vfs
						wear_levelling
						wifi_provisioning
						wpa_supplicant
						xtensa						
						snapserver
						esp_dlna
						flac
						main
						nghttp
						jsmn
						#protocol_examples_common
						#adf_utils
						#audio_board
						#audio_hal
						#audio_pipeline
						#audio_recorder
						#audio_sal
						#audio_stream
						#battery_service
						#bluetooth_service
						#clouds
						#coredump_upload_service
						#display_service
						#dueros_service
						#esp-adf-libs
						#esp-sr
						#esp_actions
						#esp_codec_dev
						#esp_dispatcher
						#esp_event_cast
						#esp_peripherals
						#input_key_service
						#ota_service
						#playlist
						#tone_partition
						#wifi_service
)



#include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock)
 
project(dlna_example)

target_add_binary_data(dlna_example.elf "./components/esp_dlna/xml/rootDesc.xml" TEXT)
target_add_binary_data(dlna_example.elf "./components/esp_dlna/xml/rootDescLogo.xml" TEXT)
target_add_binary_data(dlna_example.elf "./components/esp_dlna/xml/rootDescService.xml" TEXT)
target_add_binary_data(dlna_example.elf "./components/esp_dlna/xml/SoapResponseErr.xml" TEXT)
target_add_binary_data(dlna_example.elf "./components/esp_dlna/xml/SoapResponseOk.xml" TEXT)
target_add_binary_data(dlna_example.elf "./components/esp_dlna/xml/devicelist.xml" TEXT)

#message("components: ${components} #################")
