/*
 * Copyright (c) 2022-2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usbd_core.h"
#include "usbd_cdc.h"
#include "ota_upgrade.h" 


// OTA标记定义
 const uint8_t OTA_START_MARKER[] = {0xAA, 0xBB, 0xCC};
 const uint8_t OTA_END_MARKER[] = {0xDD, 0xEE, 0xFF};
#define OTA_MARKER_SIZE   3

// OTA响应定义
 const char OTA_RESPONSE_READY[] = "OTA_READY";
 const char OTA_RESPONSE_OK[] = "OTA_OK";
 const char OTA_RESPONSE_ERROR[] = "OTA_ERROR";
 const char OTA_RESPONSE_BUSY[] = "OTA_BUSY";

// OTA全局变量
uint8_t ota_status = OTA_IDLE;
uint32_t ota_total_received = 0;
uint32_t ota_last_receive_time = 0;
uint32_t ota_error_count = 0;
uint32_t ota_packet_count = 0;


/**
 * @brief 发送OTA响应
 */
void send_ota_response(const uint8_t *response, uint32_t len) 
{
    if (response && len > 0) {
        if (usbd_ep_start_write(runtime_in_ep(), (uint8_t*)response, len) == RT_EOK) {
            // 发送成功，轻微延迟确保数据发送
            rt_thread_mdelay(1);
        }
    }
}

/**
 * @brief 检查是否为OTA起始标记
 */
 bool is_ota_start_marker(const uint8_t *data, uint32_t length)
{
    return (length >= OTA_MARKER_SIZE && 
            memcmp(data, OTA_START_MARKER, OTA_MARKER_SIZE) == 0);
}

/**
 * @brief 检查是否为OTA结束标记
 */
 bool is_ota_end_marker(const uint8_t *data, uint32_t length)
{
    return (length >= OTA_MARKER_SIZE && 
            memcmp(data + length - OTA_MARKER_SIZE, OTA_END_MARKER, OTA_MARKER_SIZE) == 0);
}

/**
 * @brief 处理OTA超时
 */
 void handle_ota_timeout(void)
{
    uint32_t current_time = rt_tick_get();
    uint32_t elapsed_time = (current_time - ota_last_receive_time) * (1000 / RT_TICK_PER_SECOND);
    
    if (elapsed_time > OTA_TIMEOUT_MS) {
        rt_kprintf("[OTA] Timeout occurred, resetting OTA session\n");
        send_ota_response((uint8_t*)OTA_RESPONSE_ERROR, strlen(OTA_RESPONSE_ERROR));
        reset_ota_session();
    }
}

/**
 * @brief 发送进度报告
 */
 void send_progress_report(void)
{
    if (ota_packet_count % PROGRESS_REPORT_INTERVAL == 0) {
        char progress_msg[32];
        int len = snprintf(progress_msg, sizeof(progress_msg), 
                         "OTA_PROGRESS:%lu", ota_total_received);
        if (len > 0) {
            send_ota_response((uint8_t*)progress_msg, (uint32_t)len);
        }
    }
}

/**
 * @brief 处理CDC接收到的OTA数据包
 * @param data 接收到的数据
 * @param length 数据长度
 * @return true 数据已被OTA处理
 * @return false 数据未被OTA处理
 */
bool handle_ota_packet(uint8_t *data, uint32_t length)
{
    if (length == 0 || data == NULL) {
        return false;
    }

    // 检查OTA超时
    if (ota_status != OTA_IDLE) {
        handle_ota_timeout();
    }

    // 检查OTA起始标记
    if (ota_status == OTA_IDLE && is_ota_start_marker(data, length)) {
        rt_kprintf("[OTA] Start marker received, initializing OTA...\n");
        
        // 步骤1: OTA初始化
        if (ota_init() != RT_EOK) {
            rt_kprintf("[OTA] Initialization failed!\n");
            send_ota_response((uint8_t*)OTA_RESPONSE_ERROR, strlen(OTA_RESPONSE_ERROR));
            return true;
        }
        
        ota_status = OTA_INITIALIZED;
        ota_total_received = 0;
        ota_error_count = 0;
        ota_packet_count = 0;
        ota_last_receive_time = rt_tick_get();
        
        rt_kprintf("[OTA] Initialization successful, ready for data\n");
        send_ota_response((uint8_t*)OTA_RESPONSE_READY, strlen(OTA_RESPONSE_READY));
        
        // 处理起始包中的数据（如果有）
        if (length > OTA_MARKER_SIZE) {
            uint32_t data_size = length - OTA_MARKER_SIZE;
            if (process_ota_data(data + OTA_MARKER_SIZE, data_size) == RT_EOK) {
                ota_status = OTA_RECEIVING;
            } else {
                ota_status = OTA_ERROR;
                send_ota_response((uint8_t*)OTA_RESPONSE_ERROR, strlen(OTA_RESPONSE_ERROR));
            }
        }
        
        return true;
    }
    
    // 检查OTA结束标记
    if ((ota_status == OTA_RECEIVING || ota_status == OTA_INITIALIZED) && 
        is_ota_end_marker(data, length)) {
        
        rt_kprintf("[OTA] End marker received, completing OTA update...\n");
        
        // 处理结束包中的数据（如果有）
        uint32_t data_size = length - OTA_MARKER_SIZE;
        if (data_size > 0) {
            if (process_ota_data(data, data_size) != RT_EOK) {
                rt_kprintf("[OTA] Failed to process final data\n");
                send_ota_response((uint8_t*)OTA_RESPONSE_ERROR, strlen(OTA_RESPONSE_ERROR));
                reset_ota_session();
                return true;
            }
        }
        
        // 完成OTA更新
        complete_ota_update();
        return true;
    }
    
    // 处理OTA数据包
    if (ota_status == OTA_RECEIVING || ota_status == OTA_INITIALIZED) {
        // 处理数据
        if (process_ota_data(data, length) != RT_EOK) {
            ota_error_count++;
            rt_kprintf("[OTA] Data processing failed, error count: %lu\n", ota_error_count);
            
            if (ota_error_count >= MAX_ERROR_COUNT) {
                rt_kprintf("[OTA] Maximum error count reached, aborting OTA\n");
                send_ota_response((uint8_t*)OTA_RESPONSE_ERROR, strlen(OTA_RESPONSE_ERROR));
                reset_ota_session();
            } else {
                send_ota_response((uint8_t*)OTA_RESPONSE_ERROR, strlen(OTA_RESPONSE_ERROR));
            }
            return true;
        }
        
        ota_last_receive_time = rt_tick_get();
        ota_packet_count++;
        
        // 定期发送进度响应
        send_progress_report();
        
        return true;
    }
    
    // OTA忙状态响应
    if (ota_status != OTA_IDLE) {
        send_ota_response((uint8_t*)OTA_RESPONSE_BUSY, strlen(OTA_RESPONSE_BUSY));
        return true;
    }
    
    return false;
}

/**
 * @brief 处理OTA数据
 * @param data 数据指针
 * @param length 数据长度
 * @return RT_EOK 成功，其他 失败
 */
 int process_ota_data(const uint8_t *data, uint32_t length)
{
    if (data == NULL || length == 0) {
        return -RT_EINVAL;
    }
    
    // 确保数据长度不超过缓冲区大小
    if (length > OTA_BUFFER_SIZE) {
        rt_kprintf("[OTA] Data too large: %lu > %d\n", length, OTA_BUFFER_SIZE);
        return -RT_ERROR;
    }
    
    // 步骤2: 分片下载到Flash
    if (ota_shard_download_fun((uint8_t*)data, length) < 0) {
        rt_kprintf("[OTA] Flash write failed for %lu bytes\n", length);
        return -RT_ERROR;
    }
    
    ota_total_received += length;
    ota_status = OTA_RECEIVING;
    
    return RT_EOK;
}

/**
 * @brief 完成OTA更新
 */
 void complete_ota_update(void)
{
  
    // 步骤3: 更新环境变量
    int ret = aic_upgrade_end();
    if (ret != RT_EOK) {
        rt_kprintf("[OTA] Failed to update environment variables: %d\n", ret);
        send_ota_response((uint8_t*)OTA_RESPONSE_ERROR, strlen(OTA_RESPONSE_ERROR));
        reset_ota_session();
        return;
    }
    
    send_ota_response((uint8_t*)OTA_RESPONSE_OK, strlen(OTA_RESPONSE_OK));
    
    // 短暂延迟确保响应发送完成
    rt_thread_mdelay(500);
    
    // 清理OTA资源
    ota_deinit();
    ota_status = OTA_COMPLETED;
    
    rt_kprintf("[OTA] Ready for system reset...\n");
    
    // 步骤4: 重启设备
    rt_thread_mdelay(1000);
    extern void rt_hw_cpu_reset(void);
    rt_hw_cpu_reset();
}

/**
 * @brief 重置OTA会话
 */
 void reset_ota_session(void)
{
    if (ota_status != OTA_IDLE) {
        ota_deinit();
    }
    
    ota_status = OTA_IDLE;
    ota_total_received = 0;
    ota_error_count = 0;
    ota_packet_count = 0;
    ota_last_receive_time = 0;
}

