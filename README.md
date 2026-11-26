# 小智Pro个人版

## 简介 ℹ️

- **项目名称**：小智Pro
- **项目简介**：利用 MCP，为小智 AI 接入三头六臂，打造您身边的全能 AI 助手。

基于 [小智AI](https://github.com/78/xiaozhi-esp32) 开源固件扩展设备端能力：固件直接下载烧录即用，源码同步开源，支持定制化开发。
- 固件下载：[Releases](https://github.com/hougeai/xiaozhiPro/releases)
- 源码仓库：[dev 分支](https://github.com/hougeai/xiaozhiPro/tree/dev)
- 官方网站（控制台 / 文档）：https://xzp.qlcloud.top/

更多精彩内容，欢迎关注公众号：猴哥的AI知识库

![猴哥的AI知识库](gzh.png)

## 产品功能

给小智 AI 接入 MCP，有两种方式：

- 通过官方暴露的 MCP 接入点；
- 设备端代码新增 MCP tool；

`小智Pro` 为降低大家的接入成本，同时支持以上两种接入方式：

### 接入点MCP

实现了两个功能界面：

- MCP广场
- MCP绑定管理

为用户提供 MCP 服务管理、设备绑定一条龙服务。

#### MCP广场

- 公共MCP服务：汇集全网热门MCP服务，支持平台用户公开自己的MCP服务
- 我的MCP服务：创建、管理和部署您的 MCP 服务

#### MCP绑定

- 将 MCP 服务绑定到您的设备
- 管理设备的MCP服务
- 连接状态切换/监控
- 接入点配置

### 设备端MCP

#### 闹钟提醒 ⏰

- 倒计时提醒 / 每日循环提醒
- 重新加载闹钟，清空闹钟

#### 音乐播放 🎵

- 搜索/播放音乐
- 播放歌单 / 上一首 / 下一首 / 重置歌单

#### 知识库RAG 📚

- 接入自研知识库
- 接入外部知识库（目前仅支持 Dify）
- 调用知识库，作为AI回答的上下文

#### 长期记忆 🧠

- 接入长期记忆
- 调用用户专属的记忆库，作为AI回答的上下文

#### 视觉理解 👁️

- 无需设备端摄像头，通过浏览器摄像头，为小智接入'实时眼睛'
- 接入 Qwen-VL / GLM 等视觉模型，为小智提供视觉理解能力

#### 指挥本地 Agent 🦾

- 通过 Agent Connector 接入本地 Agent，设备端语音指挥它们干活
- 当前已支持 12 种 Agent：OpenClaw、Hermes、Codex、Claude Code、Qwen Code、OpenCode、Qoder、MiniMax Code、Kimi Code、CodeBuddy、DeepSeek Harness、Pi


#### 智能家居HA控制
- 平台打通了和 `Home Assistant` 的通信
- 支持用户接入自己的 `Home Assistant`，并在设备端通过语音指挥全屋智能设备

#### 远程控制API
- 平台提供一套完整的远程控制设备 API 接口，支持设备唤醒、重启等功能

## 产品特性

- **开放扩展性**：通过MCP（模块化连接协议）架构，为AI助手提供无限扩展能力
- **本地化部署**：支持在设备端直接运行MCP服务，保护隐私并减少延迟
- **丰富功能**：涵盖闹钟提醒、音乐播放、知识库、长期记忆、视觉理解、Agent 接入等多种实用功能
- **社区生态**：通过MCP广场实现服务共享，构建丰富的功能生态
- **灵活配置**：支持多种接入点配置，适应不同使用场景
- **计划升级**：未来将支持更多高级功能

完整版本更新日志，请移步官方文档：https://xzp.qlcloud.top/docs/cn/user/release.html

## 安装使用

三步走：

1. 下载自己开发板对应的固件包，并通过烧录工具烧录进设备（后缀为.bin的文件）
   - 小智Pro最新固件下载：https://github.com/hougeai/xiaozhiPro/releases
   - 烧录参考：[Flash工具/Web端烧录固件（无IDF开发环境）](https://ccnphfhqs21z.feishu.cn/wiki/Zpz4wXBtdimBrLk25WdcXzxcnNS)
   - 如果没有找到开发板对应的固件包，可下载 [dev 分支](https://github.com/hougeai/xiaozhiPro/tree/dev)源码，自行编译烧录
2. 确保已在【小智AI】控制台绑定设备：https://xiaozhi.me/ 
3. 注册登录【小智Pro】控制台绑定设备：https://xzp.qlcloud.top/


# 小智Pro企业版

私有化定制：小智Pro全套解决方案☁️
- 设备端核心功能
- 用户前端界面
- 管理后台
- 服务端搭建

使用中有任何问题，欢迎咨询，+V，备注小智 📞

![小智Pro企业版定制](qrcode.webp)