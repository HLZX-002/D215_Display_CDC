//touch.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <rtthread.h>
#include <rtdef.h>
#include <rtdevice.h>
#include <aic_log.h>

extern bool dtr_enable;
extern bool ep_tx_busy_flag;

#if defined(AIC_USB_OSD_TOUCH_ENABLE)
/* V1.64: the whole CDC touch-report path is compiled out for display-only
 * products built with CONFIG_AIC_USB_OSD_TOUCH_ENABLE=n.  The stubs below
 * keep the USB callbacks and the touch-panel driver linked unchanged; the
 * host simply never receives Touch[D]/Touch[U] reports. */
#define TOUCH_QUEUE_SIZE   10   // 增加队列深度
#define TOUCH_PACKET_SIZE  32   // 单个数据包最大长度

/* 触摸事件结构体 */
typedef struct {
    char data[TOUCH_PACKET_SIZE];
    uint16_t len;
    int8_t id;           // 触摸点ID
    bool is_down;        // 按下/抬起标志
    uint32_t timestamp;  // 时间戳
} touch_event_t;

/* 触摸数据队列结构 */
static struct {
    touch_event_t events[TOUCH_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    rt_mutex_t mutex;
} touch_queue = {0};

/* 初始化触摸队列 */
int touch_queue_init(void)
{
    memset(&touch_queue, 0, sizeof(touch_queue));
    
    // 创建互斥锁
    touch_queue.mutex = rt_mutex_create("touch_q", RT_IPC_FLAG_PRIO);
    if (touch_queue.mutex == RT_NULL) {
        return -RT_ERROR;
    }
    
    return RT_EOK;
}

/* 发送完成回调 */
void touch_send_complete_callback(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    
    // 标记发送完成
    ep_tx_busy_flag = false;
    
    // 从队列中移除已发送的数据
    if (touch_queue.count > 0) {
        if (rt_mutex_take(touch_queue.mutex, RT_WAITING_FOREVER) == RT_EOK) {
            touch_queue.head = (touch_queue.head + 1) % TOUCH_QUEUE_SIZE;
            touch_queue.count--;
            rt_mutex_release(touch_queue.mutex);
        }
    }
    
    rt_hw_interrupt_enable(level);
}

/* 格式化触摸数据 */
static int format_touch_data(char *buf, int buf_size, int8_t id, int16_t x, int16_t y, bool is_down)
{
    return snprintf(buf, buf_size,
                   "Touch[%s],%d,%d,%d\n",
                   is_down ? "D" : "U", id, x, y);
}

/* 发送触摸数据 */
void send_touch_data(int8_t id, int16_t x, int16_t y, bool is_down)
{
    rt_base_t level;
    int len;
    
    if (!touch_queue.mutex) {
        return;
    }
    
    // 获取互斥锁
    if (rt_mutex_take(touch_queue.mutex, RT_WAITING_FOREVER) != RT_EOK) {
        return;
    }
    
    // 格式化数据包
    len = format_touch_data(touch_queue.events[touch_queue.tail].data, 
                           TOUCH_PACKET_SIZE, id, x, y, is_down);
    if (len <= 0) {
        rt_mutex_release(touch_queue.mutex);
        return;
    }
    
    // 保存事件信息
    touch_queue.events[touch_queue.tail].len = len;
    touch_queue.events[touch_queue.tail].id = id;
    touch_queue.events[touch_queue.tail].is_down = is_down;
    touch_queue.events[touch_queue.tail].timestamp = rt_tick_get();
    
    // 队列已满时丢弃最旧数据
    if (touch_queue.count >= TOUCH_QUEUE_SIZE) {
        uint8_t discard_idx = touch_queue.head;
        int8_t discard_id = touch_queue.events[discard_idx].id;
        bool discard_is_down = touch_queue.events[discard_idx].is_down;
        
        // 如果丢弃的是按下事件，尝试找到对应的抬起事件一起丢弃
        if (discard_is_down) {
            for (uint8_t i = 1; i < touch_queue.count; i++) {
                uint8_t idx = (touch_queue.head + i) % TOUCH_QUEUE_SIZE;
                if (touch_queue.events[idx].id == discard_id && 
                    !touch_queue.events[idx].is_down) {
                    // 丢弃这对按下-抬起事件
                    touch_queue.head = (idx + 1) % TOUCH_QUEUE_SIZE;
                    touch_queue.count -= (i + 1);
                    break;
                }
            }
        } else {
            // 直接丢弃抬起事件
            touch_queue.head = (touch_queue.head + 1) % TOUCH_QUEUE_SIZE;
            touch_queue.count--;
        }
    }
    
    // 存入队列
    touch_queue.tail = (touch_queue.tail + 1) % TOUCH_QUEUE_SIZE;
    touch_queue.count++;
    
    // 释放互斥锁
    rt_mutex_release(touch_queue.mutex);
    
    // 尝试立即发送
    level = rt_hw_interrupt_disable();
    
    /* 只有在端点空闲且DTR有效时发送 */
    if (!ep_tx_busy_flag && dtr_enable && touch_queue.count > 0) {
        ep_tx_busy_flag = true;
        rt_hw_interrupt_enable(level);
        
        // 获取要发送的数据
        if (rt_mutex_take(touch_queue.mutex, RT_WAITING_FOREVER) != RT_EOK) {
            level = rt_hw_interrupt_disable();
            ep_tx_busy_flag = false;
            rt_hw_interrupt_enable(level);
            return;
        }
        
        char *buf = touch_queue.events[touch_queue.head].data;
        len = touch_queue.events[touch_queue.head].len;
        
        rt_mutex_release(touch_queue.mutex);
        
        int ret = usbd_ep_start_write(runtime_in_ep(), (uint8_t*)buf, len);
        if (ret != RT_EOK) {
            level = rt_hw_interrupt_disable();
            ep_tx_busy_flag = false;
            rt_hw_interrupt_enable(level);
        }
        // 发送成功时，等待发送完成中断处理
    } else {
        rt_hw_interrupt_enable(level);
    }
}

/* 批量发送函数 */
void touch_bulk(void)
{ 
    rt_base_t level = rt_hw_interrupt_disable();
    
    // 先标记端点空闲
    ep_tx_busy_flag = false;
    
    /* 队列中有数据且DTR有效时继续发送 */
    if (touch_queue.count > 0 && dtr_enable) {
        // 获取互斥锁
        if (rt_mutex_take(touch_queue.mutex, RT_WAITING_FOREVER) != RT_EOK) {
            rt_hw_interrupt_enable(level);
            return;
        }
        
        char *buf = touch_queue.events[touch_queue.head].data;
        uint16_t len = touch_queue.events[touch_queue.head].len;
        
        // 发送前检查，确保不中断正在进行的触摸序列
        int8_t current_id = touch_queue.events[touch_queue.head].id;
        bool current_is_down = touch_queue.events[touch_queue.head].is_down;
        
        // 如果当前是按下事件，确保能发送完整的序列
        if (current_is_down) {
            // 检查队列中是否有对应的抬起事件
            bool has_up_event = false;
            for (uint8_t i = 1; i < touch_queue.count; i++) {
                uint8_t idx = (touch_queue.head + i) % TOUCH_QUEUE_SIZE;
                if (touch_queue.events[idx].id == current_id && 
                    !touch_queue.events[idx].is_down) {
                    has_up_event = true;
                    break;
                }
            }
            
            // 如果没有对应的抬起事件，暂时不发送（等待抬起事件）
            if (!has_up_event && touch_queue.count > 1) {
                // 优先发送完整的序列
                for (uint8_t i = 1; i < touch_queue.count; i++) {
                    uint8_t idx = (touch_queue.head + i) % TOUCH_QUEUE_SIZE;
                    if (touch_queue.events[idx].is_down) {
                        // 找到另一个完整的按下-抬起序列
                        bool has_corresponding_up = false;
                        for (uint8_t j = i + 1; j < touch_queue.count; j++) {
                            uint8_t jdx = (touch_queue.head + j) % TOUCH_QUEUE_SIZE;
                            if (touch_queue.events[jdx].id == touch_queue.events[idx].id && 
                                !touch_queue.events[jdx].is_down) {
                                has_corresponding_up = true;
                                break;
                            }
                        }
                        
                        if (has_corresponding_up) {
                            // 发送这个完整的序列
                            buf = touch_queue.events[idx].data;
                            len = touch_queue.events[idx].len;
                            // 更新head，这样下次发送完整序列
                            touch_queue.head = idx;
                            break;
                        }
                    }
                }
            }
        }
        
        rt_mutex_release(touch_queue.mutex);
        
        ep_tx_busy_flag = true;
        rt_hw_interrupt_enable(level);
        
        int ret = usbd_ep_start_write(runtime_in_ep(), (uint8_t*)buf, len);
        if (ret != RT_EOK) {
            level = rt_hw_interrupt_disable();
            ep_tx_busy_flag = false;
            rt_hw_interrupt_enable(level);
        }
    } else {
        rt_hw_interrupt_enable(level);
    }
}

/* 初始化函数 */
int touch_system_init(void)
{
    int ret = touch_queue_init();
    if (ret != RT_EOK) {
        pr_err("Touch queue init failed\n");
        return ret;
    }
    
    pr_info("Touch system initialized\n");
    return RT_EOK;
}
INIT_DEVICE_EXPORT(touch_system_init);

#else /* !AIC_USB_OSD_TOUCH_ENABLE */

/* Report-only stubs: identical signatures, zero side effects. */
void touch_send_complete_callback(void)
{
}

void send_touch_data(int8_t id, int16_t x, int16_t y, bool is_down)
{
    (void)id;
    (void)x;
    (void)y;
    (void)is_down;
}

void touch_bulk(void)
{
}

int touch_queue_init(void)
{
    return RT_EOK;
}

int touch_system_init(void)
{
    return RT_EOK;
}

#endif /* AIC_USB_OSD_TOUCH_ENABLE */
