# 安装 Suger RGB

## ⚠️ 不要使用 ESP32-C3 Super Mini，请使用 ESP32-C3 Pro Mini

> [!WARNING]
> **请勿购买或使用 ESP32-C3 Super Mini（SuperMini）制作本项目。** 这类板子可能出现 Wi-Fi 不稳定、搜不到配网热点或配网失败等问题，即使固件刷写成功也可能无法正常联网。
>
> 本项目实测曾遇到上述问题，换用 **ESP32-C3 Pro Mini** 后恢复正常。**请直接选用 ESP32-C3 Pro Mini。**

[English](INSTALL.en.md) · [返回项目首页](../README.md)

推荐使用网页安装器。它会检查芯片型号，并把 bootloader、分区表和应用程序作为一个完整固件写入 ESP32-C3；不需要安装 ESP-IDF 或使用命令行。

## 网页安装

### 准备

- ESP32-C3 Pro Mini，4 MB Flash；
- 能传输数据的 USB 线；
- Windows、macOS 或 Linux 电脑；
- 桌面版 Chrome 或 Edge；
- 稳定的网络连接，用于打开页面和下载约 1 MB 固件。

Safari 与 iPhone 不支持网页串口刷写。开始前请关闭串口监视器、Arduino IDE、PlatformIO 等可能占用串口的软件。

### 操作

1. 打开 [Suger RGB 网页安装器](https://rgb.sidluo.com/installer/)。
2. 使用 USB 数据线连接开发板。
3. 点击“安装 Suger RGB”。
4. 在浏览器弹窗中选择 ESP32-C3 对应的串口。
5. 确认安装；网页刷写会清除旧固件和已经保存的配网信息。
6. 确认安装并等待开发板自动重启。

安装器只接受 ESP32-C3；检测到其他芯片时不会开始写入。刷写在浏览器与本地 USB 设备之间完成，安装页不会获得 Wi-Fi 密码或 Nightscout 配置。每次网页刷写完成后都需要重新配网。

### 刷写完成后

首次启动时，灯会显示低亮度白光追逐，并创建无密码热点 `Suger-RGB-XXXX`：

1. 用手机连接该热点；
2. 等待配网页自动弹出，或访问 `http://192.168.4.1/`；
3. 填写家庭 Wi-Fi、Nightscout 地址和灯珠数量；
4. 验证读到真实血糖后保存。

保存后设备重启并关闭设置网页。正常运行期间，局域网 IP 不提供管理入口；以后如需修改，长按实体 BOOT 按键约 5 秒重新进入配网状态。

## 浏览器找不到开发板

依次检查：

1. 换一根确认可以传数据的 USB 线；
2. 换一个电脑 USB 接口，避免无供电或不稳定的扩展坞；
3. 关闭所有占用串口的软件和其他网页；
4. 拔掉开发板，按住 BOOT 后重新插入 USB，再松开 BOOT；
5. 如果开发板带 RST 按键，也可以按住 BOOT、短按 RST、再松开 BOOT；
6. 重新点击安装并选择新出现的串口。

部分使用外置 USB 转串口芯片的开发板可能需要 CH34x、CP210x 或厂商驱动；ESP32-C3 原生 USB Serial/JTAG 通常不需要额外驱动。

## 从源码编译

项目使用 ESP-IDF 5.4.x：

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Windows 串口通常类似 `COM5`。退出监视器使用 `Ctrl+]`。

## 使用合并固件手动烧录

网页安装器使用的完整固件位于：

```text
docs/installer/firmware/suger-rgb-esp32c3.bin
```

安装 ESP-IDF 或 esptool 后可以写入地址 `0x0`：

```bash
esptool.py --chip esp32c3 --port PORT write_flash 0x0 \
  docs/installer/firmware/suger-rgb-esp32c3.bin
```

该合并固件与网页安装相同，会覆盖旧配置，写入后需要重新配网。

每次发布都会同时提供 `SHA256SUMS.txt`，可用于核对下载文件是否完整。

## 更新网页安装固件

维护者在 ESP-IDF 环境中运行：

```bash
./tools/package_web_firmware.sh
```

脚本会重新编译项目、生成地址从 `0x0` 开始的合并固件、执行镜像校验，并更新 SHA-256 文件。发布前还应完成 [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md)。
