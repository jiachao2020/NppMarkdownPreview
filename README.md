# NppMarkdownPreview

Notepad++ 的**原生** Markdown 预览插件（C++ / Win32 / WebView2，无 .NET 依赖），支持 **ARM64** 与 **x64**。

在停靠面板中实时渲染当前 Markdown 缓冲区：

- WebView2（Edge 内核）渲染 + marked.js 解析
- 切换文件、保存、输入时自动刷新（约 0.4 秒防抖）
- 深浅色主题自动跟随系统（`prefers-color-scheme`）

## 云端编译（GitHub Actions，免费，无需本机安装任何工具链）

1. 在 GitHub 网页新建一个 **Public** 仓库（例如 `NppMarkdownPreview`），**不要**勾选 README / .gitignore / license。
2. 在空仓库页面点击 **"uploading an existing file"**，把本目录中的 `src` 文件夹和 `README.md` 拖进去，点 **Commit changes**。
3. 再点 **Add file → Create new file**，文件名一栏输入 `.github/workflows/build.yml`（输入 `/` 会自动创建目录），然后把根目录 `workflow-build.yml` 文件的全部内容粘贴进去，点 **Commit changes**。
4. 打开 **Actions** 标签页，构建自动开始（约 3–5 分钟）。完成后在 **Releases** 页面即可下载：
   - `NppMarkdownPreview-ARM64.zip`（Windows on ARM 原生）
   - `NppMarkdownPreview-x64.zip`

之后每次 `git push`（或网页提交修改）都会自动重新构建并发布新的 Release。

## 安装

1. 下载对应架构的 zip 并解压，得到 `NppMarkdownPreview` 文件夹（内含 `NppMarkdownPreview.dll` 与 `WebView2Loader.dll`）。
2. 将该文件夹复制到 Notepad++ 的 `plugins` 目录，例如：
   - `D:\Application\Notepad++\plugins\NppMarkdownPreview\`（安装版）
   - 或 `%AppData%\Notepad++\plugins\NppMarkdownPreview\`（当前用户）
3. 重启 Notepad++，菜单 **插件 → NppMarkdownPreview → Toggle Markdown Preview**。
4. 打开任意 `.md` 文件即可看到预览（默认停靠在右侧）。

> x64 版 Notepad++ 需要 64 位构建；Windows 11 on ARM 自带 WebView2 运行时，无需额外安装。

## 本机编译（可选）

```
msbuild src\NppMarkdownPreview.vcxproj /restore /p:Configuration=Release /p:Platform=ARM64
msbuild src\NppMarkdownPreview.vcxproj /restore /p:Configuration=Release /p:Platform=x64
```

需要 Visual Studio 2022（v143 工具集 + ARM64 组件）或 VS Build Tools，NuGet 包（WebView2 SDK）会自动还原。

## 许可

- 插件代码：GPLv3（`PluginInterface.h` / `Notepad_plus_msgs.h` / `Scintilla.h` / `Docking.h` 取自 GPLv3 的 Notepad++ 源码树）
- [marked.js](https://github.com/markedjs/marked)：MIT
- WebView2Loader.dll：按 [Microsoft WebView2 SDK](https://learn.microsoft.com/microsoft-edge/webview2/) 许可条款再分发
