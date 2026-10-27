# 简体中文语言包

简体中文沿用现有 Vita 语言插件机制：`cn.file`、`cn.cfg` 和 `language_cn/`。安装完整插件后，设置菜单会显示 `ZH-CN`，选择后重启生效。只有中文插件且没有保存的语言偏好时，自动选中中文。

## 准备与安装

1. 按[上游说明](https://github.com/stoicpingu/umineko-vita#setup-instructions-for-end-users)准备 Vita 底座和 Hotfix v2，安装包含本改动的 VPK。原版 v1.4 VPK 尚不识别中文。
2. 在本地取得固定版本的日不落脚本源码：

   ```sh
   git clone https://github.com/Umineko-Project-CN/umineko-scripting.git build/cn-source
   git -C build/cn-source checkout --detach 162a77eb764f181236b8044d1f75a93458ea4937
   ```

   工具会核对提交并拒绝已修改的源码。脚本拼接、中文替换和 ONS2 编码均由 Python 在本地完成，无需 PHP 或 Actions 产物。
3. 自行准备[日不落 PC 版](https://snsteam.club/download/port/)并应用官网资源更新，保留其中的 `language_cn/`。使用 Python 3.11+、Pillow 和 fontTools 运行：

   ```sh
   python tools/prepare_cn_assets.py /path/to/updated-pc-game build/cn-source build/cn-addon
   ```

4. 将 `build/cn-addon/umineko/` 的内容合并到 `ux0:data/umineko/`，在设置中选择 `ZH-CN` 后重启。

生成包与现有 PT-BR / ES 语言包采用同样的松散文件布局：

```text
umineko/
├── cn.file
├── cn.cfg
└── language_cn/
    ├── fonts_cn/
    ├── graphics/
    ├── native-image-sizes.tsv
    └── render_scale.txt
```

工具保留原 `cn.cfg` 的 `lang-dir=language_cn` 和 `font-dir=fonts_cn`，原样复制十个字体槽。中文 PNG 按原宽高的一半取整，保留透明度；极小标记保持原尺寸。尺寸表记录 PC 原尺寸，`render_scale.txt` 为 `0.5000000`。背景、立绘、语音和视频继续使用 Vita 底座。

中文进度、已读记录和缓存使用 `ux0:data/umineko/save/cn/`；首次启动按游戏原有流程生成字体缓存。语言顺序为 `English / JP / PT-BR / Español / ZH-CN`，仅列出已安装的语言，保留其他语言的存档目录。

## 来源与许可

- 脚本：日不落汉化组的 [Umineko-Project-CN/umineko-scripting](https://github.com/Umineko-Project-CN/umineko-scripting/tree/162a77eb764f181236b8044d1f75a93458ea4937)，固定提交 `162a77eb764f181236b8044d1f75a93458ea4937`，包含简体中文 EP1–EP8 和 omake。Python 生成器复现原 `update-manager.php` 的处理，不修改译文。
- 翻译：日不落汉化组；其[项目介绍](https://snsteam.club/port/)注明基于 MMR 六轩岛调查队授权的翻译底本。原脚本中的制作人员名单保留。
- 脚本源码的 [BSD-3-Clause 许可证](https://github.com/Umineko-Project-CN/umineko-scripting/blob/162a77eb764f181236b8044d1f75a93458ea4937/LICENSE.txt)允许修改和再分发，要求保留版权、许可条件及免责声明，且不得暗示原作者背书。生成包随附原始许可文本 `SCRIPT-LICENSE.txt`、来源及 `SHA256SUMS`。
- 该脚本许可不涵盖字体、图片或其他游戏素材的权利。仓库和 VPK 不包含这些资源，准备工具只读取用户自行取得的 PC 安装。完整游戏仍适用 [Umineko Project 版权声明](https://www.umineko-project.org/en/copyright-message/)。

## 验证范围

语言识别、菜单、存档和缓存使用现有路径机制。日不落脚本的 CJK 排版声明不再被误认为日文菜单：菜单行布局根据歌曲字幕选项是否存在判断，适配仍在内存中进行，保持脚本长度、换行和标签位置。

主机测试编译生产代码中的相关入口，检查 16 种插件组合、缺失或无效插件、保存路径和语言偏好恢复。配合实际 EN / JP / PT-BR / ES / CN 脚本检查菜单及标签位置，并通过 AddressSanitizer / UndefinedBehaviorSanitizer。未安装中文时，原语言菜单除偏好恢复的初始化区外，与 v1.4 逐字节比较。

生成器保留 PHP 的 ASCII 空白处理、文件配对与自然排序、首次替换及防重复替换，并跳过 `._*` 文件。发布日期固定为来源提交时间，使相同环境下的重复生成结果一致。以下检查均在本地执行：

```sh
python tools/tests/test_native_languages.py --source build/cn-source build/cn-addon/umineko/cn.file /path/to/en.file /path/to/jp.file /path/to/pt.file /path/to/es.file
```

`--source` 同时核对八章入口与结尾、重复生成、ONS2 编解码往返和替换边界情况。若保留了原 PHP 生成的 `cn.txt` 与 `cn.file`，可加 `--reference /path/to/php-output`，对照原始脚本及解码内容，仅允许发布日期不同。压缩流可能随 zlib 版本变化。

原生完整引擎构建已在 Vita 上确认中文启动、文字、声音和输入正常；尚未完成全部剧情或视频播放测试。
