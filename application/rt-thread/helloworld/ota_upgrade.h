#ifndef __OTA_H__
#define __OTA_H__

#include <ota.h>
#include <env.h>
#include <absystem_os.h>
#include <rtthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <rtdef.h>

#define CDC_IN_EP  0x82

// OTA状态定义
#define OTA_IDLE          0
#define OTA_INITIALIZED   1
#define OTA_RECEIVING     2
#define OTA_COMPLETED     3
#define OTA_ERROR         4

// OTA配置
#define OTA_BUFFER_SIZE   2048
#define MAX_ERROR_COUNT   5
#define OTA_TIMEOUT_MS    (30 * 1000)  // 30秒超时
#define PROGRESS_REPORT_INTERVAL 50    // 每50个包报告一次进度

// OTA标记定义
#define OTA_MARKER_SIZE   3

// 函数声明
bool handle_ota_packet(uint8_t *data, uint32_t length);
void send_ota_response(const uint8_t *response, uint32_t len);

// OTA状态外部变量
extern uint8_t ota_status;
extern uint32_t ota_total_received;
extern uint32_t ota_last_receive_time;
extern uint32_t ota_error_count;
extern uint32_t ota_packet_count;


// 函数声明
 int process_ota_data(const uint8_t *data, uint32_t length);
 void reset_ota_session(void);
 void complete_ota_update(void);
 bool is_ota_start_marker(const uint8_t *data, uint32_t length);
 bool is_ota_end_marker(const uint8_t *data, uint32_t length);
 void handle_ota_timeout(void);
 void send_progress_report(void);
extern uint8_t runtime_in_ep(void);
#endif /* __OTA_H__ */