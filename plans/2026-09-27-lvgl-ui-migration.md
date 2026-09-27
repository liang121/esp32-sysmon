# ESP32 LVGL UI Migration Implementation Plan

**Goal:** 将首页、系统监控、AI 用量三页统一迁到 LVGL；返回键只显示白色房子图标，无底色。

**Architecture:** 保留单固件和现有 Wi-Fi/NVS/HTTP 主循环。新增 `ui.c`/`ui.h` 管理 LVGL screen、图表、按钮和触摸回调；主循环更新数据时拿 LVGL 锁，刷新按钮只发非阻塞请求，由主循环执行 HTTP。

**Tech Stack:** ESP-IDF 5.5、LVGL 9、espressif/esp_lvgl_port 2.9、ESP32-S3 RGB565 panel、GT911 触摸。

**Spec:** `specs/2026-09-27-lvgl-ui-migration-design.md`

## Global Constraints

- `firmware-idf/partitions.csv`、NVS 键、Mac 数据接口和串口状态格式不变。
- 两页统计字段与 Claude Code / Codex 的五小时和每周用量语义不变。
- 返回键白色 `LV_SYMBOL_HOME`，透明至少 44×44 点按热区，不画彩色或实心背景。
- LVGL 独占 LCD 帧缓冲和 GT911；Mac 网络请求永不在 LVGL 回调中阻塞执行。
- 现有可用应用映像已保存在被 `.gitignore` 排除的 `firmware-idf/build/rollback-sysmon-b6fb190.bin`，仅供本机回退。

## Task 1：安装并接入底层驱动

**文件：**`firmware-idf/main/idf_component.yml`、`firmware-idf/main/CMakeLists.txt`、`firmware-idf/dependencies.lock`、`firmware-idf/main/main.c`、`firmware-idf/main/ui.h`。

- [ ] 将 `lvgl/lvgl` 9.x、`espressif/esp_lvgl_port` 2.9.x 放进组件管理清单并锁定依赖；刷新 CMake 组件引用。
- [ ] 使用现有 `esp_lcd_panel_handle_t` 初始化 LVGL RGB port，让 RGB565 和两块 LCD 帧缓冲只由它管理；GT911 保留原驱动，由 LVGL 自己的输入回调读取。
- [ ] 编译一个能显示 LVGL 首页文字的固件，检查新应用镜像仍小于 `0x300000` 应用分区。

## Task 2：建立三页 UI 与导航

**文件：**新增 `firmware-idf/main/ui.c`，修改 `firmware-idf/main/ui.h`、`firmware-idf/main/main.c`。

- [ ] 使用 LVGL 控件创建首页 Monitor 卡片、系统监控页和 AI 用量页。左上角仅绘制白色房子符号，透明按钮承接点击。
- [ ] 将 CPU/内存/交换空间与进程、图表、两套 AI 用量窗口及错误状态绑定现有数据，保持首页 Mac 在线提示。
- [ ] 用 LVGL 的点击、滑动与背景点击事件取代 GT911 主循环轮询。点 Monitor 进入系统页、左右滑切页、两页房子图标回首页；空白处切换亮度。
- [ ] 主循环一秒一次更新数据，`lvgl_port_lock()` 只包围短时间 UI 更新；AI 刷新回调置位标志，主循环执行 HTTP 并随后更新画面。
- [ ] 编译、静态检查三页导航和离线提示，确认原有串口状态与 Wi-Fi 切换逻辑仍存在。

## Task 3：上板验证与交付

**文件：**`README.md`、设计稿、实施计划和以上源码/依赖文件。

- [ ] 运行 `idf.py -C firmware-idf build`，确认固件尺寸、编译通过；核对改动中没有凭据、固件备份或编译产物。
- [ ] 用 `arduino-cli board list` 再确认串口，然后使用 `idf.py -C firmware-idf -p /dev/cu.usbmodem11301 flash` 刷写；串口变化时改为实际端口。
- [ ] 重启后检查设备 Wi-Fi、Mac `/api/now` 与串口 `last=ok`；屏幕现场验证首页、房子图标、系统和 AI 图表、滑动与手动刷新。记录无法远程观察的触摸细节。
- [ ] 若新固件无法启动或持续无法取数，使用原应用分区的已验证镜像 `rollback-sysmon-b6fb190.bin` 在 `0x10000` 恢复原版；不擦除 NVS。
- [ ] 更新 README 双语描述与回退说明；代码、依赖锁、设计稿和计划作为一个功能提交推到个人 GitHub，核对公开远端 SHA。
