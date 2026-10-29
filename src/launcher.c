/* ============================================================================
 * portable-py -- launcher.c
 *
 * 纯 C / Win32 API 实现的“自解压启动器”(self-extracting launcher)。
 * 不依赖 7-zip / tar / Expand-Archive 等任何外部解压工具，ZIP 解压逻辑全手工。
 *
 * 最终产物 portable-py.exe 的单文件布局:
 *
 *     +--------------------------+  offset 0
 *     |  launcher  (PE 代码段)   |  长度 L
 *     +--------------------------+  offset L
 *     |  payload  (STORED zip)   |  长度 S
 *     +--------------------------+  offset L+S
 *     |  footer (20 字节)        |  'PPY1' | uint64 L | uint64 S
 *     +--------------------------+
 *
 * 运行流程:
 *   1. 打开自身文件, 读取尾部 20 字节 footer, 定位内嵌 payload。
 *   2. 检查 %TEMP%\portable_py_env 是否已就绪 (标记文件 + python.exe 同时存在)。
 *   3. 未就绪则解析 payload 的 ZIP 中央目录, 把 STORED 条目解压到该目录
 *      (逐条做 CRC32 校验, 任何不一致立即报错)。
 *   4. 用原封不动的命令行参数 CreateProcess 启动 python.exe, 透传退出码。
 *      双击无参数运行时, 子进程即进入 Python 交互式终端 (REPL)。
 *
 * 编译 (MinGW-w64):
 *   gcc -O2 -s -Wall -municode -static -o build\launcher.exe src\launcher.c
 * ============================================================================ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PPY_FOOTER_MAGIC  0x31595050u              /* footer 4 字节: 'P','P','Y','1' */
#define PPY_FOOTER_SIZE   20                       /* magic(4) + uint64 off(8) + uint64 size(8) */
#define PPY_ENV_DIR       L"portable_py_env"       /* 解压目标目录名 */
#define PPY_MARKER        L".portable_py_ok"       /* 解压完成标记文件 */
#define PPY_PYTHON_EXE    L"python.exe"
#define PPY_MUTEX_NAME    L"Local\\portable_py_extract"
#define PPY_MAX_PATH      1024
#define PPY_MAX_PAYLOAD   (512u * 1024u * 1024u)   /* payload 上限 */

/* ---------------- 基础工具 ---------------- */

static uint16_t ppy_rd16(const BYTE* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ppy_rd32(const BYTE* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Fail(const wchar_t* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fwprintf(stderr, L"[portable-py] error: ");
  vfwprintf(stderr, fmt, ap);
  fwprintf(stderr, L"\n");
  fflush(stderr);
  va_end(ap);
  ExitProcess(1);
}

/* ---------------- CRC32 (ZIP 标准多项式) ---------------- */

static uint32_t g_crc_tab[256];

static void crc_init(void)
{
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    g_crc_tab[i] = c;
  }
}

static uint32_t crc32_update(uint32_t crc, const BYTE* p, SIZE_T n)
{
  while (n--)
    crc = g_crc_tab[(crc ^ (uint32_t)*p++) & 0xFFu] ^ (crc >> 8);
  return crc;
}

/* ---------------- 路径工具 ---------------- */

static void PathJoin(wchar_t* dst, size_t cap, const wchar_t* a, const wchar_t* b)
{
  size_t al = wcslen(a), bl = wcslen(b);
  if (al + 1 + bl + 1 > cap)
    Fail(L"path too long: %ls", a);
  memcpy(dst, a, al * sizeof(wchar_t));
  dst[al] = L'\\';
  memcpy(dst + al + 1, b, bl * sizeof(wchar_t));
  dst[al + 1 + bl] = L'\0';
}

/* 递归创建绝对路径的全部目录组件 ( dir 可能以分隔符结尾 ) */
static void EnsureDir(const wchar_t* dir)
{
  wchar_t tmp[PPY_MAX_PATH];
  size_t n = wcslen(dir);

  while (n > 0 && (dir[n - 1] == L'\\' || dir[n - 1] == L'/')) n--;
  if (n == 0) return;
  if (n >= PPY_MAX_PATH) Fail(L"path too long: %ls", dir);
  memcpy(tmp, dir, n * sizeof(wchar_t));
  tmp[n] = L'\0';

  /* 跳过驱动器根 (C:\) 或 UNC 前缀 (\\server\share\) */
  size_t start = 0;
  if (n >= 3 && tmp[1] == L':') {
    start = 3;
  } else if (n >= 2 && tmp[0] == L'\\' && tmp[1] == L'\\') {
    int sep = 0;
    for (size_t i = 0; tmp[i]; i++) {
      if (tmp[i] == L'\\') { sep++; if (sep == 4) { start = i + 1; break; } }
    }
  }

  for (size_t i = start; i < n; i++) {
    if (tmp[i] == L'\\' && i + 1 < n) {
      tmp[i] = L'\0';
      if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        Fail(L"cannot create directory %ls (error %lu)", tmp, GetLastError());
      tmp[i] = L'\\';
    }
  }
  if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    Fail(L"cannot create directory %ls (error %lu)", tmp, GetLastError());
}

/* 为文件路径创建其全部父目录 */
static void EnsureParent(const wchar_t* filePath)
{
  wchar_t tmp[PPY_MAX_PATH];
  size_t n = wcslen(filePath);
  if (n >= PPY_MAX_PATH) Fail(L"path too long: %ls", filePath);
  memcpy(tmp, filePath, n * sizeof(wchar_t));
  tmp[n] = L'\0';
  for (size_t i = n; i > 0; i--) {
    if (tmp[i - 1] == L'\\' || tmp[i - 1] == L'/') {
      tmp[i - 1] = L'\0';
      EnsureDir(tmp);
      return;
    }
  }
}

/* 防范 zip-slip: 拒绝绝对路径 / 盘符 / ".." 段 */
static int ZipNameSafe(const wchar_t* name)
{
  if (!name || !*name) return 0;
  if (name[0] == L'\\') return 0;
  if (name[1] == L':') return 0;
  const wchar_t* p = name;
  while (*p) {
    const wchar_t* seg = p;
    while (*p && *p != L'\\') p++;
    if (p - seg == 2 && seg[0] == L'.' && seg[1] == L'.') return 0;
    if (*p) p++;
  }
  return 1;
}

/* 去掉命令行里的第一个 token (本程序自己的路径), 剩余参数原样返回 */
static const wchar_t* SkipFirstToken(const wchar_t* cl)
{
  if (!cl) return L"";
  if (*cl == L'"') {
    cl++;
    while (*cl) {
      if (*cl == L'"') {
        if (cl[1] == L'"') { cl += 2; continue; }   /* "" 是引号转义 */
        cl++; break;
      }
      cl++;
    }
  } else {
    while (*cl && *cl != L' ' && *cl != L'\t') cl++;
  }
  while (*cl == L' ' || *cl == L'\t') cl++;
  return cl;
}

/* ---------------- footer / payload 读取 ---------------- */

static void ReadFooter(const wchar_t* self, uint64_t* off, uint64_t* size)
{
  HANDLE h = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    Fail(L"cannot open my own executable %ls (error %lu)", self, GetLastError());

  LARGE_INTEGER fs;
  if (!GetFileSizeEx(h, &fs)) Fail(L"cannot get file size (error %lu)", GetLastError());
  if ((uint64_t)fs.QuadPart < (uint64_t)PPY_FOOTER_SIZE + 4096u)
    Fail(L"no payload attached: %ls is not a packaged portable-py executable", self);

  BYTE f[PPY_FOOTER_SIZE];
  LARGE_INTEGER pos, moved;
  pos.QuadPart = fs.QuadPart - PPY_FOOTER_SIZE;
  if (!SetFilePointerEx(h, pos, &moved, FILE_BEGIN))
    Fail(L"cannot seek to footer (error %lu)", GetLastError());
  DWORD got = 0;
  if (!ReadFile(h, f, PPY_FOOTER_SIZE, &got, NULL) || got != PPY_FOOTER_SIZE)
    Fail(L"cannot read footer (error %lu)", GetLastError());
  CloseHandle(h);

  if (ppy_rd32(f) != PPY_FOOTER_MAGIC)
    Fail(L"footer magic not found: this exe has no portable-py payload");

  uint64_t o = 0, s = 0;
  for (int i = 0; i < 8; i++) o |= (uint64_t)f[4 + i] << (8 * i);
  for (int i = 0; i < 8; i++) s |= (uint64_t)f[12 + i] << (8 * i);
  if (o == 0 || s == 0 || o > (uint64_t)fs.QuadPart ||
      o + s + PPY_FOOTER_SIZE != (uint64_t)fs.QuadPart)
    Fail(L"payload descriptor is inconsistent with file size");

  *off = o;
  *size = s;
}

static BYTE* ReadPayload(const wchar_t* self, uint64_t off, uint64_t size)
{
  if (size == 0 || size > (uint64_t)PPY_MAX_PAYLOAD)
    Fail(L"payload size out of range (%I64u bytes)", size);

  HANDLE h = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) Fail(L"cannot open my own executable (error %lu)", GetLastError());

  LARGE_INTEGER pos, moved;
  pos.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(h, pos, &moved, FILE_BEGIN))
    Fail(L"cannot seek to payload (error %lu)", GetLastError());

  SIZE_T sz = (SIZE_T)size;
  BYTE* buf = (BYTE*)VirtualAlloc(NULL, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!buf) Fail(L"out of memory while reading payload (%I64u bytes)", size);

  SIZE_T done = 0;
  while (done < sz) {
    DWORD chunk = (DWORD)(sz - done);
    if (chunk > 0x40000000u) chunk = 0x40000000u;
    DWORD got = 0;
    if (!ReadFile(h, buf + done, chunk, &got, NULL) || got == 0)
      Fail(L"payload read failed at offset %I64u", (uint64_t)done);
    done += got;
  }
  CloseHandle(h);
  return buf;
}

/* ---------------- 手工 ZIP 解压 (仅 STORED 条目) ---------------- */

static const BYTE* ZipFindEocd(const BYTE* p, SIZE_T n)
{
  if (n < 22) return NULL;
  SIZE_T start = (n >= (SIZE_T)65536 + 22) ? n - ((SIZE_T)65536 + 22) : 0;
  SIZE_T i = n - 22;
  for (;;) {
    if (ppy_rd32(p + i) == 0x06054b50u) return p + i;
    if (i == start) break;
    i--;
  }
  return NULL;
}

static void ExtractZip(const BYTE* pay, SIZE_T payN, const wchar_t* envDir)
{
  crc_init();
  const BYTE* eocd = ZipFindEocd(pay, payN);
  if (!eocd) Fail(L"payload is not a ZIP archive (EOCD record not found)");

  uint16_t nent = ppy_rd16(eocd + 10);
  SIZE_T cdOff = ppy_rd32(eocd + 16);
  if (cdOff >= payN || cdOff + 46 > payN)
    Fail(L"corrupt ZIP: central directory out of range");
  const BYTE* cd = pay + cdOff;
  wchar_t dst[PPY_MAX_PATH];
  uint32_t extracted = 0;

  fwprintf(stderr, L"[portable-py] unpacking %u entries to %ls ...\n",
           (unsigned)nent, envDir);

  for (uint32_t e = 0; e < nent; e++) {
    SIZE_T cpos = (SIZE_T)(cd - pay);
    if (cpos + 46 > payN) Fail(L"corrupt ZIP: central directory truncated");
    if (ppy_rd32(cd) != 0x014b50u && ppy_rd32(cd) != 0x02014b50u)
      Fail(L"corrupt ZIP: bad central directory signature");

    uint16_t method  = ppy_rd16(cd + 10);
    uint32_t crcWant = ppy_rd32(cd + 16);
    uint32_t csize   = ppy_rd32(cd + 20);
    uint32_t usize   = ppy_rd32(cd + 24);
    uint16_t nlen    = ppy_rd16(cd + 28);
    uint16_t xlen    = ppy_rd16(cd + 30);
    uint16_t clen    = ppy_rd16(cd + 32);
    uint32_t lho     = ppy_rd32(cd + 42);
    const BYTE* fname = cd + 46;
    if (cpos + 46 + nlen > payN) Fail(L"corrupt ZIP: filename out of range");

    cd += 46 + (SIZE_T)nlen + (SIZE_T)xlen + (SIZE_T)clen;

    /* 用本地文件头定位真正的数据起点 (本地 extra 长度可能与中央目录不同) */
    if ((SIZE_T)lho + 30 > payN) Fail(L"corrupt ZIP: local header out of range");
    const BYTE* lh = pay + lho;
    if (ppy_rd32(lh) != 0x04034b50u) Fail(L"corrupt ZIP: bad local header signature");
    uint16_t lname  = ppy_rd16(lh + 26);
    uint16_t lextra = ppy_rd16(lh + 28);
    SIZE_T dataOff = (SIZE_T)lho + 30 + (SIZE_T)lname + (SIZE_T)lextra;
    if (dataOff + csize > payN) Fail(L"corrupt ZIP: entry data out of range");
    const BYTE* data = pay + dataOff;

    /* 文件名 UTF-8 -> UTF-16, 并统一分隔符 */
    wchar_t wname[PPY_MAX_PATH];
    int wn = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)fname, (int)nlen,
                                 wname, PPY_MAX_PATH - 1);
    if (wn <= 0) Fail(L"corrupt ZIP: cannot decode entry filename");
    wname[wn] = L'\0';
    for (int i = 0; i < wn; i++) if (wname[i] == L'/') wname[i] = L'\\';
    if (!ZipNameSafe(wname)) Fail(L"refusing unsafe path in ZIP: %ls", wname);

    PathJoin(dst, PPY_MAX_PATH, envDir, wname);

    if (wn > 0 && wname[wn - 1] == L'\\') {        /* 目录条目 */
      EnsureDir(dst);
      continue;
    }

    if (method != 0)
      Fail(L"unsupported compression method %u for '%ls' (payload must be STORED)",
           (unsigned)method, wname);
    if (csize != usize)
      Fail(L"corrupt ZIP entry '%ls': stored size mismatch", wname);

    EnsureParent(dst);
    HANDLE hf = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
      Fail(L"cannot create %ls (error %lu)", dst, GetLastError());

    uint32_t crc = 0xFFFFFFFFu;
    SIZE_T remain = csize;
    const BYTE* src = data;
    while (remain > 0) {
      DWORD chunk = (remain > 0x1000000u) ? 0x1000000u : (DWORD)remain;
      DWORD wrote = 0;
      if (!WriteFile(hf, src, chunk, &wrote, NULL) || wrote != chunk) {
        DWORD err = GetLastError();
        CloseHandle(hf);
        Fail(L"write failed for %ls (error %lu)", dst, err);
      }
      crc = crc32_update(crc, src, wrote);
      src += wrote;
      remain -= wrote;
    }
    CloseHandle(hf);
    crc ^= 0xFFFFFFFFu;
    if (crc != crcWant) Fail(L"CRC32 mismatch for %ls (payload is corrupt)", dst);
    extracted++;
  }

  if (extracted == 0) Fail(L"payload ZIP contained no files");
  fwprintf(stderr, L"[portable-py] %u entries extracted and verified\n", extracted);
}

/* ---------------- 标记文件与子进程启动 ---------------- */

static void WriteMarker(const wchar_t* path)
{
  HANDLE hf = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_HIDDEN, NULL);
  if (hf == INVALID_HANDLE_VALUE) {
    fwprintf(stderr, L"[portable-py] warning: cannot write marker %ls (error %lu)\n",
             path, GetLastError());
    return;
  }
  const char* msg = "portable-py environment ready\n";
  DWORD wrote = 0;
  WriteFile(hf, msg, (DWORD)strlen(msg), &wrote, NULL);
  CloseHandle(hf);
}

static int LaunchChild(const wchar_t* exePath)
{
  const wchar_t* rest = SkipFirstToken(GetCommandLineW());
  size_t el = wcslen(exePath);
  size_t rl = wcslen(rest);
  size_t need = 2 + el + (rl ? 1 + rl : 0) + 1;
  if (need > 32767) Fail(L"command line too long to forward");

  wchar_t* cmd = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, need * sizeof(wchar_t));
  if (!cmd) Fail(L"out of memory while building command line");

  size_t p = 0;
  cmd[p++] = L'"';
  memcpy(cmd + p, exePath, el * sizeof(wchar_t));
  p += el;
  cmd[p++] = L'"';
  if (rl) {
    cmd[p++] = L' ';
    memcpy(cmd + p, rest, rl * sizeof(wchar_t));
    p += rl;
  }
  cmd[p] = L'\0';

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  /* bInheritHandles=TRUE: 子进程共享当前控制台, REPL / stdin 管道均可用 */
  if (!CreateProcessW(exePath, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    DWORD err = GetLastError();
    HeapFree(GetProcessHeap(), 0, cmd);
    Fail(L"failed to start %ls (error %lu)", exePath, err);
  }
  HeapFree(GetProcessHeap(), 0, cmd);

  DWORD code = 1;
  if (WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0)
    GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)code;
}

/* ---------------- 主流程 ---------------- */

int wmain(int argc, wchar_t* argv[])
{
  (void)argc;
  (void)argv;

  wchar_t selfPath[PPY_MAX_PATH];
  wchar_t tempDir[PPY_MAX_PATH];
  wchar_t envDir[PPY_MAX_PATH];
  wchar_t pythonExe[PPY_MAX_PATH];
  wchar_t markerPath[PPY_MAX_PATH];

  DWORD got = GetModuleFileNameW(NULL, selfPath, PPY_MAX_PATH);
  if (got == 0 || got >= (DWORD)PPY_MAX_PATH)
    Fail(L"cannot determine my own executable path");

  uint64_t payOff = 0, paySize = 0;
  ReadFooter(selfPath, &payOff, &paySize);

  if (GetTempPathW(PPY_MAX_PATH, tempDir) == 0)
    Fail(L"cannot resolve TEMP directory (error %lu)", GetLastError());
  size_t tl = wcslen(tempDir);
  while (tl > 0 && (tempDir[tl - 1] == L'\\' || tempDir[tl - 1] == L'/')) tl--;
  tempDir[tl] = L'\0';

  PathJoin(envDir, PPY_MAX_PATH, tempDir, PPY_ENV_DIR);
  PathJoin(pythonExe, PPY_MAX_PATH, envDir, PPY_PYTHON_EXE);
  PathJoin(markerPath, PPY_MAX_PATH, envDir, PPY_MARKER);

  /* 命名互斥: 并发首次运行时只有一个实例执行解压, 其余实例等其完成后直接复用 */
  HANDLE mtx = CreateMutexW(NULL, FALSE, PPY_MUTEX_NAME);
  if (mtx) WaitForSingleObject(mtx, INFINITE);

  if (GetFileAttributesW(pythonExe) == INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesW(markerPath) == INVALID_FILE_ATTRIBUTES) {
    EnsureDir(envDir);
    BYTE* pay = ReadPayload(selfPath, payOff, paySize);
    ExtractZip(pay, (SIZE_T)paySize, envDir);
    VirtualFree(pay, 0, MEM_RELEASE);
    if (GetFileAttributesW(pythonExe) == INVALID_FILE_ATTRIBUTES)
      Fail(L"python.exe missing from extracted payload in %ls", envDir);
    WriteMarker(markerPath);
    fwprintf(stderr, L"[portable-py] environment ready in %ls\n", envDir);
  }

  if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }

  return LaunchChild(pythonExe);
}
