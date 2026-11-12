# 20251217 #
## 优化 ##
- Device SDK：
  - 同步升级 SDK 大版本为 V1.2.2
  - 增加 3 块 DC Demo 板配置
  - 同步 CherryUSB 代码
- Linux 驱动升级到 v.1.1.2
  - 修复性能下降问题
- Android 驱动升级到 v.1.2.0

# 20251020 #
## 优化 ##
- Device SDK：
  - 把 PID 从 (0E01/0E02) 修改成 (0E04/0E05)
  - Windows WHQL 认证已经通过，支持 Windows 驱动在线自动安装
  - 修复 U 盘模式切换概率失败的问题
- Windows 驱动升级到 v.1.1.5.0
  - 增加 PID(0E04/0E05) 支持
  - 修复 Win11 Disable 屏幕副屏没有黑屏的问题
  - 移除 Windows 下的弹出菜单
- MacOS 驱动升级到 v.1.0.0
  - 增加 PID(0E04/0E05) 支持
- Android 驱动升级到 v.1.1.7
  - 增加 PID(0E04/0E05) 支持
- Linux 驱动升级到 v.1.1.0
  - 增加 PID(0E04/0E05) 支持

## 特别说明 ##
本次更新 SDK 会把 USB 副屏的 PID 从 (0E01/0E02) 修改成 (0E04/0E05)。
主要目的是为了支持 Windows 驱动在线自动安装，过微软 WHQL 认证的驱动使用的 PID 为 (0E04/0E05) 。
更新后的驱动(Win/Mac/Android/Linux)向前兼容，同时支持 PID(0E01/0E02/0E04/0E05)。
旧的驱动只支持 PID(0E01/0E02)，旧驱动配合新固件是不能工作的。

# 20250901 #
## 优化 ##
- Windows 驱动升级到 v.1.1.1.0
  - 增加微软官方签名
  - 修复 Windows 睡眠时 USB 副屏黑屏慢的问题
- MacOS 驱动升级到 v.0.9.7
  - 增强 MacOS 驱动 USB 出错处理
- Android 驱动升级到 v.1.1.5
  - 增加镜像翻转模式
- Device SDK：
  - 优化 usb_display_enable/disable() 模式切换

# 20250805 #
## 优化 ##
- Windows 驱动升级到 v.1.0.7
  - 增加 Host Scale 功能
- Device SDK：
  - 增加自定义 EDID 中的 Monitor name 功能

# 20250724 #
## 优化 ##
- MacOS 驱动升级到 v.0.9.5
  - 增加 Host Scale 功能
- Android 驱动升级到 v.1.1.3
  - 大幅优化性能
- Device SDK：
  - 增加 Host Scale 功能

## 修改 ##
- Device SDK：
  - 修复FB旋转白屏闪动问题
  - 修复若干旋转问题

# 20250723 #
## 优化 ##
同步升级 SDK 大版本为 V1.2.1

# 20250618 #
## 修改 ##
- Device SDK：
  - 减少 USB 错包检测门限
  - 增加 JPEG 编码下限设置
  - 增加可配置 serial number 功能
  - 移除不稳定的 USB 拔出检测功能
- MacOS 驱动升级到 v.0.9.2
  - 优化副屏响应系统待机的事件
  - 修复镜像模式下系统待机卡死的问题
  - 增加 JPEG 编码下限设置
- Windows 驱动升级到 v.1.0.6
  - 增加 JPEG 编码下限设置
- Android 驱动 v.1.1.2
  - 增加 Android APK 发布

# 20250520 #
## 优化 ##
- Device SDK：
  - 支持 Device 整系统休眠，降低功耗

# 20250510 #
## 优化 ##
- Device SDK：
  - 重构 USB 复合设备初始化框架，解决 WHQL 认证失败 issue
  - 支持高画质模式（仅限 720P 分辨率以下 + Windows 驱动）
- Windows 驱动升级到 v.1.0.5
  - 支持高画质模式
## 修改 ##
- Device SDK：
  - 修复驱动安装 U 盘不稳定的问题

# 20250429 #
## 优化 ##
- Device SDK：
  - 优化 OSD 菜单分类
  - 支持 PC 待机时 Device 关闭背光
- MacOS 驱动升级到 v.0.8.8
  - 增加开机自启动功能

# 20250429 #
# V1.2.0 #
- 代码同步更新到 Luban-lite 1.2.0

# 20250401 #
## 优化 ##
- Device SDK：
  - 升级到 V0.2 协议
  - 支持 PC 待机时 Device 黑屏
- MacOS 驱动升级到 v.0.8.3
  - 优化性能，软编码改成硬编码
  - 支持 MacOS 11/12/13/14/15，不再支持 MacOS 10
  - 增加 V0.2 协议支持
- Windows 驱动：升级到 v.1.0.2
  - 增加 V0.2 协议支持
## 修改 ##
- Device SDK：
  - 修复 AMD 电脑动态旋转花屏问题

# 20250308 #
## 优化 ##
- MacOS 驱动升级到 v.0.4.0
  - 优化 fps 性能

# 20250306 #
## 优化 ##
- Windows 性能优化，1080P 60fps 33ms latency:
  - Windows 驱动：升级到 v.1.0.2
  - Device SDK： 移除等待 vsync
## 修改 ##
- Device SDK：
  - 修复使用 FB 旋转 OSD 菜单花屏问题
  - 修复驱动 U 盘挂载失败问题

# 20250301 #
## 新增 ##
- Device SDK：
  - 新增动态旋转 API
## 优化 ##
- MacOS 驱动升级到 v.0.3.3:
  - 修复副屏插入后 扩展模式/镜像模式不固定的问题
  - 修复副屏多次插入拔出造成驱动异常退出的问题
## 修改 ##
- Device SDK：
  - 修复 RSA 认证失败后 Crash 的问题

# 20250226 #
## 优化 ##
- Device SDK:
  - 给每个芯片生成独立的 EDID SN
  - 拔出 USB 线自动进入本地待机模式播放
## 修改 ##
  - 同步合入补丁：多线程下 mpp 内存小概率泄露的问题

# 20250213 #
## 优化 ##
- Windows 驱动升级到 v.1.0.1
    优化码率控制算法。提升帧率，减少帧延迟。
- MacOS 驱动升级到 v.0.2.3
    解决大部分兼容性问题，支持 MacOS 10/11/12/13/14/15

# 20250116 #
## 优化 ##
- 优化 USB Display 帧率，1080p 60fps
- MacOS 驱动升级到 v.0.2.1
## 修改 ##
- 修复 uac 播放没有声音的问题

# 20250113 #
# V1.1.2 #
## 新增 ##
- USB：
  - HID keyboard：支持命令行构造字符输入
  - MSC Host：支持无分区表的设备挂载
  - 集成了MacOS驱动试用版
  - 支持用VE完成JPEG旋转
- LVGL：
  - 适配AiUIBuilder
  - Demo中增加Key adc按钮示例
- MPP：
  - 新增WMA、FLAC音频封装格式
  - 支持AAC解码（因开源协议问题，公版不支持AAC）
  - 支持MKV、TS、FLV视频封装格式
- 烧写：支持HID模式的烧写；优化UART烧写的兼容性
- Application：支持自动install资源文件的目录
- D21x/D13x/D12x：支持栈回溯功能
- SFUD：增加安全寄存器的读写接口
- OTA：增加ymodel协议支持
- PWM：支持动态调整clk频率
- I2S：增加数据的直传接口
- QEP：增加D13x平台的支持
- RTP：支持RTP IO当作普通ADC来使用
- OneStep：支持自动打开、保存所有defconfig文件
- BT8858：新增HID device支持，并适配iOS和Android
- TP2825：支持自动检测隔行模式
- 新增方案配置：D13x hspi（衡山PI）
- 新增器件支持：
  - WiFi：AIC8800
  - NAND：XT26G04C、FM25S01BI3
  - Touch：ili2511、ft6336、c123hax022
  - Panel：ili9327
  - Audio codec：cs4344
  - RTC：pcf8563
## 优化 ##
- USB副屏：
  - Display：动态调整USB传输的报文长度
  - OSD：增加背光控制滑动条；支持保存用户的配置参数
  - UDC：优化setup报文的处理
  - 优化复合设备的加载处理
- G73：扫码Demo完成多处性能和稳定性优化；支持画面旋转
- MPP：增加独立的Audio render框架，优化Audio播放相关的功能
- Display：优化Vsync的处理逻辑
- CMU：优化PLL的展频处理
- littlefs：优化读写性能
- UMM：优化初始化的耗时
- SFUD：优化数据的等待延迟
- RTP：优化休眠唤醒后的中断处理
- NAND：优化OOB的访问处理；优化block层的访问性能
- PBP：D13x/G73：优化FatFS对不同cluster size的兼容性
- UART：优化FIFO的触发水位配置；优化DMA模式的处理逻辑
- syscfg：重构代码，更容易支持多平台
- UART烧写：大幅优化传输速度
- FreeType：优化代码体积
- RTP：优化自动校准的方法
- Audio codec：es8388优化启动时间
- SPI：调整Bit模式的分频计算，让频率更精准

## 修改 ##
- LVGL：
  - 修正自动删除Screen时的逻辑漏洞
  - 所有Demo修改资源文件为install方式
  - 打包FatFS镜像：改用整个分区size，不再自动计算
- GE：针对D13x平台增加格式有效性检查
- UseID：修正一处buf的使用
- RTP：去掉对 x_plate和y_plate 参数的取值范围限制
- CE：Hash：修正多次计算的数据处理
- aic-authorization：修正数据buf的cache处理
- OTA：修正版本回滚的处理
- LwIP：修正IP4中next-hop的处理
- workqueue：修正接收和发送的处理流程
- 启动：统一使用ArtInChip启动画面
- VSCode：修正load方式的参数配置
- 栈回溯：默认关闭此功能，可在menuconfig中打开
- OneStep：解决Luban和Luban-Lite之间切换后的命令残留问题
- Version：统一RT-Thread、FreeRTOS、Baremetal系统的version命令log内容
- CherryUSB：修正裸机模式下的互斥锁机制
- 打包镜像：当资源文件路径无效的时候，生成一个空的文件镜像
- 烧写：修正sparse格式的烧写失败问题；G73减少Boot的代码段空间
- UART：完善可选的波特率配置表
- 开机动画：改善竖屏的兼容处理
- syscfg：修正D12x获取LDO的返回值
- BootLoader：不支持用户手动选择elf区域配置
- 清理开源协议的规范使用问题

# 20241205 #
## 新增 ##
- 增加 Vendor class device.

## 修改 ##
- 优化传输策略。
- 优化 udc driver.

# 20241105 #
## 新增 ##
- 增加 Touch Crop 功能

## 修改 ##
- 修复 UAC 杂音问题

# 20241017 #
## 新增 ##
- 增加屏幕 Crop 功能
- 增加 OSD 菜单背光调整功能（仅限 PWM 背光）

## 修改 ##
- 修复 UAC 和本地模式音频播放可能冲突的问题
- 修复 H264 解码失败的问题

# 20241009 #
## 优化 ##
- 优化传输延时。
- 优化内存占用。

# 20240914 #
## 新增 ##
- 增加待机 Video 播放模式，优化 OSD 菜单的交互逻辑。
- 增加手工切换 USB Display 开关。

## 修改 ##
- 修复多个副屏插入时的偶现卡死问题。
- 修复电脑插上副屏后，电脑复位偶现认证失败的问题。
- 修复笔记本电脑插上副屏后，笔记本盒盖待机后重新开盖亮屏偶现副屏不能同步亮屏的问题。
- 修复触摸屏在本地 LVGL 模式和电脑触摸屏模式来回切换多次后，偶现失灵的问题。
- 修复在 UAC 和 本地 Video 播放同时使用 Auido 冲突的问题。

# V1.0.5 #
## 新增 ##
- 调屏：支持和 AiPQ V1.1.1 工具配合使用
- 新增LVGL Demo：
- 支持VSCode 模拟器的工程导入
- 增加压力测试、独立控件、Gif、slide、lv_ffmpeg、DVP回显等参考实现
- MPP Player：支持avi封装格式
- USB Device：增加MTP支持
- USB Host：支持GPT分区格式
- PM：支持休眠时进入DDR自刷新
- aicupg：
  -支持和AiBurn工具配合完成Flash擦写功能
  -支持通过USB/UART获取设备侧的运行log
  -支持用命令进入U-Boot 升级模式
  -烧写时的进度条界面支持90/270度的旋转
- OTA：支持NFTL格式的Data分区升级；支持NOR、MMC介质
- TSen：增加对不同CP测试数据的兼容
- UART：增加流控功能支持；增加时钟分频的最佳匹配算法；增加软件模拟485的模式
- FS：支持生成 stripped FatFS 镜像，减小镜像文件大小
- DFS：SD0设备节点支持GPT分区格式
- GMAC：支持基于CHIP ID生成MAC地址；增加IPv6支持
- I2C：增加中断模式的处理流程；增加slave模式支持
- 新增SPI NAND：F35SQB004G、GD5F1GM7xUExxG、ZB35Q04A
- 新增屏幕：
  - RGB：gc9a01a
  - LVDS：LT8911
  -MIPI DSI：hx8394/ili9881c/st7703
  - i8080：st7789
- 新增CTP：st16xx、axs15260
- 新增第三方包：cJSON-1.7.16、mbedtls
- 新增aicp图片压缩库，压缩率更高
- 新增示例：test-twinkle、spi_flash、test_tsen_htp
- 新增工具支持：elf的size分析、编译时自动检测pinmux冲突、镜像文件大小匹配检查、version命令显示SoC型号
- OneStep：新增list-module命令，可查看当前方案中已打开的模块信息

## 优化 ##
- ADC：优化校准参数的算法
- Audio：优化关闭播放时的噪声；减少处理延迟
- DE：LVDS的双link可以单独配置参数
- PSRAM：优化初始化参数，提升稳定性
- USB：优化ADB写FatFS分区的性能
- SPI NAND：优化Page buf的内存拷贝流程；减少数据传输中的delay；
- SPI：优化DMA传输的结束状态判断
- SFUD：优化参数配置，提升工作频率
- D21x：优化DDR参数提升稳定性和兼容性
- aicupg：提升U盘烧录的兼容性

## 修改 ##
- MPP player：修正多个兼容性问题
- OTA：修正HTTP中的一处内存泄漏
- D12x：修改LDO1x电压为1.2V，稳定性更好
- D12x demo68：显示格式改为RGB565，和LVGL匹配
- aicupg：修正4K page size的处理
- GPAI：周期模式的默认周期改为 18ms
- UART：修正485模式的数据传输错误




# V1.0.4 #
## 新增 ##
- MPP：支持WAV文件解析；支持超短音频的播放
- Display：支持AiPQ调屏工具
- USB：支持MIDI、USB Audio、Device MSC、CDC
- IRQ：D12x、D13x支持中断嵌套、超级中断机制
- UART：支持 485 软件模式；支持DMA方式；支持流控功能
- LwIP：增加网络配置工具；增加PTPD协议支持；支持DHCP server；增加回环测试命令
- 新增PSADC、QEP驱动
- 烧写过程增加图形的进度条界面，进度条效果可配置
- aicupg：支持通过SD卡升级指定分区
- PBP：支持关闭PBP启动过程中的log
- 支持使用PWM实现hrtimer
- D12x支持PM流程
- 增加命令"scons --list-mem"，方便查看当前方案的内容布局
- menuconfig中增加可配置各个section的位置
- OneStep：Windows环境支持mb、mc命令；
- LVGL新增dashboard demo
- 移植Luban中的userid库
- 新增方案：d12x-hmi
- 新增NAND器件：XCSP1AAPK-IT
- 新增第三方包：libmodbus、sqlite、gif、
- 新增示例：test-multipwm、test-qep、test-keyadc
## 优化 ##
- 优化MP4格式的兼容性
- 完善一些特殊包的PPS、Slice处理
- 修正CPU loading的计算
- 优化stop时的资源释放处理
- Kconfig.board：简化依赖关系，每颗SoC只需要维护一份
- Eclipse：支持menuconfig方式进行动态配置SDK
- DMA：优化写内存后的同步处理
- RTP：优化event的上报流程，响应更快；保存校准文件到/data/config目录
- USB：优化UDC的枚举流程，提升兼容性
- SPI：优化多任务并发时的等待机制
- DVP：优化中断的处理流程
- LVGL增加GE透明功能的宏控制
- LVGL旋转支持逆时针、顺时针
- LVGL完善旋转后的搬移处理
- LVGL适配RTP触屏
- 工具链：精简其中的库文件，减小体积
- OneStep：完善Windows环境的路径切换处理
## 修改 ##
- I2C：支持发送restart信号
- CAN：修正工作模式的设置流程
- USB：修正ADB的printf()异常问题；改善ADB连接的稳定性
- GPAI：修正校准参数和计算过程
- NFTL：修正一处内存泄漏
- WiFi：修正rtl8189初始化过程中的异常



# V1.0.3 #
## 新增 ##
- 新增支持FreeRTOS内核
- 新增D12x
- NAND方案：全平台都已支持NFTL
- 支持OTA升级方案
- 新增支持ADB
- 新增支持HID Device，优化Custom IO Demo可接收图片、视频文件
- MPP：D13x、D12x支持MJPEG的解码
- PWM：支持输出指定个数的脉冲信号
- 新增屏支持：MIPI ili9488、LCD st7701s
- 新增WiFi支持：rtl8733、rtl8189
- 新增ESMT等厂家的多款NAND支持
- 新增CTP支持：ft7411、gsl1680，并增加相应的测试示例
- 新增Codec支持：tlv320
- LVGL Demo：支持动态旋转、缩放、任意角度旋转、多国语言、GIF图片
- 新增示例：test_fb、draw_line、test_i2s_loopback
## 优化 ##
- D12x: 仪表盘Demo优化达到58帧/s
- UART烧写：优化速率（最高可达3Mbps）和稳定性
- D21x：功耗优化
- 增强刷Cache时的对齐检查
- 优化调度入口的处理流程
- FATFS：支持sparse格式
- 优化PSRAM的稳定性；将PSRAM初始化统一放在PBP中
- 支持USB3.0的U盘
- 优化Device驱动的Buf性能
## 修改 ##
- CAN：修正HDR参数
- RTP：修正UP事件丢失的问题
- USB：修正U盘压力测试中出错的问题；
- RTP：将校准数据保存到文件中
- 默认关闭PM（功耗管理）功能
- 修改 application/os 目录名称为 application/rt-thread
- cherryUSB升级为v1.0.0版本
- gt911：修正多点触摸时的异常问题
- I2C：修正收发长报文的异常问题
- zlib解压缩时使用轮询方式
- 完善对B帧数据的处理
- 优化单曲循环播放时的切换机制
- Audio：优化播放流程，改善播放数据的完整性；设置最大音量为0db
- PWM：优化默认值、完善shadow寄存器的流程

# V1.0.2 #
## 新增 ##
- 支持动态APP加载
- 电源管理：支持休眠唤醒流程，新增light sleep模式
- 启动：NOR支持XIP模式、支持eMMC启动和烧写
- UI新增支持AWTK
- NAND：支持NTFL、FatFS
- SD卡：支持热插拔
- Audio：支持暂停功能
- Network：新增ping命令、支持MQTT协议
- USB：支持cherryUSB V0.10.0，USB Host和Device功能可用
- SPI：支持Slave模式（仅提供说明给gx客户）、Bit模式
- MPP：支持JPeg video
- Display: 支持SRGB、Gamma调节、PWM背光
- RTP：支持五点校准算法
- TSen：支持温度校准
- GPAI：支持自动校准
- HRTimer：适配D13x
- 支持UART烧写
- 支持OTA升级
- 支持ENV分区
- OneStep新增命令：add-board、rm-board、mc（clean + make）、
- 新增示例：test-tsen、test-gpai、test-rtp、test_gpio、test_i2c、test_ce、test_pm、test_rtc、ge_test
- 新增board：D13x demo88 NOR/NAND、D12x demo68 NOR/NAND



# V1.0.1 #
## 新增 ##
- 添加awtk支持
- 添加OTA功能

# V1.0.0 #
## 新增 ##
- 初始稳定版
- 支持D21X,D13X



