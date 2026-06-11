# 添加电脑体验重设计 — 局域网发现(net_scan) + 唤醒验证(wake verify)

> 状态:阶段 1/2（固件 net_scan + wake verify）、4（后端接口 + OUI 富化）、5（小程序 discover/verify 页）已实现；
> 阶段 3 的 wol/cmd 迁移到 task 模型与 SSE 下线未做（现有同步回执仍可用，非阻塞项）；待真机端到端联调（阶段 6）
> 关联文档:`ARCHITECTURE_v3.md`(task 轮询模式)、`../PROTOCOL.md`(实施时同步更新)
> 涉及仓库:本仓(固件)、`../esp_auth`(后端)、`../wolapp`(小程序)

## 1. 背景与目标

当前添加目标电脑需要用户自己查网卡 MAC 并手敲 12 位十六进制,对终端用户是
劝退级体验。本设计用两个能力替代手动输入:

1. **net_scan(局域网发现)**:控制器扫描所在网段,小程序展示"主机名 + 厂商"
   列表,用户点选添加——全程不接触"MAC"概念。
2. **wake verify(唤醒验证)**:添加完成后立即验证"真的能唤醒",把"配置完成"
   变成"验证可用",失败时就地给排查指引。

手动输 MAC 保留为"高级"兜底入口。PC 端助手工具(读 MAC 生成二维码 + 一键修复
WoL 系统设置)为二期,见附录。

## 2. 总体流程

```
用户          小程序             后端(esp_auth)        EMQX        设备(固件)
 │ 添加电脑     │                  │                   │            │
 │────────────▶│ POST /devices/{id}/net_scan          │            │
 │             │─────────────────▶│ 建 task            │            │
 │             │◀─{task_id}───────│─publish net_scan──▶│──cmd──────▶│
 │             │                  │                   │            │ ARP 扫描
 │             │ GET tasks/{tid}  │◀──event(分批结果)──│◀─event─────│ +NBNS/mDNS
 │             │──1~2s 轮询──────▶│ 聚合 + OUI 富化     │            │
 │  列表点选    │◀─hosts(增量)─────│ + 打分排序          │            │
 │────────────▶│ POST targets {mac,name,last_ip}      │            │
 │ 测试唤醒     │ POST wol {mac,verify:true}           │            │
 │(手动关机后)  │─────────────────▶│─publish wol+verify─▶│──cmd──────▶│ 发魔法包
 │             │ GET tasks/{tid}  │                   │            │ ARP 探测
 │             │──轮询───────────▶│◀──event(wake_result)◀─event────│ ≤60s
 │  成功/排查   │◀─结果────────────│                   │            │
```

## 3. MQTT 协议增量

### 3.1 net_scan(下行)

```json
{"cmd":"net_scan"}
```

约束:扫描中 / OTA 中收到重复指令回 `error:net_scan_busy`;未连 WiFi 不可能
收到指令,无需处理。

### 3.2 net_scan 事件(上行,分批)

```json
{"event":"net_scan_start","subnet":"192.168.1.0/24","hosts":254}

{"event":"net_scan_result","seq":0,"last":false,"hosts":[
  {"ip":"192.168.1.5","mac":"AABBCCDDEEFF","name":"DESKTOP-A7K2","src":"nbns"},
  {"ip":"192.168.1.13","mac":"DE12AB34CD56","rand":true}
]}
```

- 每批 ≤5 条(单条约 70~90B,批总量控制在 MQTT buffer 1024 内)
- `src`:`nbns` / `mdns`,无名字时省略 `name` 与 `src`
- `rand:true`:MAC 首字节第二位为 2/6/A/E(本地管理位),大概率是随机 MAC 手机
- 固件已排除:自身、网关 IP
- 末批 `last:true`;零结果时发一批空 `hosts` + `last:true`
- 公共字段(uptime/heap/ip)随事件附带,与现有事件一致

### 3.3 wol 指令扩展(唤醒验证)

```json
{"cmd":"wol","mac":"AABBCCDDEEFF","verify":true,"ip":"192.168.1.5"}
```

- `verify` 可选,缺省 false 时行为与现状完全一致(向后兼容)
- `ip` 可选提示(后端填上次已知 IP,加速探测)
- 流程:立即发魔法包 + 现有 `wol` 事件 → 进入探测:有 `ip` 则每秒 ARP 该 IP
  并核对 MAC;未命中或无 `ip` 则周期全网段 ARP 扫描找该 MAC → 超时
  `WAKE_VERIFY_TIMEOUT_MS`(默认 60s,`config.h`)

```json
{"event":"wake_result","ok":true,"mac":"AABBCCDDEEFF","ip":"192.168.1.5","elapsed_ms":23400}
{"event":"wake_result","ok":false,"mac":"AABBCCDDEEFF","reason":"timeout"}
```

> 按 MAC 探测(而非只信存储 IP)是有意设计:DHCP 可能换 IP,MAC 才是稳定标识。

## 4. 固件设计(wol_esp)

### 4.1 扫描状态机

不能阻塞 `loop()`(MQTT keepalive),做成状态机,每次 loop 推进一步:

```
IDLE → ARP_WINDOW(发 8 个 ARP 请求)→ HARVEST(等 ~200ms 收割应答)
     → [循环直到扫完 /24] → NAME_QUERY(对命中 IP 逐个 NBNS/mDNS)
     → REPORT(分批发事件)→ IDLE
```

- **lwIP ARP 表默认仅 10 槽**,故窗口取 8:`etharp_request()` 发请求,
  `etharp_find_addr()` 收割,结果立即转存自有数组,再清下一窗
- 全程约 254/8 × 250ms ≈ **8~10s**,加名字查询总计 ≤15s
- 子网掩码短于 /24 时只扫自身所在 /24;结果上限 **64 台**(静态数组,
  单条 28B,共 <2KB)

### 4.2 名字查询

- **NBNS**(Windows):向命中 IP 的 UDP 137 发 NBSTAT 通配查询(`*`),
  解析应答第一个名字项;`WiFiUDP` 即可,单包问答,超时 300ms
- **mDNS 反查**(macOS/Linux/部分 Win10+):仅对 NBNS 无应答的 IP,向 5353
  发 `<reversed-ip>.in-addr.arpa` PTR 单播查询;ESP mDNS 库不支持反查,
  手搓 DNS 报文(约 50 行)
- 名字截断存 15 字符 + NUL

### 4.3 其它

- 串口新增 `netscan` 命令(正常运行模式),不依赖 MQTT 即可单机验证
- 与 OTA 互斥;扫描中再收 `net_scan` 回 busy
- wake verify 复用同一套 ARP 探测代码,独立轻量状态机(每秒一探)

## 5. 后端设计(esp_auth)

### 5.1 task 轮询基建(前置,见 ARCHITECTURE_v3.md §7)

- 新增 `internal/task`:内存 map,task = {id, device, kind, state, payload,
  created_at},终态后保留 5 分钟供重复拉取;无需落库
- `POST /devices/{id}/wol`、`/cmd` 改为返回 `{task_id}`
- 新增 `GET /devices/{id}/tasks/{task_id}` → `pending/ok/error/timeout` + 载荷
- MQTT 消费者按"设备 + 事件类型 + 时间窗"把 event 归集到 task
- `GET /devices/{id}/events`(SSE)从小程序 API 下线;管理后台若复用则保留
  仅限 admin 路由

### 5.2 net_scan 接口

- `POST /devices/{id}/net_scan` → 校验归属 → 限流(同设备 30s 一次)→
  发 cmd → 返回 `{task_id}`
- 消费者把 `net_scan_result` 分批增量写入 task payload,`last:true` 置终态;
  小程序轮询时即可见"设备一台台冒出来"
- **富化与排序**(返回前做):
  - OUI 厂商:`internal/oui`,内嵌精简 IEEE OUI 表(常见厂商数千条,
    `//go:embed` CSV)
  - 打分:有名字 +3;PC 厂商 OUI(Intel/Realtek/ASUS/MSI/Giga-Byte/Apple…)+2;
    `rand:true` −2;IoT 厂商(Espressif/Tuya/小米…)−2
  - 返回分两组:`likely_pc` / `others`

### 5.3 wake verify

- `POST /devices/{id}/wol` body 增加 `verify:true`;后端查 `wake_targets.last_ip`
  填入下发指令的 `ip` 提示
- `wake_result` 事件归集到 task;`ok:true` 时回写 `last_ip`
- DDL 增量:`ALTER TABLE wake_targets ADD COLUMN last_ip TEXT;`
- 添加目标接口(`POST /devices/{id}/targets`)接受可选 `last_ip`(来自扫描结果)

## 6. 小程序流程(wolapp)

**添加电脑**(替换手动输入页):

1. 引导页:"确保电脑已开机,建议用网线连接路由器" → 开始扫描
2. 扫描页:进度动画,1.5s 轮询 task,结果增量渲染;
   `likely_pc` 置顶("💻 DESKTOP-A7K2 · Intel 网卡 · 192.168.1.5"),
   `others` 折叠
3. 点选 → 命名 → 保存(`POST targets`,带 `last_ip`)
4. 完成页给"测试唤醒"入口(可跳过)
5. 空结果:提示检查电脑是否开机 / 路由器 AP 隔离,给手动添加入口

**测试唤醒**:

1. 引导:"请将电脑关机(不要拔电源),然后点开始"
2. 下发 `wol + verify:true`,轮询 task,展示等待动画(最长 60s)
3. 成功:"电脑已唤醒 ✓";失败:排查指引(BIOS WoL / 网卡魔术封包 /
   Windows 快速启动 / 是否插网线)——二期在此挂 PC 助手下载

**手动添加 MAC**:收进添加页底部"高级"入口。

## 7. 局限与兜底

| 局限 | 应对 |
|------|------|
| 电脑关机时扫不到 | 配置场景天然规避(用户在电脑旁),引导文案提示 |
| 路由器 AP 隔离(访客网常见)→ 零结果 | 空结果页明确提示 + 手动通道 |
| 电脑走 WiFi 时扫到无线 MAC,无线 WoL 基本不可用 | 文案建议插网线;唤醒验证当场暴露问题并引导排查 |
| Android 设备常不回名字查询 | 仅显示厂商 + IP,归入折叠组 |
| 同名/多网卡机器 | 列表展示 IP 辅助区分;选错可删重加 |

## 8. 阶段计划

| 阶段 | 仓库 | 内容 | 验收方式 |
|------|------|------|---------|
| **1** ✅ | wol_esp | net_scan 状态机 + NBNS/mDNS + 分批上报;串口 `netscan` | 串口单机跑通;MQTTX 下发指令看分批事件 |
| **2** ✅ | wol_esp | wol `verify` 扩展 + wake_result（按 MAC 探测 ARP 表）| 待真机实测；编译通过 |
| **3** 部分 | esp_auth | task 轮询基建（`internal/task`）已落地；wol/cmd 仍保留同步回执（未迁移）、SSE 未下线 | go test 覆盖 task 生命周期 |
| **4** ✅ | esp_auth | net_scan 接口 + OUI 富化打分（`internal/oui`）;wake verify 接口 + last_ip 回写 | go test 全链路（httptest 注入 event → 轮询拿富化列表/唤醒结果） |
| **5** ✅ | wolapp | 添加电脑流程页（discover）+ 测试唤醒页（verify）| 待真机小程序走通（HBuilderX 编译） |
| **6** | 三仓 | 联调;更新 PROTOCOL.md / CLAUDE.md / README | 端到端演示:扫描→点选→命名→验证唤醒 |

阶段 1/2 可先行(串口即可验证,不依赖后端);3/4 与 1/2 可并行;5 依赖 3/4。

## 附录:二期 — PC 端助手

绿色单 exe,解决"填对了也唤不醒"的系统设置问题:

1. 读有线网卡 MAC + 主机名,生成二维码,小程序"扫码添加"(比扫描更精准)
2. 体检 + 一键修复:网卡驱动"魔术封包唤醒"、关闭 Windows 快速启动
   (需管理员权限);BIOS 项给图文指引
3. 入口挂在"测试唤醒失败"的排查页,不强制所有用户安装
