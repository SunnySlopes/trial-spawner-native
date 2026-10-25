# 试炼刷怪笼引擎

独立计算密室拼装、笼子坐标和范围内最佳统计中心。搜索为水平圆/方统计，展示高度不是三维最优挂机高度。 正式运行不需要Java或Minecraft。支持1.21至已适配26.x生成代际；26.3独立气候判定，房间布局数据按已核对代际使用。

## 运行与构建

在本目录执行；`run.bat`会读取`finder.properties`，命令行参数覆盖配置。

```powershell
.\build.ps1
.\build\trial-spawner.exe --seed 12345 --version 26.3 --radius 100000 --min-structures 2 --min-spawners 20
```

相同参数的未完成任务可自动断点恢复，成功导出后清理对应断点；`--no-checkpoint`可禁用。原生CLI默认生成CSV/TXT/JSON，桌面入口支持按需保存。

## 目录与说明

`src/`为算法，`data/`为编译常量，`build/`为本地构建。普通构建需要CMake、Ninja和C++20编译器，不依赖`tests/`、`tools/`或Python。

`tests/`、`tools/`和`docs/记录/`仅本地保留，不提交。拥有完整本地测试、开发工具及Python时可用`build.ps1 -Test`运行回归；直接使用CMake构建默认关闭测试。

[使用说明](docs/说明/使用说明.md) · [第三方来源](THIRD_PARTY.md)

## 许可证

本仓库自有代码采用GNU GPL第3版（`GPL-3.0-only`），完整条款见[LICENSE](LICENSE)。第三方代码及参考来源保留各自的版权与许可，见[THIRD_PARTY.md](THIRD_PARTY.md)。
