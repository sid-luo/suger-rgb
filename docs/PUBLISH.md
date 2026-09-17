# 发布 Suger RGB

本文面向项目维护者。普通用户请阅读 [`INSTALL.md`](INSTALL.md)。

## 1. 准备版本

1. 更新根目录 `VERSION`；
2. 同步更新 `CHANGELOG.md`；
3. 同步更新 `docs/installer/manifest.json` 中的 `version`；
4. 在 ESP-IDF 5.4.x 环境执行：

```bash
./tools/package_web_firmware.sh
```

5. 完成 [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md)。

## 2. 网站入口与部署

当前正式入口为：

- 网页安装器：<https://rgb.sidluo.com/installer/>
- 灯效演示：<https://rgb.sidluo.com/demo/>

更新网站时，将整个 `docs/` 目录部署到网站托管服务，保持目录结构不变。

如需另外启用 GitHub Pages，仓库上传至 `sid-luo/suger-rgb` 后：

1. 打开仓库 `Settings`；
2. 进入 `Pages`；
3. 在 `Build and deployment` 中选择 `Deploy from a branch`；
4. 选择 `main` 分支和 `/docs` 目录；
5. 保存并等待部署完成。

`docs/.nojekyll` 必须保留。安装页、ESP Web Tools、清单和固件都位于 `/docs`，因此不依赖第三方 CDN。

## 3. 创建 GitHub Release

建议为每个版本附带：

- `suger-rgb-esp32c3.bin`；
- `SHA256SUMS.txt`；
- 该版本对应的源代码；
- 从 `CHANGELOG.md` 摘取的更新说明。

网页安装器使用的合并固件会清除旧配置。发布说明中应明确提示用户刷写后重新配网。

## 4. 国内镜像

将整个 `docs/` 目录原样部署至支持 HTTPS 的国内静态网站托管即可。不要只复制 `index.html`，以下内容必须保持相对目录不变：

```text
installer/manifest.json
installer/firmware/
installer/vendor/esp-web-tools/
installer/assets/
demo/
```

部署后分别检查中文、英文、灯效演示、固件下载响应和 SHA-256。镜像与正式站应使用同一个发布版本，不单独修改固件。
