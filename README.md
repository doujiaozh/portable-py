# portable-py

> **把一整个 Python 塞进一个 `.exe`。** 双击即 REPL，拖脚本即跑，零安装、零依赖、U 盘即插即走。
> 目标机器没装 Python？不存在的。

实测：**Windows 7 SP1 (x64)** 上从零跑通，冷启动含解压 ~1 秒，之后每次启动等同原生 Python。

---

## 📦 给人用：一个文件就够了

把 `dist/portable-py.exe` 发给任何人（U 盘 / 微信 / GitHub Release 随便）。对方机器 **不需要装任何东西**：

```powershell
# ① 交互式终端（双击 exe 就是它）
portable-py.exe
>>> print("Hello from portable python!")

# ② 一行代码
portable-py.exe -c "print('Hello Portable Python!')"

# ③ 跑脚本
portable-py.exe your_script.py

# ④ 管道 / stdin
echo "print(2**64)" | portable-py.exe -

# ⑤ 退出码原样透传
portable-py.exe -c "import sys; sys.exit(42)"
echo $LASTEXITCODE      # 42
```

**首次运行**：从自身尾部抽出 payload，解压到 `%TEMP%\portable_py_env`，逐条 CRC32 校验；
**之后每次**：直接复用已解压环境，启动开销与原生 Python 无异。
命令行参数被 **原封不动** 透传给内部 `python.exe`，`-c` / `-m` / 脚本路径 / stdin 管道全部可用。

---

## 🧠 架构原理

单文件布局（自解压引导器，非壳、非安装器）：

```
┌───────────────────────────────────────────────────────────┐
│  launcher            25 KB   纯 C / Win32 API，无第三方依赖 │
├───────────────────────────────────────────────────────────┤
│  payload            20.6 MiB  STORED zip                    │
│    └─ python.exe python311.dll *.pyd python311.zip …        │
├───────────────────────────────────────────────────────────┤
│  footer 20 bytes   'PPY1' | u64 launcherLen | u64 payloadLen│
└───────────────────────────────────────────────────────────┘
```

启动器运行流程（`src/launcher.c`）：

1. **读自身尾部 20 字节 footer** → 校验 `PPY1` 魔数 → 定位 payload。
2. **探测环境**：`%TEMP%\portable_py_env\python.exe` 与隐藏标记 `.portable_py_ok` 同时存在则视为就绪。
3. **未就绪则解压**：payload 整体读入内存，从尾部 EOCD (`0x06054b50`) 反向定位中央目录，
   纯手工遍历条目逐个写出（无 7-zip / `Expand-Archive` / 任何外部工具）。
   - 仅认 `method=0 (STORED)`；每条目写完再算 CRC32 复核，错一字节立即终止报错。
   - **zip-slip 防护**：拒绝绝对路径 / 盘符 / `..` 段。
   - **并发安全**：命名互斥量 `Local\portable_py_extract` 保证多个实例首次同时跑时只有一个在解压，其余排队复用。
4. **透传执行**：`CreateProcessW` 启动环境内 `python.exe`，命令行原样转交，`WaitForSingleObject` 等候并 **返回子进程退出码**。

> 为什么 payload 必须是零压缩 (STORED)？因为 launcher 的解压器是纯手写的最小实现（红线：不依赖任何外部解压库），
> 用一点点体积换来零依赖。20.6 MiB vs 压缩后体积——一个 U 盘的事。

---

## 🛠 从源码构建

前置：**MinGW-w64 gcc** 与 **PowerShell 7 (pwsh)**。

```powershell
git clone https://github.com/<you>/portable-py.git
cd portable-py

# 1. 编译启动器（静态链接，只依赖 KERNEL32 / msvcrt）
gcc -O2 -s -Wall -municode -static -o build/launcher.exe src/launcher.c

# 2. 一键打包（自动下载官方嵌入式 Python，默认 3.11.8）
.\build.ps1

# 想换版本？改个参数就行（自动匹配镜像下载）
.\build.ps1 -Version 3.12.3
```

产物：`dist/portable-py.exe`（20.63 MiB）。`build.ps1` 结尾会回读 footer 做完整性自检，过不了直接抛错。

项目结构：

```
portable-py/
├── src/launcher.c        # 启动器源码（单文件，~500 行 C）
├── build.ps1             # 打包脚本：下载 → 重打 STORED → 拼接 footer → 自检
├── test.ps1              # 冒烟测试：8 项断言（首次解压/透传/退出码/脚本/stdin/标准库/免解压快路径）
├── dist/portable-py.exe  # 最终单文件产物
├── LICENSE
└── README.md
```

---

## ⚠️ 已知限制 & 进阶玩法

- **无 pip**：官方 embed 包默认不含 pip。需要装包时：
  ```powershell
  curl.exe -o get-pip.py https://bootstrap.pypa.io/get-pip.py
  portable-py.exe get-pip.py
  # 编辑 %TEMP%\portable_py_env\python311._pth，取消 "#import site" 的注释后 pip 可用
  ```
  注意环境解压在 `%TEMP%`（系统清理或换电脑会重来），适合“随用随造”的体力活场景。
- **杀软误报**：未签名的自解压 PE 是启发式扫描的常客，个别杀毒软件（如 360）可能拦截。加个白名单即可；真要彻底解决得上代码签名证书。
- **Win7 友好**：开发与测试均在 Win7 SP1 完成；启动器只用 `KERNEL32`/`msvcrt` 两个上古系统库，`-static` 静态链接 C 运行时，不留 mingw 运行库依赖。

---

## 📄 许可证

- 启动器与构建/测试脚本：**MIT**（见 [LICENSE](LICENSE)）。
- 内嵌的 [Python](https://www.python.org/) 发行版遵循 **PSF License**（随包附带 `LICENSE.txt`），所有权归 Python 软件基金会。

## 🙏 致谢

- [CPython](https://github.com/python/cpython) 及其官方 Windows embeddable 发行版。
- 纯手工 ZIP 解压参考 PKWARE APPNOTE.txt 规范。
