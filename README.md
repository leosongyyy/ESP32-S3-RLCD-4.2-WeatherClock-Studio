# ESP32-S3 RLCD 4.2 天气时钟 · 游戏版

> **简体中文（当前）** · [English](README_EN.md) · [繁體中文](README_zh_TW.md) · [日本語](README_JA.md)

这是一个基于 **ESP32-S3** 和 **4.2 英寸 RLCD 屏幕** 的低功耗天气时钟固件。它在常驻显示、本地温湿度、联网天气、日历、图片展示、音频提醒和 OTA 更新之外，新增了**第六个工作页「游戏」**，让这块单色墨水屏能直接玩 Game Boy 游戏。

## 关于本分支

本项目是 [`wickenzh/ESP32-S3-RLCD-4.2`](https://github.com/wickenzh/ESP32-S3-RLCD-4.2) 的分支。原始项目由 **wickenzh** 开发并维护，版权与许可归原作者所有，本分支完整保留其署名、许可证和上游链接。所有原创部分的非商业使用限制继续适用，详见 [`LICENSE`](LICENSE)。

本分支相对上游做了两件事：

**新增 Game Boy 模拟器工作页。** 用 Peanut-GB 内核配 minigb_apu 出声，ROM 从 TF 卡读取，支持蓝牙手柄、电池存档和 4 级灰度模拟。详情页见下方「游戏工作页」。

**裁掉一批模块给 app 分区腾空间。** 移除小智 AI 语音助手（`main/xiaozhi/` 及其 UI、模拟器预览、测试）、`weather_city_mcp`、固件发布工作流 `build-release.yml` 和三个网页构建脚本。app 分区上限 6MB，加入模拟器后已用到约 96%，不裁剪放不下。

上游其它能力（八个页面中的七个、天气、OTA、配网页、上位机、Power Demo）保持原样。

## 快速入口

**界面主要基于 LVGL 开发。** 页面布局、文字、菜单和状态控件由 LVGL 管理；大号时钟、图片和曲线等通过 LVGL Canvas 与自定义绘制实现。底层 RLCD 驱动负责单色输出，并优先采用局部刷新，兼顾显示效果与低功耗。

- [简易用户手册](docs/User_zh.md) · [用户详细说明](docs/User_Detailed_zh.md)
- [贡献指南 / Contributing Guide](CONTRIBUTING.md)
- [安全政策 / Security Policy](SECURITY.md)
- [第三方开源许可说明](THIRD_PARTY_NOTICES.md)
- [项目许可证](LICENSE)
- [最低功耗 Power Demo](docs/Power%20Demo/README.md)
- [实机模拟预览](https://wickenzh.github.io/ESP32-S3-RLCD-4.2/#screens)

## 关联项目

- [微雪 ESP32-S3-RLCD-4.2 官方产品页](https://www.waveshare.com/product/esp32-s3-rlcd-4.2.htm)：本项目所使用开发板的官方介绍、规格和购买信息。
- [微雪 ESP32-S3-RLCD-4.2 官方文档](https://docs.waveshare.com/ESP32-S3-RLCD-4.2)：开发板接口、原理图、示例和硬件资料。
- [ESP32-S3-RLCD-4.2_UP](https://github.com/wickenzh/ESP32-S3-RLCD-4.2_UP)：OTA 固件镜像仓库，可查看可用固件及版本信息。
- [在线上位机](https://wickenzh.github.io/ESP32-S3-RLCD-4.2/)：配置资源、转换图片/GIF、校验并烧录固件、查看串口日志；[源码与说明](host_web/README.md)。

## 游戏工作页

这是本分支新增的部分。

### 放 ROM

ROM 从 **TF 卡（microSD）** 读取，路径是卡根目录下的 `rom/gb`（设备内挂载点为 `/sdcard`，首次进入游戏页会自动建目录）。

- 卡需要格式化成 FAT32。
- 最多 64 个 ROM，单个 ROM 上限 4MB，文件名支持中文（UTF-8）。
- 也可以不开壳：进配网页后在同一局域网用浏览器上传，设备提供三个接口 `/roms/list`、`/roms/upload?name=xxx.gb`、`/roms/delete?name=xxx.gb`；单次上传上限 1300 KiB（约 1.27MB）。
- 没插卡时游戏页会显示「请插入 TF 卡」。

### 操作

| 场景 | 操作 | 效果 |
|---|---|---|
| ROM 列表 | BOOT 短按 | 在 4 行 ROM 与音量条之间循环移动焦点 |
| ROM 列表 | KEY 短按 | 焦点在 ROM 上开始游戏，焦点在音量条上加一档 |
| ROM 列表 | 手柄 上/下 | 移动焦点 |
| ROM 列表 | 手柄 A 或 B | 同 KEY 短按 |
| ROM 列表 | 手柄 左/右 | 音量减/加一档（快捷键，焦点自动跳到音量条） |
| 游戏中 | 两个实体键 | 同按 600ms 暂停，按住 2s 退出 |
| 游戏中 | 手柄 | A/B/十字键/START/SELECT 直接对应 |
| 任意 | BOOT 双击 | 进手柄绑定页 |
| 任意 | BOOT 长按 / KEY 长按 | 翻页 / 进设置 |

画面是 160×144 整数放大 2 倍到 320×288，居中显示，左右各留 40px 黑边。

### 4 级灰度

Game Boy 原机是 4 级灰度，而这块面板只有纯黑纯白两色，必须靠**空间抖动**用面积换灰度。

墨水屏**不能做时间抖动**（逐帧交替图案）。整屏刷新要几百毫秒到一秒以上，游戏帧率只有个位数，交替图案只会变成缓慢刺眼的闪烁，还会成倍放大残影。所以图案每帧固定，只取空间维度。

放大倍率固定 2 倍，于是每个 Game Boy 像素恰好对应面板上 2×2 四个物理像素，用「点亮其中几个」表示 4 档明暗。密度按 DMG 真实调色板 `#9bbc0f / #8bac0f / #306230 / #0f380f` 的相对亮度取 `{4, 3, 1, 0}`，不是均分的 `{4, 3, 2, 1}`：机身两个亮色挨得很近、两个暗色分得开，均分会把亮部差异凭空放大一倍，画面整体偏高对比，丢掉原本的柔和层次。

另有一个 4×4 Bayer 有序抖动模式可切换（在 160×144 原生分辨率上抖动再最近邻放大），纹理更细但孤立像素的灰度会丢失。默认用 2×2 空间 PWM，像素画保真度更高。两种模式都不增加帧缓冲和 SPI 开销。

### 蓝牙手柄

设备跑 **NimBLE Central**，直连市面上的 BLE 手柄，不再需要逐个按键对号入座。

- 内置 HID 1.11 报告描述符解析器，从 Report Map 直接推出按键、摇杆、Hat 的位偏移，比猜字节布局可靠。
- Hat 开关做**运行时自校准**，以观测值的众数作为静止中位。这条绕开了厂商常见的「描述符声明 0=上、固件实际发 0=中位」不一致。观测到超过 9 个不同取值就判定不是真 Hat，直接关掉方向键，而不是退化成摇杆去误判。
- 手动绑定页要求**连续按三次同一个键**才确认，防串键；绑定时只接受描述符里 BUTTON/HAT 类型的字节，摇杆和常量字段一律跳过。

如果手柄按钮出现错乱，先重启设备一次。自校准锚点和报告布局锁都只在内存里，断电即清空重来，绑定表存在 NVS 里会保留。

### 存档与音量

- **存档**：卡带 RAM 落盘为 `<rom名>.sav`，写 RAM 时只有值真的变了才置脏，在暂停和退出游戏时写入，启动时读回。不暂停直接断电会丢上次落盘之后的进度，这是电池存档的固有行为。
- **音量**：ROM 列表页底部一行音量条，静音 + 5 档（20/40/60/80/100），默认 80%，存 NVS 断电不丢。ROM 列表从 5 行缩到 4 行给它腾位置。音量只能在游戏外调，下一次启动游戏才生效。

## 八个工作页面

默认页面顺序如下，用户可以在设置中关闭页面或重新排序；系统始终保留至少一个可用工作页。

1. **天气时钟**：时间、日期、实时天气、预警、本地温湿度、电池和状态图标。
2. **图片时钟**：本地图片、大号分钟时间和每日文字；自定义图库支持多档切换周期。
3. **天气看板**：城市天气、空气质量、湿度、风力、日出日落、预警和未来天气。
4. **温湿时钟**：高对比时分秒、本地温湿度、趋势、日期和农历。
5. **日历**：当月日历、农历、节日和今日高亮；极端六行月份会保持当前日期可见。
6. **温湿历史**：记录并显示本地温湿度历史与趋势。
7. **游戏**：Game Boy 模拟器，见上一节。
8. **聚合时钟**：秒级时分秒、当天天气及最高/最低温、公历日号与农历月份/日期、本地温湿度，采用灰度点阵信息区与加粗数字。

聚合时钟默认追加在最后，可独立关闭或排序；天气共用缓存和整点同步，温湿度沿用白天每分钟、夜间每两分钟采样，时间仅更新变化数字。升级保留原页面配置，无需恢复出厂。离线模式下联网页面不可启用。

所有页面采用「能局部刷新就不全屏刷新」的原则。秒级页面只更新变化数字，分钟级页面只在时间、数据或显示状态变化时刷新；隐藏页面不会继续执行无意义的绘制。

## 项目定位

这个项目面向长期摆放使用的桌面天气时钟。设计重点不是追求持续联网或高刷新率，而是在信息始终可读的前提下，让 Wi-Fi、音频、蓝牙、高频 UI 和其它高功耗资源只在真正需要时运行。

主要目标：

- **常驻显示**：RLCD 在不持续刷新的情况下保持画面可读。
- **按需功耗**：页面不可见时停止其专属刷新，普通联网任务完成后关闭 Wi-Fi。
- **离线可用**：支持 RTC 保持时间，也可以不配置网络直接进入离线模式。
- **稳定优先**：OTA、音频、显示、蓝牙和网络任务之间设有资源与状态保护。
- **便于维护**：页面、天气、配置、音频、传感器、OTA 和游戏按职责维护。

## 配置与联网

天气服务可选和风天气或 Open-Meteo，配网页和网页快捷配置中的开关默认关闭。开启 Open-Meteo 无需和风 Key/Host，显示 US AQI，不提供本项目预警；两个来源不自动混用。公共服务仅限非商业使用，数据来自 [Open-Meteo](https://open-meteo.com/) / [CAMS](https://atmosphere.copernicus.eu/)，城市查询使用 [GeoNames](https://www.geonames.org/)。

首次使用通过设备配网页完成配置：

- 支持主 Wi-Fi 和可选备用 Wi-Fi；主网络连续不可用时会尝试备用网络，成功后可按既有策略晋升。
- 联网天气需要 QWeather API Key 和账号专属 API Host；API Host 在 QWeather 控制台中查看。
- 天气城市可以使用公网 IP 自动定位，也可以通过配网页或上位机设置手动城市。
- 不配置 Wi-Fi 时，可以填写本地日期时间并进入离线模式。
- 同一个配网页提供 ROM 上传入口，见「游戏工作页」。

设备联网行为按需执行：

- 天气时钟、天气看板和聚合时钟共用天气缓存，在需要时按整点及缺数据补拉规则同步；开机为已启用页面错峰预取。
- 图片时钟在需要时获取每日文字。
- NTP 在启动、手动同步和既定校时点运行。
- OTA、网络检测和手动同步只在用户主动操作时运行。
- 蓝牙只在游戏页和手柄绑定页启停，离开后一段时间无人请求就自动停掉。
- 离线模式、低电量、配网和 OTA 期间会拦截不合适的后台联网任务。

天气与每日文字使用本地缓存。短时网络失败不会清空已有内容，后台重试受次数和退避限制，避免 Wi-Fi 反复上电。

## 提醒与交互

- 两个实体按键用于页面切换、菜单移动、确认、返回和停止提示音。
- 支持整点提醒、全天提醒和多档音量设置。
- 闹钟与番茄钟使用独立状态，并对相同触发时刻进行冲突保护。
- 番茄钟模块保留，可用按键停止提醒；语音控制的入口随小智 AI 一起移除。

## 低功耗策略

固件在不改变屏幕常驻显示的前提下减少无效工作：

- 启用 ESP-IDF 动态电源管理与 FreeRTOS Tickless Idle：CPU 在负载允许时从 `240MHz` 自动降至 `40MHz`，空闲窗口自动进入 Light Sleep；联网、音频和 OTA 只在工作期间通过 PM Lock 临时保持所需性能。
- 秒级页面采用局部刷新；静态页面在没有事件时暂停周期绘制。
- 温湿度、电池和历史采样按运行时段及充电状态调度。
- 普通天气、每日文字、NTP 和 OTA 会错峰执行，避免启动阶段集中占用内存与 Wi-Fi。
- 音频播放结束后释放 Codec 和相关资源；游戏音频只在游戏页运行，音频采样率固定 32kHz。
- 蓝牙按需启停：只有游戏页和手柄绑定页会拉起手柄服务，离开且一段时间无人请求后自动停掉。
- TF 卡只在进入游戏页或上传 ROM 时访问。
- Wi-Fi 图标与无线电实际状态保持一致，便于直接观察高功耗网络是否仍在运行。
- OTA 下载期间降低 UI 刷新频率，并保留百分比、速度和进度条。
- 低电量模式会进入极简显示并禁止高功耗操作。

`docs/Power Demo/` 中的 [Power Demo](docs/Power%20Demo/README.md) 是独立实验工程，用于验证保留屏幕显示、分钟级唤醒和最小外设条件下的功耗边界。它不包含完整版设置、OTA、多页面和游戏业务，也不参与正式固件构建。

## 硬件与软件

- 开发板：[Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/product/esp32-s3-rlcd-4.2.htm)。
- 主控：ESP32-S3-WROOM-1-N16R8，16MB Flash、8MB PSRAM（Octal 80MHz）。
- 显示：4.2 英寸 400 × 300 RLCD，单色 1bpp。
- 存储：板载 TF（microSD）卡槽，SDMMC 1-bit 模式；另有 NVS 保存联网与用户设置，独立资源分区保存可更新素材。
- 本地设备：RTC、温湿度传感器、电池 ADC 和实体按键。
- 音频：ES8311 Codec、功放、扬声器和麦克风，用于提示音与游戏音频。
- 网络：Wi-Fi（配网、天气、每日文字、NTP、OTA、ROM 上传）与 Bluetooth LE（NimBLE Central/Observer，最多 1 路连接）。
- 开发框架：ESP-IDF `v5.5.3`；图形界面：LVGL `v8.4.0`；音频编解码：`esp_audio_codec` `v2.4.1`。

## 源码结构

- `RLCD_CLOCK/main/`：应用入口和按业务划分的固件模块。
  - `main/game/`：Game Boy 模拟器。内核 `peanut_gb.h` 与适配层 `peanut_gb_impl.c`、渲染 `gameboy_fb.cpp`、ROM 与存档 `gameboy_rom_store.cpp`、音频 `game_audio.cpp`、手柄 HID 解析 `gameboy_hid.c`、BLE 主机 `gameboy_pad_ble.c`、绑定 `gameboy_pad_bind.c`、生命周期 `gameboy_runtime.cpp`。
  - `main/ui/ui_game.cpp`、`main/ui/ui_game_bind.cpp`：游戏页与绑定页。
  - `main/network/rom_upload.cpp`：配网页的 ROM 列表/上传/删除接口。
- `RLCD_CLOCK/components/`：板级支持、显示、音频、资源和第三方组件。
  - `components/minigb_apu/`：Game Boy APU 模拟器组件。
- `RLCD_CLOCK/simulator/`：SDL 页面预览生成器，`RLCD_CLOCK/tests/`：主机端单元测试。
- `RLCD_CLOCK/partitions.csv`：设备分区表。
- `RLCD_CLOCK/CMakeLists.txt`：ESP-IDF 工程入口。
- `RLCD_CLOCK/sdkconfig.defaults`：默认工程配置。
- `docs/`：用户手册和独立 Power Demo。
- `previews/`：由当前源码生成的 SDL 页面预览和总览图。
- `host_web/`：网页上位机站点文件与测试。
- `.github/`：网页部署工作流。

构建与提交要求见 [贡献指南](CONTRIBUTING.md)。先阅读[简易用户手册](docs/User_zh.md)，完整刷机、配网、页面设置和故障处理见[用户详细说明](docs/User_Detailed_zh.md)。

## 构建与烧录

```bash
cd RLCD_CLOCK
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem21101 flash monitor
```

首次构建会通过 ESP-IDF 组件管理器拉取 `dependencies.lock` 里锁定的依赖（LVGL、esp_audio_codec 等），落到 `RLCD_CLOCK/managed_components/`，该目录不入库，需要联网。

固件以 GitHub Release 形式发布，每个版本提供四个产物，按各自偏移烧：

```bash
esptool.py --chip esp32s3 -p /dev/cu.usbmodem21101 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin \
  0x20000 weather_clock.bin
```

已有设备做 OTA 的话只烧 `0x20000` 那个 app 就行。

app 分区上限 6MB，当前固件占用约 5.76MB（96%），剩余约 243KB。再往里加功能需要先裁剪别的模块。

## 已知问题

- **手柄自动重连偏慢**：部分手柄每次开机广播的蓝牙地址末位会变，NVS 里记的旧地址对不上，于是先空等约 13s 连接超时才转扫描，转扫描后 1s 内连上。能用，只是慢。
- **音量只能在游戏外调**，下一次启动游戏才生效。
- **存档只在暂停和退出时落盘**，不暂停直接断电会丢进度。
- **ROM 上传单次上限 1300 KiB**，更大的 ROM 请直接拷到 TF 卡。
- **app 分区只剩约 243KB**，添加新功能前先评估体积。

## 开源来源与第三方许可

Game Boy 模拟器部分使用了以下第三方项目，均为 MIT License，许可文本随各自源码保留：

- [`Peanut-GB`](https://github.com/deltabeard/Peanut-GB) `main/game/peanut_gb.h`，Copyright (c) 2018-2023 Mahyar Koshkouei。
- [`minigb_apu`](https://github.com/deltabeard/minigb_apu) `components/minigb_apu/`，Copyright (c) 2019 Mahyar Koshkouei、Copyright (c) 2017 Alex Baines，基于 [MiniGBS](https://github.com/baines/MiniGBS)。

上游原项目的小智 AI 页面基于 [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) 移植。本分支已移除该页面及其全部源码，其 MIT 版权声明、许可文本和免责声明仍保留在 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) 中，不作删除。

`xiaozhi-esp32` 由 Shenzhen Xinzhi Future Technology Co., Ltd. 及项目贡献者以 MIT License 发布。本项目的非商业许可证只约束项目维护者拥有版权的原创部分，不覆盖第三方许可证已经授予的权利。

## 贡献与安全

提交 Issue 或 Pull Request 前请阅读：

- [贡献指南 / Contributing Guide](CONTRIBUTING.md)
- [安全政策 / Security Policy](SECURITY.md)

请勿在 Issue、日志、截图或提交中公开 Wi-Fi 密码、QWeather API Key、API Host 私有配置、Token、NVS 镜像、设备私钥、本机绝对路径或私有服务地址。

## 使用限制

本项目仅允许个人学习、研究、评估和其它非商业用途。

**严禁商用。**

未经项目所有者书面许可，禁止将项目维护者拥有版权的原创部分用于商业产品、付费服务、批量销售、转售、营利性分发或其它商业交付。第三方组件仍遵循各自原始许可证。详细条款见 [`LICENSE`](LICENSE)。
