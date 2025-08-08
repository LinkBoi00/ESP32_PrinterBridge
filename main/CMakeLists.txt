idf_component_register(SRCS "usb_host_lib.c" "class_driver.c" "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES usb esp_driver_gpio
                    )
