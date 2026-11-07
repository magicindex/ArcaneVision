# EPUB 预览说明（0.8.4）

## 现象

在荣耀手机上点搜索到的 `.epub` 电子书，页面会跳到飞牛「预览」应用，随后停在加载页/空白页，
书打不开。小米手机（较新的 WebView）不一定复现。

## 排查过程

### 1. 文件本身没有问题

以出问题的《巴黎没有摩天轮》为例：

- `file` 识别为 `EPUB document`，大小 253 KB；
- `unzip -l` 可以看到标准的 `mimetype`、`META-INF/container.xml`、`content.opf`、章节
  XHTML 与图片；
- 没有 `META-INF/encryption.xml`，即 **没有 DRM 加密**。

### 2. 玄览把 EPUB 交给了「预览」应用

0.8.3 的打开逻辑里，EPUB 归在 PDF 分类下，但没有内置渲染器，于是走
`noBuiltinPreview()`：检测到「在线预览」应用在运行，就打开

```
/app/trim-preview/?path=<URL 编码的 epub 路径>
```

网关日志显示该请求返回 200，紧接着加载了预览应用的静态资源，但**没有任何
`/app/trim-preview/api/v1/file-info` 或 `file-content` 请求**——预览应用在发请求之前就挂了。

### 3. 「预览」应用的前端在 Chrome 83 上语法不兼容

荣耀手机（Android 10，飞牛 App 内置 WebView）的 UA 是
`Chrome/83.0.4103.106`。预览应用的前端 bundle 使用了：

- `??=`（逻辑空赋值，Chrome 85+）
- `||=`（逻辑或赋值，Chrome 85+）

Chrome 83 在解析阶段就抛 `SyntaxError`，React 根本没有启动，所以既没有请求文件，也没有
显示错误。预览应用本身是支持 EPUB 的（静态资源里有 `EpubViewer`），但它的构建目标高于
这台手机的 WebView。

### 4. 结论

EPUB 打不开是两层原因叠加：

1. 玄览自身没有 EPUB 阅读器，依赖外部「预览」应用；
2. 外部预览应用的 JS 需要 Chrome 85+，荣耀的 Chrome 83 跑不起来。

更新系统 WebView 可以让「预览」应用恢复，但不是所有旧机型都能更新；更稳妥的做法是让玄览
自己读 EPUB。

## 0.8.4 的实现

### 解压与解析

- 新增加载 `ui/vendor/fflate.min.js`（fflate 0.8.2，MIT），只在打开 EPUB 时动态加载；
- 读取 `/api/file?path=...` 的字节流，按 ZIP 中央目录粗检解压总量（防 zip bomb），
  再用 `fflate.unzipSync` 解压；
- 依次解析 `META-INF/container.xml` → OPF → `manifest` / `spine`；
- 目录优先取 EPUB3 的 `nav`（`properties="nav"`），否则取 EPUB2 的 `toc.ncx`；
- 按 spine 顺序渲染章节，支持目录、上一章/下一章、章节内锚点与跨章节链接。

### 资源处理

- 章节内的图片按相对路径从 ZIP 中取出，转成 `data:` URI 内嵌，不依赖外部请求；
- 书自带的 CSS 会合并进来，`url(...)` 指向 ZIP 内图片的同样转 data URI；
  外链、`@import`、`expression()` 一律去掉；
- 字体、音频、视频不做内嵌（避免体积失控），由 CSP 拦截。

### 安全

- 章节 HTML 用白名单清洗：移除 `script/iframe/object/embed/form/input/...`，
  去掉所有 `on*` 事件属性、`javascript:` 链接、外链图片与 `srcset`；
- 所有 `id` 加章节前缀，避免不同章节锚点冲突；
- 阅读器 iframe 使用 `sandbox="allow-same-origin allow-popups"`（不允许脚本），
  生成文档再带一条 CSP：`default-src 'none'; img-src data:; style-src 'unsafe-inline'`；
- `META-INF/encryption.xml` 含 `EncryptedData` 时直接提示 DRM 不支持；
- 限额：EPUB 文件 ≤ 64 MB、解压总量 ≤ 256 MB、生成的 HTML ≤ 12 MB，超出时提示或截断。

### 打开方式

- EPUB 默认走内置阅读器（PDF 分类的默认打开方式 `builtin`）；
- 如果用户在设置里把 PDF 分类改成「文档服务」或「在线预览」，仍按用户选择走外部应用；
- 外部应用失败时不再留下空白面板：内置阅读器会显示具体错误，并提示下载或换阅读器。

## 验证

用 headless Chromium 加载真实前端 + 模拟 API，分别用合成 EPUB 与真实 EPUB 测试：

| 用例 | 结果 |
| --- | --- |
| 合成 EPUB（2 章、NCX、脚本、`onclick`、跨章链接、内嵌 PNG） | 2 章、2 条目录、脚本 0、事件属性 0、图片为 `data:`、跨章链接 `#c1-end` |
| 真实 EPUB（253 KB，59 章，50 条目录，4 张图） | 59 章、50 条目录、4 张 `data:` 图片，生成的 srcdoc 约 280 KB，无控制台错误 |
| EPUB 关闭后再打开图片 | iframe 的 `sandbox`/`srcdoc` 已清除，`src` 恢复正常图片地址 |

兼容性按 Chrome 83 的语法集合检查：fflate 为 ES5，阅读器代码没有使用 `??=`、`||=`、
`.at()`、`replaceAll` 等 Chrome 85+ 语法；`srcdoc`、`sandbox`、`TextDecoder`、`fetch`
在 Chrome 83 均可用。

## 仍不支持

- DRM 加密的 EPUB；
- EPUB3 的音频/视频、脚本化内容（按安全策略丢弃）；
- 分页阅读、阅读进度、书签、批注、全文搜索（当前是单页滚动 + 目录跳转）；
- 复杂 CSS（浮动、多栏、绝对定位）可能与原书排版有差异；
- XPS、DjVu、FB2 仍没有内置阅读器，只提供下载或外部应用。
