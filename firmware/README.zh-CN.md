# Espressif 固件

使用 ESP-IDF 6.1 与固定版本的 MicroPixel WAMR fork。在仓库根目录执行命令，
先按[构建指南](../docs/development/flashing.zh-CN.md)配置工具链。

```sh
bash tools/p4.sh build-host
bash tools/s31.sh build-host
bash tools/s3.sh build-host <box3|szpi|cores3>
```

构建与烧录分开执行。例如 `bash tools/p4.sh flash-host <port>` 烧录已构建的 Host，保留 App Store。
连接前关闭其他串口工具。

- [构建、配置与烧录](../docs/development/flashing.zh-CN.md)
- [源码导航](espressif/main/README.zh-CN.md)
- [参与贡献](../CONTRIBUTING.zh-CN.md)
- [English](README.md)
