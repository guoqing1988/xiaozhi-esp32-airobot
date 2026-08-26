# Bread Compact Wi-Fi S3Cam AI Robot (面包板)

本板由 `bread-compact-wifi-s3cam` 克隆而来，作为 DIY 的基础版本。

- 硬件基于 ESP32-S3 CAM 开发板，摄像头为 OV2640（占用 IO 较多，占用了 ESP32S3 的 USB 19/20 引脚）。
- 连线方式参考 `config.h` 中对引脚的定义。
- **当前为克隆初始状态，硬件配置与原 `bread-compact-wifi-s3cam` 一致**，后续在此目录上做自定义 DIY。

## 编译配置命令

**配置编译目标为 ESP32S3：**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig：**

```bash
idf.py menuconfig
```

**选择板子：**

```
Xiaozhi Assistant -> Board Type -> Bread Compact Wi-Fi + LCD + Camera AI Robot (面包板)
```

**编译烧入：**

```bash
idf.py build flash
```

或使用构建脚本（推荐）：

```bash
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```

## 与上游合并提示

作为独立命名的 board（`bread-compact-wifi-s3cam-airobot`），其目录与 `config.json` 的 `type`/`name` 均为唯一标识，不会与上游同名板冲突。合并上游代码时注意保留 `main/Kconfig.projbuild` 与 `main/CMakeLists.txt` 中本板的注册分支。
