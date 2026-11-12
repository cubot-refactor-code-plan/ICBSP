# 器件手册（第三方资料）

驱动开发时对照的厂商说明书和板卡资料，**版权归各厂商所有，不属于本仓库的 MIT 许可**。

**手册本身不随仓库分发**：2026-11-12 起已从仓库及其全部历史中移除（手册占了仓库体积的九成以上，拖慢克隆）。
本目录只保留这份清单。需要原文件时按下面的“官方渠道”获取，以厂商发布的最新版为准。
`.gitignore` 已忽略本目录下除本文件外的所有文件：下载后放在这里对照即可，不会被误提交。

## 清单

驱动按下列版本编写，代码注释里引用的章节、表格编号以这些版本为准：

| 手册 | 厂商 | 对应的库代码 |
| --- | --- | --- |
| RoboMaster M3508 直流无刷减速电机使用说明 V1.0（中英日） | DJI RoboMaster | `device/motor/dji_motor.hpp`（M3508） |
| RoboMaster C620 无刷电机调速器使用说明（中英日）V1.01 | DJI RoboMaster | 同上（C620 电调协议） |
| RM M2006 P36 直流无刷减速电机使用说明 | DJI RoboMaster | `dji_motor.hpp`（M2006） |
| RM C610 无刷电机调速器使用说明 发布版 | DJI RoboMaster | 同上（C610 电调协议） |
| RoboMaster GM6020 直流无刷电机使用说明 20231013 | DJI RoboMaster | `dji_motor.hpp`（GM6020，电压 / 电流模式） |
| DM-J4310-2EC V1.2 减速电机说明书 V1.2 定稿 | 达妙科技 | `device/motor/dm_motor.hpp`（J4310） |
| DM-J4340P-2EC 减速电机使用说明书 V1.2（中 / 英，2026-09-14） | 达妙科技 | `dm_motor.hpp`（J4340P） |
| DM-J4340P-2EC V1.1 减速电机使用说明书 V1.4（中 / 英，2026-09-14） | 达妙科技 | `dm_motor.hpp`（J4340P_V11） |
| DM-J8009P-2EC 减速电机说明书 V1.3（中 / 英，2026-09-10） | 达妙科技 | `dm_motor.hpp`（J8009P） |
| DM-J8009P-2EC V1.1 减速电机说明书 V1.0（中 / 英，2026-09-10） | 达妙科技 | `dm_motor.hpp`（J8009P，V1.1 硬件） |
| 达妙科技 DM-MC-Board02 电机开发板使用说明书 V1.1、CtrBoard-H7 管脚标注图、固件更新日志 3.1 | 达妙科技 | 回归宿主用的 DM-MC02 开发板（STM32H723） |

## 官方渠道

- **DJI RoboMaster**：按上表的手册名称搜索即可找到（RoboMaster 官网各组件产品页的下载栏）。调参 / 固件升级用 RoboMaster Assistant。
- **达妙科技**：从厂商的代码仓库获取说明书与上位机：
  - [Gitee](https://gitee.com/kit-miao/projects)
  - [GitHub](https://github.com/dmBots?tab=repositories)

## 本仓库自己的许可证例外

代码部分的许可证例外（VQF、Matrix）见根目录 [README 的“许可证”一节](../../README.md#许可证)。
