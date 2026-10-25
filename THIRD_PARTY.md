# 第三方来源与许可

本仓库自有代码采用[GPL-3.0-only](LICENSE)。以下外部代码与参考材料保留原作者版权及原许可证；项目许可证不改写这些声明。

## 外部开源项目

| 来源 | 使用范围 | 版本与许可 |
|---|---|---|
| [Cubiomes](https://github.com/Cubitect/cubiomes)，Cubitect | `third_party/cubiomes/`，提供生物群系、噪声和RNG能力，用于结构环境过滤；26.3气候数值由本仓库实现 | 固定提交`e61f90580cbdd883214a8054670dacae655e59c0`，另见`third_party/cubiomes-revision.txt`；[MIT原文](third_party/cubiomes/LICENSE) |
| [JSON for Modern C++](https://github.com/nlohmann/json)，Niels Lohmann及贡献者 | `third_party/json.hpp`，用于JSON Lines通信及结果序列化 | `v3.12.0`；[MIT原文](third_party/json-LICENSE.MIT)，头文件内其他原始声明保留 |

Cubiomes不提供本项目的试炼密室内部拼接或刷怪笼生成完整实现；不能将生物群系过滤的结果视为整座密室的原版验证。

## 游戏行为与数据参照

[Minecraft Java Edition](https://www.minecraft.net/)由Mojang Studios/Microsoft开发，是结构放置、密室拼接、刷怪笼生成及版本差异的行为参照。生成常量包含几何、连接器、池和刷怪笼信息；数据生成工具与回归数据仅在本地保留，不随仓库提交。

仓库不分发Minecraft JAR、官方数据包、NBT模板或反编译缓存。游戏及官方资源的权利归原权利人，不能以本仓库GPL声明替代其授权。正式运行不启动Java或Minecraft。

## 分发时保留的文件

分发本项目时保留[LICENSE](LICENSE)、本说明、相关MIT许可证及源码中的版权声明。分发二进制时还需按GPL第3版条款提供相应源代码。
