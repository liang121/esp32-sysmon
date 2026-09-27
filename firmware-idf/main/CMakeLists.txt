idf_component_register(SRCS "main.c" "ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES rgb_lcd_port touch io_extension espressif__esp_lvgl_port lvgl__lvgl
                                esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_driver_usb_serial_jtag esp_timer)
