#ifndef USB_CDC_FLASH_RW_H
#define USB_CDC_FLASH_RW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_CDC_SN_PAYLOAD_LEN 26
#define USB_CDC_SN_FLASH_LEN   27

int usb_cdc_storage_init(void);
size_t usb_cdc_load_sn(char *sn, size_t sn_size);
bool usb_cdc_queue_sn_update(const uint8_t *packet, uint32_t len);
bool usb_cdc_set_backlight(uint8_t brightness, bool save);
uint8_t usb_cdc_get_backlight(void);

/* CherryUSB composite-device serial-number override. */
int usbd_custom_serial_number(char *str);

#endif
