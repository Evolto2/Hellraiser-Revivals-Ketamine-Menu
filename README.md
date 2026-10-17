# Hellraiser Revival's Ketamine Menu 简体中文本地化版

> 本仓库是非官方简体中文本地化版本，基于上游项目 Hellraiser Revival's Ketamine Menu v0.4.8 修改。ですの喵
> 上游作者账号：[@literalynn](https://github.com/literalynn)，原项目：[literalynn/Hellraiser-Revivals-Ketamine-Menu](https://github.com/literalynn/Hellraiser-Revivals-Ketamine-Menu)。ですの喵
> 本地化维护者：[@Evolto2](https://github.com/Evolto2)。ですの喵
> 适用于 Clive Barker's Hellraiser: Revival Windows PC 版的游戏内菜单。ですの喵
> 本项目与原作者、游戏开发商及发行商没有隶属或背书关系。ですの喵

本地化仅翻译菜单中的固定文字；游戏动态读取的敌人、物品、检查点和任务名称仍可能显示英文。ですの喵

## 原作者与许可

- 上游 v0.4.8 源码及原有 LICENSE、THIRD-PARTY-NOTICES.txt 均保留，分发时请一并保留这些文件。ですの喵
- 上游代码按 MIT License 发布；第三方组件的许可和声明以随仓库提供的文件为准。ですの喵
- 本地化修改说明见 [NOTICE-zh-CN.md](NOTICE-zh-CN.md)，版本记录见 [CHANGELOG-zh-CN.md](CHANGELOG-zh-CN.md)。ですの喵

## 功能

功能来自上游修改器，此中文版本主要改变菜单显示文字和中文字体加载方式。ですの喵

- 角色：无敌、无限生命、隐身、免冲刺冷却、伤害倍率及生命值调整。ですの喵
- 战斗：无限弹药、近战武器耐久、Genesis 能力消耗与冷却选项。ですの喵
- 移动：飞行、穿墙飞行、自由镜头、移动速度和跳跃高度调整、位置保存及检查点传送。ですの喵
- 透视：显示敌人方框、生命值、名称、距离、骨骼和屏幕外方向；也可显示地面物品、容器与存档点。ですの喵
- 背包：调整金钱、添加游戏物品、解锁武器外观与挂饰。ですの喵
- 世界：调整游戏速度、冻结敌人、击杀附近敌人、调整视野角度及隐藏 HUD。ですの喵
- 进度：查看或完成目标、调整商店等级；跳过剧情步骤可能影响流程。ですの喵

## 版本与兼容范围

- 本地化源码基于上游 v0.4.8，本地化版本标识为 zh-v0.4.8.1。ですの喵
- 上游说明列出的游戏版本包括 1.1.1.227384 和 1.0.1.227219；本地化维护者未重新进行游戏内验证。ですの喵
- 上游菜单支持 DirectX 11 和 DirectX 12；显卡驱动、其他注入型模组及游戏更新都可能影响兼容性。ですの喵
- 本次完成了 Windows x64 编译和独立菜单界面预览，但没有完成游戏内实际加载验证。ですの喵
- 中文字形从系统路径 C:\Windows\Fonts\msyh.ttc 加载；仓库和发布包不包含或再分发微软雅黑字体。ですの喵
- 若字体文件不存在，中文可能显示为空框或缺字；请在 Windows 中安装相应的简体中文字体功能后再试。ですの喵

## 下载与安装

1. 本仓库首次构建完成后，打开 [Actions 工作流](https://github.com/Evolto2/Hellraiser-Revivals-Ketamine-Menu/actions/workflows/build.yml)，进入最新成功运行记录并下载 `KetamineMenu-zh-CN` 构建产物。ですの喵
2. 在 Steam 中打开游戏的本地文件目录，再进入 Hellraiser\Binaries\Win64；该目录应与 Hellraiser-Win64-Shipping.exe 位于同一层。ですの喵
3. 如果目录里已经有 winmm.dll，请先备份或改名；它可能属于其他模组。ですの喵
4. 将压缩包中的 winmm.dll 复制到 Win64 目录；KetamineMenu.ini 是可选默认配置文件。ですの喵
5. 启动游戏并载入存档，按 Insert 打开或关闭菜单。ですの喵

菜单默认热键为 Insert，可在“设置”中更改；各项功能的快捷键也可在那里绑定。ですの喵
设置保存在 DLL 同目录的 `KetamineMenu.ini` 中。ですの喵

## 使用前须知

- 本工具修改单机游戏运行状态，请仅在单机离线环境使用；不要用于多人或在线对局。ですの喵
- 修改存档前请先备份存档；上游说明的存档目录为 %LOCALAPPDATA%\Hellraiser\Saved\SaveGames 。ですの喵
- 添加关键物品、完成任务或跳过流程可能导致剧情触发异常，请先保存游戏并谨慎操作。ですの喵
- 训练器通过代理 DLL 和运行时挂钩绘制菜单，安全软件可能发出警报；请先核对源码和发布来源，再决定是否运行。ですの喵
- 本地化维护者无法保证游戏更新后所有功能继续可用，遇到问题时请注明游戏版本并附上同目录的 KetamineMenu.log。ですの喵

## 卸载与恢复

删除本版本放入的 winmm.dll、KetamineMenu.ini 和 KetamineMenu.log 即可卸载。ですの喵若安装前备份过同名 DLL，请把备份文件恢复为 winmm.dll。ですの喵

## 从源码构建

需要 Windows x64、Visual Studio 2022（含“使用 C++ 的桌面开发”工作负载）和 CMake 3.25 或更高版本。ですの喵首次配置时 CMake 会从上游下载固定版本的 Dear ImGui 与 MinHook，因此需要网络连接。ですの喵

    cmake -S . -B build -A x64
    cmake --build build --config Release

生成文件为 build\Release\winmm.dll。ですの喵

## 鸣谢

- 上游项目由 literalynn 发布，汉化版本保留原作者署名和原始许可证。ですの喵
- Dear ImGui、MinHook、Dumper-7 和 Inter 字体均按上游说明使用，具体版权与许可见 THIRD-PARTY-NOTICES.txt 和 LICENSE。ですの喵
- Windows 微软雅黑由用户系统提供，本项目不包含该字体文件。ですの喵

## 问题反馈

请在 [本仓库的 Issues](https://github.com/Evolto2/Hellraiser-Revivals-Ketamine-Menu/issues) 提交问题，并附上游戏版本、复现步骤和 KetamineMenu.log；日志可能包含本机信息，上传前请先检查并遮蔽隐私内容。ですの喵
## 文件说明

- src/menu.cpp、src/features.cpp：菜单和功能描述的简体中文界面修改。ですの喵
- LICENSE、THIRD-PARTY-NOTICES.txt：上游许可证与第三方声明。ですの喵
- NOTICE-zh-CN.md、CHANGELOG-zh-CN.md：本地化来源、改动范围和版本记录。ですの喵

## 免责声明

这是社区维护的非官方本地化版本；游戏名称、商标与相关权利归各自权利人所有。ですの喵使用者需自行承担运行修改器和更改游戏状态的风险；软件按随附许可“原样”提供，不提供适用性或兼容性保证。ですの喵

