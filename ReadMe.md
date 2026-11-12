# V1.64 产品配置指南

## SN 方向与 framebuffer 旋转

固件通过 `framebuffer rotation degree` 设置平台旋转。所有 SN 修改都只在
`application/rt-thread/helloworld/screen_sn_config.h` 中完成：修改完整的
`USB_CDC_DEFAULT_SN` 后重新编译并一起烧录。上位机根据 SN 中的 A/B/C/D 和
分辨率自行适配；SN 不与平台旋转联动。

画面旋转使用 SDK 官方 framebuffer/GE 链路，触摸坐标跟随 framebuffer
旋转，不跟随 SN 字母。面板时序仍保持原生宽高，不要交换
`AIC_JD9365_HACTIVE/VACTIVE`。切换配置后应重新完整编译固件；Flash 中
已有的 SN 不会被自动改写。

全部通过 SDK menuconfig（`scons --menuconfig`，配置保存在 `target/configs/d21x_d215-demo88-nor_rt-thread_helloworld_defconfig`）宏切换，无需改代码。

## 1. LOGO 模式切换（动画 / 图片）

| 配置项 | 动画 LOGO 模式 | 图片 LOGO 模式 |
|---|---|---|
| `CONFIG_LV_USB_OSD_LOGO_TYPE_VIDEO` | =y | 注释掉 |
| `CONFIG_LV_USB_OSD_LOGO_TYPE_IMAGE` | 注释掉 | =y |

- 切换后 SConscript 自动选择对应的 USB 仲裁层编译：
  - VIDEO 模式 → `packages/third-party/cherryusb/demo/cdc_acm_template.c`
  - IMAGE 模式 → `packages/third-party/cherryusb/demo/cdc_acm_template_static_logo.c`
- 图片 LOGO 资源：`packages/artinchip/lvgl-ui/lvgl_src_nor/lvgl_data/logo_rotate/logo.png`（ROTATE_90 布局）。
- 图片位置：`CONFIG_LV_USB_OSD_LOGO_IMAGE_X/Y`，默认 **-1 表示自动居中**；也可设为具体坐标。

## 2. 触摸功能开关

| 配置项 | 开启触摸 | 关闭触摸 |
|---|---|---|
| `CONFIG_AIC_USB_OSD_TOUCH_ENABLE` | =y（默认） | 注释掉或 =n |

- 关闭时：gt911/HID 触摸数据通路完全停用（`touch.c` 编译为空桩、HID 接口 `drop_hid_data=1`），USB 复合设备描述符布局保持不变（CDC+HID 枚举不变，仅无触摸数据）。

## 3. 防花屏机制（上电 LOGO 显示后进系统前）

- **两种 LOGO 模式**：Display 初始化延迟均强制为 0；等待 Display 工作线程完成 framebuffer/图层初始化后才提交 LOGO，避免延迟初始化覆盖已显示的 LOGO。配置中的延迟值仅对未启用 LOGO 仲裁的产品生效。
- **动画模式**：视频首帧回调后开背光；开机/唤醒动画完整播放一次后保留最后一帧，只有主机有效画面就绪才释放。重复 D0 不得把 UI 透明度改成 255 遮住底层视频。
- **图片模式**：`lv_refr_now` 提交静态 LOGO 并等待 VSYNC 后才开背光；USB 提前完成枚举不影响可见确认。CDC 与 PNG 共用 UI 层，交接时不再重绘黑色背景覆盖已经提交的主机首帧。
- **枚举/唤醒**：已可见 LOGO 期间，重复 D0 和通用 OSD 状态机不能重设图层、停止视频或清屏；真正的休眠与正常模式切换仍由原仲裁路径处理。
- USB suspend 恢复时通过 `cdc_image_processor_reset_stream()` 丢弃跨会话残缺 JPEG 帧，避免花屏。

## 4. 电脑睡眠/休眠/重启行为

| 操作 | 动画模式 | 图片模式 |
|---|---|---|
| 电脑睡眠（D3） | LOGO 挂起/保持最后一帧 | 静态 PNG 遮盖主机保留帧 |
| 电脑唤醒（D3→D0） | 重新请求播放 startup 动画 | 遮盖保持至新 JPEG 帧渲染后释放 |
| 电脑重启 | USB RESET → 重新完整播放 | USB RESET 阶段预置静态 LOGO |

当前主体功能包括 CDC 显示数据流、SN 读写和背光控制，均以 V1.64 为统一开发基线。

---

Luban-Lite SDK 是由ArtInChip设计的，旨在为系统级芯片(SoC)设计提供一个轻量级、高效且易于使用的软件开发工具包。该SDK的设计规划兼顾了简单性与广泛的客户支持，同时确保对主流实时操作系统(RTOS)的兼容性。

通过这个仓库，匠芯创科技期望与全球开发者共同打造一个功能丰富、易于使用的嵌入式开发生态，推动技术的创新与发展。

# 架构与设计介绍
Luban-Lite SDK的架构设计分为两种主要情况，根据是否使用操作系统(OS)进行区分：

- **RTOS架构**：当使用RTOS时，Luban-Lite提供了一个结构化的框架，使得在不同的RTOS间移植变得容易。它抽象了底层硬件的差异，提供了统一的API接口，使得应用程序可以无缝地在不同的RTOS上运行。 RTOS 架构兼容市面上最流行的几种RTOS内核，如RT-Thread、FreeRTOS等，为开发者提供灵活的实时系统选择。

- **Baremetal架构**：在不使用RTOS的情况下，Luban-Lite提供了一套完整的库和驱动，允许开发者直接在硬件上编写和运行代码。Baremetal 架构为那些不需要完整操作系统的应用程序提供直接在硬件上运行的能力，从而减少开销并提高性能。

Luban-Lite SDK 还具备完整的软件栈生态资源，提供丰富的库和工具，促进快速开发和部署，包括驱动程序、协议栈、中间件和示例应用程序。

# 功能与特性
Luban-Lite SDK的主要功能和特性包括：

- 源码自动检测机制：SDK能够自动检测源码的更改，并触发相关的构建过程，简化了开发流程。
- 文件系统镜像制作：提供了制作不同类型文件系统镜像的工具，方便开发者根据项目需求创建定制化的文件系统。
- 模块化设计：SDK采用模块化设计，各个软件包和组件可以独立更新和编译，提高了代码的可维护性和扩展性。

# 使用前提条件
使用Luban-Lite SDK之前，开发者需要确保满足以下前提条件：

- 硬件平台：确保拥有与Luban-Lite兼容的ArtInChip SoC硬件平台。
- 开发环境：设置合适的开发环境，包括必要的编译器、工具链和IDE。
- 基础知识：具备基本的编程知识，熟悉C或C++语言，以及对嵌入式系统开发的基本理解。

# 授权与使用条件
Luban-Lite SDK为开源项目，遵循相关开源协议，授权任何个人和组织使用、复制、修改、合并和分发。但在对SDK进行修改和衍生时，必须保留原作者的版权信息，并在衍生作品中声明更改的部分及原作者信息。

# 文档和资源
本仓库不仅提供了Luban-Lite SDK 的安装和配置指南，还包含了丰富的示例代码、开发教程和API参考文档，帮助开发者快速上手并充分利用SDK的功能。

Luban-Lite SDK 的文档请参考 doc 目录，是广东匠芯创科技有限公司对技术透明性和开放性承诺的体现，旨在为全球的开发者和工程师提供一个可靠的资源中心，以支持他们在各种硬件平台上的创新和产品开发。

# 参与与贡献
匠芯创科技鼓励开发者积极参与项目，贡献代码或提供改进建议。请咨询仓库管理员，了解具体的贡献流程和要求。

总体而言，Luban-Lite SDK是为那些寻求在ArtInChip SoC上快速开发高效、可靠应用的开发者设计的。它的设计理念是简化开发过程，同时提供足够的灵活性以支持各种实时操作系统和裸机应用，使得开发者能够根据具体需求选择最适合的开发路径。




