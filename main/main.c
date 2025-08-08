/* 
 * SPDX-FileCopyrightText: 2025 Ilias Dimopoulos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"

#define HOST_LIB_TASK_PRIORITY    2
#define CLASS_TASK_PRIORITY     3

extern void class_driver_task(void *arg);
extern void usb_host_lib_task(void *arg);

static const char *TAG = "PrinterBridge";

void app_main(void)
{
    ESP_LOGI(TAG, "Bonjour from PrinterBridge");

    TaskHandle_t host_lib_task_hdl, class_driver_task_hdl;

    // Create usb host lib task
    BaseType_t task_created;
    task_created = xTaskCreatePinnedToCore(usb_host_lib_task,
                                           "usb_host",
                                           4096,
                                           xTaskGetCurrentTaskHandle(),
                                           HOST_LIB_TASK_PRIORITY,
                                           &host_lib_task_hdl,
                                           0);
    assert(task_created == pdTRUE);

    // Wait until the USB host library is installed
    ulTaskNotifyTake(false, 1000);

    // Create class driver task
    task_created = xTaskCreatePinnedToCore(class_driver_task,
                                           "class",
                                           5 * 1024,
                                           NULL,
                                           CLASS_TASK_PRIORITY,
                                           &class_driver_task_hdl,
                                           0);
    assert(task_created == pdTRUE);
}
