# 器件手册（第三方资料）

本目录放的是驱动开发时对照用的厂商说明书和板卡资料，**版权归各厂商所有，不属于本仓库的 MIT 许可**，放在这里只为方便查阅。
请勿把它们当作本库的一部分再分发；以厂商官方渠道发布的最新版为准。厂商或权利人要求删除时，直接从仓库移除。

| 文件 | 厂商 / 来源 | 对应的库代码 |
| --- | --- | --- |
| `DJI_Motor/RoboMaster M3508直流无刷减速电机使用说明V1.0（中英日）.pdf` | DJI RoboMaster | `device/motor/dji_motor.hpp`（M3508） |
| `DJI_Motor/RoboMaster C620无刷电机调速器使用说明（中英日）V1.01.pdf` | DJI RoboMaster | 同上（C620 电调协议） |
| `DJI_Motor/RM M2006 P36直流无刷减速电机使用说明.pdf` | DJI RoboMaster | `dji_motor.hpp`（M2006） |
| `DJI_Motor/RM C610无刷电机调速器使用说明 发布版.pdf` | DJI RoboMaster | 同上（C610 电调协议） |
| `DJI_Motor/RoboMaster GM6020直流无刷电机使用说明20231013.pdf` | DJI RoboMaster | `dji_motor.hpp`（GM6020，电压 / 电流模式） |
| `DM-J4310-2EC V1.2减速电机说明书V1.2定稿.pdf` | 达妙科技 | `device/motor/dm_motor.hpp`（J4310） |
| `J4340P/`（中 / 英，V1.2 与 V1.1 硬件的 V1.4） | 达妙科技 | `dm_motor.hpp`（J4340P / J4340P_V11） |
| `J8009P/`（中 / 英，V1.3 与 V1.1 硬件的 V1.0） | 达妙科技 | `dm_motor.hpp`（J8009P） |
| `达妙科技DM-MC-Board02电机开发板使用说明书V1.1.pdf`、`CtrBoard-H7管脚标注图.pdf`、`固件更新日志-3.1.docx` | 达妙科技 | 回归宿主用的 DM-MC02 开发板（STM32H723） |

## 官方渠道

- DJI RoboMaster：[robomaster.com](https://www.robomaster.com/) 各组件产品页的下载栏（例如 [组件详情页](https://www.robomaster.com/en-US/products/components/detail/2696)）；
  C620 说明书在官方 CDN：[RM C620 无刷电机调速器使用说明 V1.01](https://cdn-hz.robomaster.com/robomasters/public/document/RM%20C620%E6%97%A0%E5%88%B7%E7%94%B5%E6%9C%BA%E8%B0%83%E9%80%9F%E5%99%A8%E4%BD%BF%E7%94%A8%E8%AF%B4%E6%98%8E%EF%BC%88%E4%B8%AD%E8%8B%B1%E6%97%A5%EF%BC%89V1.01.pdf)。
  调参 / 固件升级用 RoboMaster Assistant。
- 达妙科技：向厂商的官方渠道（店铺、技术支持）索取最新版说明书与上位机；2026-11-07 检索时没有找到可以确认的官网下载链接。

## 本仓库自己的许可证例外

代码部分的许可证例外（VQF、Matrix）见根目录 [README 的“许可证”一节](../../README.md#许可证)。
