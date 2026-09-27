# ESP32 界面迁移到 LVGL：设计草案

日期：2026-09-27 · 状态：用户同意先完成一版，可在不满意时回退

## 目标

把现有 App 首页、系统监控页、AI 用量页统一交给 LVGL 绘制和处理触摸，继续保持开机首页、点 Monitor 进入、页间切换、房子图标返回。迁移的目的在于让图标、按钮、图表和后续 App 共用同一套组件与交互，不改变现有 Mac 数据接口、设备 Wi-Fi 配置或刷写分区。

## 已核对的接入条件

- 屏幕是 ESP-IDF 的 800×480 RGB565 LCD panel，`waveshare_esp32_s3_rgb_lcd_init()` 返回 `esp_lcd_panel_handle_t`；现有 LCD 配置为两块帧缓冲，并开启 PSRAM。
- `touch_gt911_init()` 返回项目自带的 `esp_lcd_touch_handle_t`，但项目采用私有 `touch.h`，没有 LVGL port 的 `esp_lcd_touch.h`；直接调用 port 的触摸快捷封装会额外引入同名驱动实现。LVGL 通过输入回调读取现有 `touch_gt911_read_point()`，主循环不再读取触摸。
- Espressif 的 `espressif/esp_lvgl_port` 2.9.0 支持 ESP-IDF 5.2+、LVGL 8/9，并提供 `lvgl_port_add_disp_rgb()`；LVGL 有内置 `LV_SYMBOL_HOME` 房子符号。设计以 LVGL 9 为目标并锁定依赖版本。

## 方案与用户动线

启动时先初始化现有 LCD panel、GT911 和背光，再通过 `esp_lvgl_port` 接管屏幕刷新，并向 LVGL 注册使用现有 GT911 的输入回调；首页显示 Monitor 卡片。用户点卡片看到系统监控页，滑动切换 AI 用量页，点左上角房子图标回首页。AI `REFRESH`、亮度切换、Mac 离线提示与现状一致。

首页、两个监控页都使用 LVGL screen；Monitor 卡片使用 `lv_button`，返回控件使用**白色 `LV_SYMBOL_HOME` 图标，不画背景色或按钮底块**，同时保留至少 44×44 的透明可点击区域。曲线与进度用 LVGL 的图表和样式。复用已有颜色、文字和两分钟采样数据，不改变字段含义。触摸全部由 LVGL 输入设备读取，不再由主循环同时轮询 GT911；一次点击只派发一次导航或刷新。

`main.c` 保留 Wi-Fi/NVS/HTTP/串口与一秒一次的取数任务；UI 模块独占 LVGL 对象和页面切换。主循环获取数据后短暂持有 `lvgl_port_lock()` 更新组件，不在 LVGL 回调或持锁期间执行 HTTP 请求。刷新按钮向取数任务发送请求，再由原有逻辑调用 Mac 的 `/api/ai/refresh`。把现有监控状态类型移到两侧共用的窄头文件，避免维护第二套接口字段。

## 帧缓冲与取舍

首选 `lvgl_port_add_disp_rgb()` 的 RGB 避免撕裂模式，复用 LCD panel 已分配的两块缓冲；不再同时手动调用 `waveshare_get_frame_buffer()`、`waveshare_rgb_lcd_display()` 或当前逐像素绘图函数，以免两个渲染器同时拥有显存。800×480 RGB565 的双缓冲约占 1.5 MB，构建和实机验证时要检查 PSRAM 余量、动画/图表刷新与长时间显示的稳定性。

不采用“首页用 LVGL、监控页保留手绘”的混合方式：两套系统需要轮流控制同一个 panel、双缓冲与触摸，后续每加一个 App 都会继续承担切换成本。也不恢复原厂二进制或更改分区表。

## 影响面和验收

- 固件增加 LVGL 与 Espressif port 依赖；修改 `firmware-idf/main/idf_component.yml` 和锁文件。
- `firmware-idf/main/main.c` 保留数据链路并移走屏幕与触摸的直接操作；新增独立的 LVGL UI 模块及共享数据类型。Mac 端代码与串口配置协议不变。
- 编译通过，镜像仍放得进 3 MB 应用分区；Wi-Fi 与 NVS 配置保持可用。实机验证开机首页、Monitor 进入/返回、两页滑动、用量刷新、离线状态和背光操作。
- 观察帧缓冲与 PSRAM 分配、是否有撕裂或字体闪烁，并确认连续运行时网络取数与触摸都能响应。

迁移全部三个页面。当前可用固件的应用映像在本机保留一份作为回退基线；回退只写应用分区，不清除 NVS 设置。
