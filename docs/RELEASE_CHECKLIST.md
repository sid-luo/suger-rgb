# Suger RGB 发布检查清单

## 版本与构建

- [ ] `VERSION`、`CHANGELOG.md` 与 `docs/installer/manifest.json` 版本一致；
- [ ] 使用干净的 ESP-IDF 5.4.x 环境执行 `idf.py build`；
- [ ] 执行 `./tools/package_web_firmware.sh`；
- [ ] 合并固件通过 `esptool image_info`；
- [ ] `SHA256SUMS.txt` 与实际文件一致。

## 首次安装

- [ ] 从未知固件开始，网页安装并选择清除设备；
- [ ] 安装器拒绝非 ESP32-C3 芯片；
- [ ] 首次启动显示低亮度白光追逐；
- [ ] `Suger-RGB-XXXX` 为无密码热点；
- [ ] Captive Portal 与 `http://192.168.4.1/` 均可打开；
- [ ] 中英文切换、Nightscout 验证、保存和重启正常。

## 正常运行与实体权限

- [ ] 保存后设备只连接家庭 Wi-Fi，不保留 AP；
- [ ] 正常运行的局域网 IP 没有 HTTP 设置页面；
- [ ] 长按 BOOT 约 5 秒后才开放配网页；
- [ ] 配网状态下再次独立长按约 5 秒，三次白光反馈后清除连接配置；
- [ ] 强制恢复保留灯珠数量。

## 数据与灯效

- [ ] Nightscout URL 自动补全和匿名读取正常；
- [ ] 五档颜色与 README 表格一致；
- [ ] `↑ ⇈ ↓ ⇊` 四种动态趋势与设置页预览一致；
- [ ] `<3.3 mmol/L` 与 `≥15 mmol/L` 优先级正确且不可关闭；
- [ ] 10 分钟无新数据进入白光呼吸。

## 发布

- [ ] GitHub Pages 安装页通过 HTTPS 加载；
- [ ] 安装页不依赖第三方 CDN；
- [ ] 灯效演示可自动轮播，并能手动切换血糖、趋势和三种灯珠布局；
- [ ] 灯效演示的中英文、颜色区间和固件自动映射一致；
- [ ] GitHub Release 附带合并固件和 SHA-256；
- [ ] 国内镜像使用相同固件并核对 SHA-256；
- [ ] README、英文 README、许可和第三方声明均为当前版本。
