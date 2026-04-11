# ReShade Helldivers2 Ingame IME

《绝地潜兵2》中文输入支持

## 警告

《Helldivers 2》是一款多人游戏，设有反作弊机制。此实现通过 ReShade 加载外部 DLL，可能会导致反作弊检测。原作者不对由此导致的任何后果负责。

如果你不想承担此风险，建议使用 [GRW-CNChat](https://github.com/GameXueRen/GRW-CNChat/)。

## 使用方法
1. 安装 [ReShade](https://reshade.me)
   - 安装时需开启 Add-On 功能
   - 如果有使用基于 ReShade 的隐藏披风/去雾 Mod，可以跳过这一步，这类 Mod 通常使用 ReShade Add-On 实现
2. 从 Releases 下载 `.addon64` 文件，放到游戏目录下的 `bin` 文件夹
3. 启动游戏
   - 如果正常加载，ReShade 菜单中插件选项卡会显示此插件
   - `Enter` 启用或关闭输入法
   - `Esc` 关闭输入法
   - 输入内容会实时反映到游戏内
