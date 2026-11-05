# protocol/ — 协议编解码层

[← 代码层索引](README.md) · [device](device.md)

**职责**：把「一条指令」变成字节，或把字节流还原成「一条指令」。
**不负责**：怎么发出去（→ `device/` 的 `BspUartT`）、发不发（→ `app/`）。

这一层是**纯函数式**的：没有状态机以外的成员，不 include HAL、不 include bsp。

---

## 1. 文件清单

| 文件 | 内容 |
| --- | --- |
| `emmv5_protocol.hpp/.cpp` | Emm_V5 步进闭环的帧结构、组装、扫描器 |

---

## 2. Emm_V5 帧格式

总帧布局（见 `emmv5_protocol.cpp` 的文件头注释）：

```text
地址(1) + 0xAA(1) + 总长度(2, 大端) + 子命令(N) + 0x6B(1)
                                    └─ 功能码(1) + 参数(...) ┘
```

- 首字节是电机地址（总线多机时区分目标）
- `0xAA` 是帧头；末尾是校验字节，默认固定 `0x6B`，`Checksum::XOR` 时为前面全部字节的异或（子命令各自的校验字节同理，见 `emmv5_protocol.hpp` 的 `Checksum`）
- 长度字段是**大端**，包含子命令长度

---

## 3. 关键类型

```cpp
namespace emmv5
{
  enum class Direction : uint8_t;      // 正转 / 反转
  enum class PositionMode : uint8_t;   // 相对 / 绝对
  enum class SystemParam : uint8_t;    // 读写的系统参数项

  struct Frame
  {
    void append_u8(uint8_t);
    void append_u16(uint16_t);
    void append_u32(uint32_t);
    void seal();       // 补齐长度 + 尾字节，之后才能发
    bool valid() const;
  };

  struct InPositionScanner   { void reset(); bool feed(uint8_t addr, const uint8_t *data, size_t size); };
  struct PositionScanner     { void reset(); bool feed(uint8_t addr, const uint8_t *data, size_t size, int32_t &position); };
}
```

### 组装一帧

```cpp
emmv5::Frame f;
f.append_u8(static_cast<uint8_t>(emmv5::SystemParam::Enable));
f.append_u8(1);        // 使能
f.seal();              // 长度为 0 时不要发，seal() 之后用 is_valid() 检查

uart.send(f.data(), f.size());
```

### 解析回复（流式）

Emm_V5 的回复是「位置到达」之类的通知帧，字节流可能有粘连/截断，所以用扫描器而不是一次性解析：

```cpp
emmv5::PositionScanner scanner;

// 每收到一段串口数据就喂进去；凑满一帧返回 true，位置写进 position
if (scanner.feed(addr, buf, len, position))
{
  /* 更新该地址的当前位置 */
}
```

扫描器内部保存半帧状态，**同一路总线的扫描器只能在一个任务里用**。

---

## 4. 为什么要单独一层

`device/device_emmv5_group.hpp` 里每个方法都在做「填参数 → 组帧 → 发 → 等回复」，
如果把字节布局也写在设备类里，改一个字段就要在设备代码里翻半天。
分开以后：

- 改协议：只动 `protocol/`
- 改收发方式（485 换成 CAN）：只动 `device/`
- 加新指令：`protocol/` 加一个 `append_*` 序列，`device/` 加一个薄封装

---

## 5. 常见坑

| 现象 | 原因 |
| --- | --- |
| 电机不响应 | 忘了 `seal()`（长度字段还是 0），或地址与电机拨码不一致 |
| 偶发丢回复 | 用「读一次就解析」而不是扫描器，遇到半帧就丢 |
| 位置值跳变 | `PositionScanner` 的 `addr` 传错，把别的电机回复当成自己的 |
| 多任务抢 485 | 扫描器状态被两个任务共享；应在 `DeviceEmmv5GroupT` 上 `lock_bus()` |
