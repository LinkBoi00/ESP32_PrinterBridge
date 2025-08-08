/*
 * SPDX-FileCopyrightText: 2025 Ilias Dimopoulos
 *
 * SPDX-License-Identifier: Apache-2.0
*/

// TODO: Handle more than 1 printer interfaces
// TODO: Implement bi-directional communication

#include <stdio.h>
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "test/test_page_small.h"

static const char *TAG = "Printer handler";

// Definitions taken from https://www.usb.org/sites/default/files/usbprint11a021811.pdf
#define USB_CLASS_PRINTER 0x07
#define USB_PRINTER_PROTOCOL_UNI    0x01
#define USB_PRINTER_PROTOCOL_BI     0x02
#define USB_PRINTER_PROTOCOL_1284   0x03

typedef struct {
    usb_device_handle_t dev_hdl;
    usb_host_client_handle_t client_hdl;
    uint8_t interface_number;
    uint8_t bulk_out_ep;
    uint8_t bulk_in_ep;                     // NULL if unidirectional
    SemaphoreHandle_t transfer_done_sem;    // Semaphore for transfer syncronization
} printer_device_t;

static printer_device_t saved_printer;

static uint8_t is_printer_interface(const usb_intf_desc_t* intf_desc);
static void save_printer_endpoint_details(usb_device_handle_t dev_hdl, usb_host_client_handle_t client_hdl,
                                            uint8_t interface_num, const usb_intf_desc_t *intf_desc,
                                            const usb_config_desc_t *config_desc);
static void print_transfer_callback(usb_transfer_t *transfer);

// Function that checks whether a USB device has printer interfaces
// Returns the printer device if successfull, else NULL
bool check_device_for_printer_interfaces(usb_device_handle_t dev_hdl, usb_host_client_handle_t client_hdl) {
    if (dev_hdl == NULL) {
        ESP_LOGI(TAG, "Device handle is NULL");
        return false;
    }
    if (client_hdl == NULL) {
        ESP_LOGI(TAG, "Device handle is NULL");
        return false;
    }

    ESP_LOGI(TAG, "Checking device for printer interfaces...");

    // Get config descriptor
    const usb_config_desc_t *config_desc;
    esp_err_t ret = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get config descriptor: %s", esp_err_to_name(ret));
        return false;
    }

    uint8_t is_printer = false;
    int offset = 0;
    const usb_intf_desc_t *intf_desc = NULL;

    // Go through all interfaces (TODO: Handle more than 1 printer interfaces)
    for (int i = 0; i < config_desc->bNumInterfaces; i++) {
        intf_desc = usb_parse_interface_descriptor(config_desc, i, 0, &offset);
        if (intf_desc == NULL) {
            ESP_LOGW(TAG, "Failed to parse interface %d", i);
            continue;
        }

        ESP_LOGI(TAG, "Interface %d: Class=0x%02x, SubClass=0x%02x, Protocol=0x%02x",
                 i, intf_desc->bInterfaceClass, intf_desc->bInterfaceSubClass, intf_desc->bInterfaceProtocol);

        // Check if interface is a printer
        if (is_printer_interface(intf_desc)) {
            ESP_LOGI(TAG, "*** This is a PRINTER device! Interface %d ***", i);
            is_printer = true;

            // Handle bi-directional communication (TODO)
            if (is_printer == USB_PRINTER_PROTOCOL_BI) {
                ESP_LOGI(TAG, "Printer supports bi-directional communication (TODO)");
            }

            // Save the printer device's details
            save_printer_endpoint_details(dev_hdl, client_hdl, i, intf_desc, config_desc);

            if (saved_printer.bulk_out_ep != 0xFF) {
                ESP_LOGI(TAG, "Printer saved successfully and ready for use");
            }
        } else {
            ESP_LOGI(TAG, "This is NOT a printer device. Ignoring...");
        }
    }

    return is_printer;
}

// Helper function which checks for a printer interface and returns its protocol
// TODO: Implement bi-directional communication
static uint8_t is_printer_interface(const usb_intf_desc_t* intf_desc) {
    if (intf_desc->bInterfaceClass == USB_CLASS_PRINTER) {
        printf("Found Printer Interface!\n");
        printf("  Interface Class: 0x%02x (Printer)\n", intf_desc->bInterfaceClass);
        printf("  Interface SubClass: 0x%02x\n", intf_desc->bInterfaceSubClass);
        printf("  Interface Protocol: 0x%02x", intf_desc->bInterfaceProtocol);

        switch (intf_desc->bInterfaceProtocol) {
            case USB_PRINTER_PROTOCOL_UNI:
                printf(" (Unidirectional)\n");
                break;
            case USB_PRINTER_PROTOCOL_BI:
                printf(" (Bidirectional)\n");
                break;
            case USB_PRINTER_PROTOCOL_1284:
                printf(" (IEEE 1284.4 compatible bi-directional)\n");
                break;
            default:
                printf(" (Unknown)\n");
                break;
        }
        return intf_desc->bInterfaceProtocol;
    }

    return 0;
}

// Helper function that saves a printer's details to our struct
static void save_printer_endpoint_details(usb_device_handle_t dev_hdl, usb_host_client_handle_t client_hdl,
                                            uint8_t interface_num, const usb_intf_desc_t *intf_desc,
                                            const usb_config_desc_t *config_desc) {
    printer_device_t printer = {0};

    // Save basic device info
    printer.dev_hdl = dev_hdl;
    printer.client_hdl = client_hdl;
    printer.interface_number = interface_num;
    printer.bulk_out_ep = 0xFF;
    printer.bulk_in_ep = 0xFF;

    // Parse endpoints
    int ep_offset = 0;
    for (int ep = 0; ep < intf_desc->bNumEndpoints; ep++) {
        const usb_ep_desc_t *ep_desc = usb_parse_endpoint_descriptor_by_index(
            intf_desc, ep, config_desc->wTotalLength, &ep_offset);
        if (ep_desc == NULL) {
            ESP_LOGW(TAG, "Failed to parse endpoint %d", ep);
            continue;
        }

        // Check if it's a bulk endpoint
        if ((ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
            if (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                // Bulk IN endpoint (for status)
                printer.bulk_in_ep = ep_desc->bEndpointAddress;
                ESP_LOGI(TAG, "Found bulk IN endpoint: 0x%02x", printer.bulk_in_ep);
            } else {
                // Bulk OUT endpoint (for print data)
                printer.bulk_out_ep = ep_desc->bEndpointAddress;
                ESP_LOGI(TAG, "Found bulk OUT endpoint: 0x%02x", printer.bulk_out_ep);
            }
        }
    }

    // Verify we found the required OUT endpoint
    if (printer.bulk_out_ep == 0xFF) {
        ESP_LOGE(TAG, "No bulk OUT endpoint found for printer interface %d", interface_num);
        return;
    }

    // Create semaphore for transfer synchronization
    printer.transfer_done_sem = xSemaphoreCreateBinary();
    if (printer.transfer_done_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create transfer semaphore");
        return;
    }

    // Save to static array
    saved_printer = printer;

    ESP_LOGI(TAG, "Printer saved successfully:");
    ESP_LOGI(TAG, "  Interface: %d", printer.interface_number);
    ESP_LOGI(TAG, "  Bulk OUT: 0x%02x", printer.bulk_out_ep);
    if (printer.bulk_in_ep != 0xFF) {
        ESP_LOGI(TAG, "  Bulk IN:  0x%02x", printer.bulk_in_ep);
    }
}

// Function that sends a print job to the saved printer
esp_err_t send_print_job(void) {
    if (saved_printer.dev_hdl == NULL) {
        ESP_LOGE(TAG, "No printer device available");
        return ESP_ERR_INVALID_STATE;
    }
    if (saved_printer.bulk_out_ep == 0xFF) {
        ESP_LOGE(TAG, "No valid bulk OUT endpoint");
        return ESP_ERR_INVALID_STATE;
    }    

    ESP_LOGI(TAG, "Starting print job...");
    ESP_LOGI(TAG, "Printer details:");
    ESP_LOGI(TAG, "  Interface: %d", saved_printer.interface_number);
    ESP_LOGI(TAG, "  Bulk OUT EP: 0x%02x", saved_printer.bulk_out_ep);
    ESP_LOGI(TAG, "  Data size: %d bytes", test_print_data_size);

    // Claim the printer interface
    esp_err_t ret = usb_host_interface_claim(saved_printer.client_hdl, 
                                           saved_printer.dev_hdl, 
                                           saved_printer.interface_number, 
                                           0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to claim printer interface: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Successfully claimed printer interface");

    // Allocate transfer for sending print data
    usb_transfer_t *transfer = NULL;
    ret = usb_host_transfer_alloc(test_print_data_size, 0, &transfer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate transfer: %s", esp_err_to_name(ret));
        usb_host_interface_release(saved_printer.client_hdl, saved_printer.dev_hdl, saved_printer.interface_number);
        return ret;
    }
    
    // Set up the transfer
    transfer->device_handle = saved_printer.dev_hdl;
    transfer->bEndpointAddress = saved_printer.bulk_out_ep;
    transfer->callback = print_transfer_callback;
    transfer->context = NULL;
    transfer->num_bytes = test_print_data_size;

    // Copy print data to transfer buffer
    memcpy(transfer->data_buffer, test_print_data, test_print_data_size);

    ESP_LOGI(TAG, "Sending print data to endpoint 0x%02x...", saved_printer.bulk_out_ep);

    // Submit the transfer
    ret = usb_host_transfer_submit(transfer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit transfer: %s", esp_err_to_name(ret));
        usb_host_transfer_free(transfer);
        usb_host_interface_release(saved_printer.client_hdl, saved_printer.dev_hdl, saved_printer.interface_number);
        return ret;
    }

    printf("Print job sent successfully!");

    // Wait for transfer to complete (with timeout)
    if (xSemaphoreTake(saved_printer.transfer_done_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Transfer timeout");
        usb_host_interface_release(saved_printer.client_hdl, saved_printer.dev_hdl, saved_printer.interface_number);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void print_transfer_callback(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "Print transfer completed successfully!");
        ESP_LOGI(TAG, "Sent %d bytes to printer", transfer->actual_num_bytes);
    } else {
        ESP_LOGE(TAG, "Print transfer failed with status: %d", transfer->status);
    }

    // Clean up transfer
    usb_host_transfer_free(transfer);

    // Release the interface
    usb_host_interface_release(saved_printer.client_hdl, saved_printer.dev_hdl, saved_printer.interface_number);

    // Signal that transfer is done
    if (saved_printer.transfer_done_sem != NULL) {
        xSemaphoreGive(saved_printer.transfer_done_sem);
    }

    ESP_LOGI(TAG, "Print job completed!");
}