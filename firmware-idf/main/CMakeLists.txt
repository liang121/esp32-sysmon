idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES rgb_lcd_port gui_paint fonts touch io_extension
                                esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_driver_usb_serial_jtag esp_timer)
