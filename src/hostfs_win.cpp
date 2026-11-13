// Host filesystem access for Windows with POSIX semantics.
//
// * Paths: UTF-8 with '/' -> "\\?\" wide paths (no MAX_PATH limit, no Win32 name normalisation, so
//   names like "aux" or trailing dots work). Characters NTFS forbids in names are mapped to
//   U+F000+c (Cygwin's scheme) and mapped back when listing directories.
// * Symlinks: SYSTEM-attribute files containing "!<symlink>\xff\xfe" + UTF-16LE target (Cygwin's
//   format) -- no administrator rights or Developer Mode needed. FIFOs: "!<clover-fifo>" files.
// * unlink/rename use POSIX semantics (FILE_DISPOSITION_POSIX_SEMANTICS) so open files can be
//   removed or replaced, as the firmware expects.
#include "hostfs.h"
#include "winutil.h"
#include <mutex>
#include <memory>
#include <cwchar>

namespace hfs {
namespace {

constexpr char LINK_MAGIC[] = "!<symlink>";
constexpr char FIFO_MAGIC[] = "!<clover-fifo>";

// FILE_INFO_BY_HANDLE_CLASS values / flags (declared locally: older SDKs gate them on NTDDI).
constexpr int kFileDispositionInfoEx = 21;
constexpr int kFileRenameInfoEx = 22;
constexpr ULONG kDispDelete = 0x1, kDispPosix = 0x2, kDispIgnoreReadOnly = 0x10;
constexpr ULONG kRenameReplace = 0x1, kRenamePosix = 0x2;

int win_err(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH: case ERROR_DELETE_PENDING:
        return -lnx::ENOENT_;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: return -lnx::EACCES_;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return -lnx::EEXIST_;
    case ERROR_DIR_NOT_EMPTY: return -lnx::ENOTEMPTY_;
    case ERROR_DIRECTORY: return -lnx::ENOTDIR_;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_BUSY: return -lnx::EBUSY_;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return -lnx::ENOSPC_;
    case ERROR_NOT_SAME_DEVICE: return -lnx::EXDEV_;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return -lnx::ENAMETOOLONG_;
    case ERROR_INVALID_HANDLE: return -lnx::EBADF_;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return -lnx::ENOMEM_;
    case ERROR_WRITE_PROTECT: return -lnx::EROFS_;
    case ERROR_NEGATIVE_SEEK: case ERROR_INVALID_PARAMETER: return -lnx::EINVAL_;
    case ERROR_TOO_MANY_OPEN_FILES: return -lnx::EMFILE_;
    }
    return -lnx::EIO_;
}
int last_err() { return win_err(GetLastError()); }

bool is_mapped_char(wchar_t c) {
    return c < 32 || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'\\';
}

std::wstring wpath(const std::string& p) {
    std::wstring w = utf8_to_wide(p);
    std::wstring out;
    out.reserve(w.size() + 16);
    size_t i = 0;
    if (w.size() >= 2 && (w[0] == L'/' || w[0] == L'\\') && (w[1] == L'/' || w[1] == L'\\')) {
        out = L"\\\\?\\UNC\\";
        i = 2;
    } else if (w.size() >= 2 && iswalpha(w[0]) && w[1] == L':') {
        out = L"\\\\?\\";
        out += w[0];
        out += L':';
        i = 2;
    } else {
        // relative path: anchor at the current directory
        wchar_t cwd[MAX_PATH * 4];
        DWORD n = GetCurrentDirectoryW((DWORD)std::size(cwd), cwd);
        std::string base = wide_to_utf8(cwd, n);
        for (auto& c : base)
            if (c == '\\') c = '/';
        return wpath(base + "/" + p);
    }
    for (; i < w.size(); i++) {
        wchar_t c = w[i];
        if (c == L'/') {
            if (out.empty() || out.back() != L'\\') out += L'\\';
        } else if (is_mapped_char(c)) out += (wchar_t)(0xF000 + c);
        else out += c;
    }
    // drop "\." components and trailing separators (but keep "X:\")
    for (size_t pos; (pos = out.find(L"\\.\\")) != std::wstring::npos;) out.erase(pos, 2);
    if (out.size() >= 2 && out.compare(out.size() - 2, 2, L"\\.") == 0) out.resize(out.size() - 2);
    while (out.size() > 7 && out.back() == L'\\' && out[out.size() - 2] != L':') out.pop_back();
    if (out.size() >= 2 && out.back() == L':') out += L'\\';
    return out;
}

std::string unmap_name(const wchar_t* n) {
    std::wstring w(n);
    for (auto& c : w)
        if (c >= 0xF000 && c < 0xF080 && is_mapped_char((wchar_t)(c - 0xF000))) c = (wchar_t)(c - 0xF000);
    return wide_to_utf8(w);
}

u64 ft_ns(const FILETIME& ft) {
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (t < 116444736000000000ull) return 0;
    return (t - 116444736000000000ull) * 100ull;
}

// Inode numbers are derived from the path (NTFS file IDs would cost an extra open per stat), so the
// path is first normalised lexically: "dir", "dir\" and "dir\." must give the same inode. Programs
// compare fstat(fd) with stat("dir/.") to make sure a directory was not swapped (the menu does
// this before loading its settings, and discarded them on every boot otherwise).
u64 path_ino(const std::wstring& w) {
    size_t start = w.rfind(L"\\\\?\\", 0) == 0 ? 4 : 0;
    std::vector<std::wstring> parts;
    std::wstring cur;
    auto flush = [&] {
        if (cur.empty() || cur == L".") {
        } else if (cur == L"..") {
            if (parts.size() > 1) parts.pop_back();  // never above the drive
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (size_t i = start; i < w.size(); i++) {
        if (w[i] == L'\\' || w[i] == L'/') flush();
        else cur += (wchar_t)towlower(w[i]);
    }
    flush();
    u64 h = 1469598103934665603ull;
    for (auto& p : parts) {
        for (wchar_t c : p) {
            h ^= (u64)c;
            h *= 1099511628211ull;
        }
        h ^= (u64)L'\\';
        h *= 1099511628211ull;
    }
    return h | 1;
}

HANDLE open_raw(const std::wstring& w, DWORD access, DWORD disp, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
    return CreateFileW(w.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                       flags | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

// Marker-file classification. Returns 0 regular, 1 symlink, 2 fifo; fills target for symlinks.
int classify_marker(const std::wstring& w, std::string* target) {
    HANDLE h = open_raw(w, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[8192];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof buf, &got, nullptr);
    CloseHandle(h);
    if (!ok) return 0;
    size_t lm = sizeof LINK_MAGIC - 1, fm = sizeof FIFO_MAGIC - 1;
    if (got >= lm && memcmp(buf, LINK_MAGIC, lm) == 0) {
        if (target) {
            const char* p = buf + lm;
            size_t n = got - lm;
            if (n >= 2 && (u8)p[0] == 0xff && (u8)p[1] == 0xfe) {
                const wchar_t* ws = (const wchar_t*)(p + 2);
                size_t wl = (n - 2) / 2;
                size_t len = 0;
                while (len < wl && ws[len]) len++;
                *target = wide_to_utf8(ws, len);
            } else {
                *target = std::string(p, strnlen(p, n));
            }
        }
        return 1;
    }
    if (got >= fm && memcmp(buf, FIFO_MAGIC, fm) == 0) return 2;
    return 0;
}

bool may_be_marker(DWORD attrs, u64 size) {
    return (attrs & FILE_ATTRIBUTE_SYSTEM) && !(attrs & FILE_ATTRIBUTE_DIRECTORY) && size < 8192;
}

void fill_stat(const std::wstring& w, DWORD attrs, u64 size, const FILETIME& at, const FILETIME& mt, const FILETIME& ct,
               Stat& st) {
    st = Stat{};
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) st.mode = lnx::L_S_IFDIR | 0755;
    else st.mode = lnx::L_S_IFREG | ((attrs & FILE_ATTRIBUTE_READONLY) ? 0555 : 0755);
    st.size = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 4096 : size;
    st.ino = path_ino(w);
    st.nlink = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
    st.blocks = (st.size + 511) / 512;
    st.atime_ns = ft_ns(at);
    st.mtime_ns = ft_ns(mt);
    st.ctime_ns = ft_ns(ct);
    if (may_be_marker(attrs, size)) {
        std::string tgt;
        int k = classify_marker(w, &tgt);
        if (k == 1) {
            st.mode = lnx::L_S_IFLNK | 0777;
            st.size = tgt.size();
        } else if (k == 2) {
            st.mode = lnx::L_S_IFIFO | 0666;
            st.size = 0;
        }
    }
}

// ---------------------------------------------------------------- fd table
struct Entry {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring path;
    bool is_dir = false;
    s64 pos = 0;
    std::mutex m;
};
std::mutex g_fd_mtx;
std::vector<std::shared_ptr<Entry>> g_fds;

std::shared_ptr<Entry> get(int fd) {
    std::lock_guard lk(g_fd_mtx);
    if (fd < 0 || fd >= (int)g_fds.size()) return nullptr;
    return g_fds[fd];
}
int install(std::shared_ptr<Entry> e) {
    std::lock_guard lk(g_fd_mtx);
    for (size_t i = 0; i < g_fds.size(); i++)
        if (!g_fds[i]) {
            g_fds[i] = std::move(e);
            return (int)i;
        }
    g_fds.push_back(std::move(e));
    return (int)g_fds.size() - 1;
}

s64 file_size(HANDLE h) {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) return -1;
    return sz.QuadPart;
}

// Guest memory is committed lazily by a vectored exception handler, but the kernel does not raise
// user-mode faults: ReadFile/WriteFile on a not-yet-committed guest page fail with ERROR_NOACCESS.
// Touch every page of the buffer from user mode (which commits it) and let the caller retry.
void prefault(const void* buf, u32 n) {
    if (!n) return;
    const volatile u8* p = (const volatile u8*)buf;
    uintptr_t a = (uintptr_t)buf & ~(uintptr_t)0xfff, e = (uintptr_t)buf + n;
    (void)p[0];
    for (a += 0x1000; a < e; a += 0x1000) (void)*(const volatile u8*)a;
}

s64 do_read(HANDLE h, void* buf, u32 n, s64 off) {
    for (int attempt = 0;; attempt++) {
        OVERLAPPED ov{};
        ov.Offset = (DWORD)((u64)off & 0xffffffffu);
        ov.OffsetHigh = (DWORD)((u64)off >> 32);
        DWORD got = 0;
        if (ReadFile(h, buf, n, &got, &ov)) return got;
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return 0;
        if (e == ERROR_NOACCESS && attempt == 0) {
            prefault(buf, n);
            continue;
        }
        return win_err(e);
    }
}
s64 do_write(HANDLE h, const void* buf, u32 n, s64 off) {
    for (int attempt = 0;; attempt++) {
        OVERLAPPED ov{};
        ov.Offset = (DWORD)((u64)off & 0xffffffffu);
        ov.OffsetHigh = (DWORD)((u64)off >> 32);
        DWORD put = 0;
        if (WriteFile(h, buf, n, &put, &ov)) return put;
        DWORD e = GetLastError();
        if (e == ERROR_NOACCESS && attempt == 0) {
            prefault(buf, n);
            continue;
        }
        return win_err(e);
    }
}

int set_eof(HANDLE h, s64 len) {
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = len;
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof info) ? 0 : last_err();
}

int lstat_w(const std::wstring& w, Stat& st) {
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &ad)) return last_err();
    u64 size = ((u64)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
    fill_stat(w, ad.dwFileAttributes, size, ad.ftLastAccessTime, ad.ftLastWriteTime, ad.ftCreationTime, st);
    return 0;
}

int create_marker(const std::wstring& w, const void* data, size_t n) {
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_SYSTEM, nullptr);
    if (h == INVALID_HANDLE_VALUE) return last_err();
    DWORD put = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &put, nullptr);
    CloseHandle(h);
    return ok && put == n ? 0 : -lnx::EIO_;
}

}  // namespace

// ---------------------------------------------------------------- fd operations
int open(const std::string& path, u32 f, u32 mode) {
    std::wstring w = wpath(path);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    bool exists = GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &ad) != 0;
    bool excl_create = (f & lnx::L_O_CREAT) && (f & lnx::L_O_EXCL);
    if (exists && excl_create) return -lnx::EEXIST_;
    u32 acc = f & 3;
    if (exists && (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (acc != 0) return -lnx::EISDIR_;
        auto e = std::make_shared<Entry>();
        e->path = w;
        e->is_dir = true;
        return install(e);
    }
    if (!exists && (f & lnx::L_O_DIRECTORY)) return -lnx::ENOENT_;
    if (exists && (f & lnx::L_O_DIRECTORY)) return -lnx::ENOTDIR_;
    DWORD access = acc == 0 ? GENERIC_READ : acc == 1 ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE);
    DWORD disp;
    if (f & lnx::L_O_CREAT) disp = excl_create ? CREATE_NEW : OPEN_ALWAYS;
    else disp = OPEN_EXISTING;
    HANDLE h = CreateFileW(w.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED && exists && (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -lnx::EISDIR_;
        return win_err(e);
    }
    if ((f & lnx::L_O_TRUNC) && acc != 0) set_eof(h, 0);
    auto e = std::make_shared<Entry>();
    e->h = h;
    e->path = w;
    return install(e);
}

int close(int fd) {
    std::shared_ptr<Entry> e;
    {
        std::lock_guard lk(g_fd_mtx);
        if (fd < 0 || fd >= (int)g_fds.size() || !g_fds[fd]) return -lnx::EBADF_;
        e = std::move(g_fds[fd]);
    }
    if (e->h != INVALID_HANDLE_VALUE) CloseHandle(e->h);
    return 0;
}

s64 read(int fd, void* buf, u32 n) {
    auto e = get(fd);
    if (!e) return -lnx::EBADF_;
    if (e->is_dir) return -lnx::EISDIR_;
    std::lock_guard lk(e->m);
    s64 r = do_read(e->h, buf, n, e->pos);
    if (r > 0) e->pos += r;
    return r;
}

s64 write(int fd, const void* buf, u32 n, bool append) {
    auto e = get(fd);
    if (!e) return -lnx::EBADF_;
    if (e->is_dir) return -lnx::EISDIR_;
    std::lock_guard lk(e->m);
    if (append) {
        s64 sz = file_size(e->h);
        if (sz >= 0) e->pos = sz;
    }
    s64 r = do_write(e->h, buf, n, e->pos);
    if (r > 0) e->pos += r;
    return r;
}

s64 pread(int fd, void* buf, u32 n, s64 off) {
    auto e = get(fd);
    if (!e) return -lnx::EBADF_;
    if (e->is_dir) return -lnx::EISDIR_;
    return do_read(e->h, buf, n, off);
}

s64 pwrite(int fd, const void* buf, u32 n, s64 off) {
    auto e = get(fd);
    if (!e) return -lnx::EBADF_;
    if (e->is_dir) return -lnx::EISDIR_;
    return do_write(e->h, buf, n, off);
}

s64 lseek(int fd, s64 off, int whence) {
    auto e = get(fd);
    if (!e) return -lnx::EBADF_;
    std::lock_guard lk(e->m);
    s64 np;
    if (whence == 0) np = off;
    else if (whence == 1) np = e->pos + off;
    else if (whence == 2) {
        s64 sz = e->is_dir ? 0 : file_size(e->h);
        if (sz < 0) return last_err();
        np = sz + off;
    } else return -lnx::EINVAL_;
    if (np < 0) return -lnx::EINVAL_;
    e->pos = np;
    return np;
}

int fstat(int fd, Stat& st) {
    auto e = get(fd);
    if (!e) return -lnx::EBADF_;
    if (e->is_dir) return lstat_w(e->path, st);
    BY_HANDLE_FILE_INFORMATION bi;
    if (!GetFileInformationByHandle(e->h, &bi)) return last_err();
    u64 size = ((u64)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    st = Stat{};
    st.mode = lnx::L_S_IFREG | ((bi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0555 : 0755);
    st.size = size;
    st.ino = path_ino(e->path);
    st.nlink = bi.nNumberOfLinks ? bi.nNumberOfLinks : 1;
    st.blocks = (size + 511) / 512;
    st.atime_ns = ft_ns(bi.ftLastAccessTime);
    st.mtime_ns = ft_ns(bi.ftLastWriteTime);
    st.ctime_ns = ft_ns(bi.ftCreationTime);
    return 0;
}

int ftruncate(int fd, s64 len) {
    auto e = get(fd);
    if (!e) return -lnx::EBADF_;
    if (e->is_dir) return -lnx::EISDIR_;
    return set_eof(e->h, len);
}

// ---------------------------------------------------------------- path operations
int lstat(const std::string& path, Stat& st) { return lstat_w(wpath(path), st); }

int stat(const std::string& path, Stat& st) { return lstat(path, st); }

int readlink(const std::string& path, std::string& target) {
    std::wstring w = wpath(path);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &ad)) return last_err();
    u64 size = ((u64)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
    if (!may_be_marker(ad.dwFileAttributes, size)) return -lnx::EINVAL_;
    return classify_marker(w, &target) == 1 ? 0 : -lnx::EINVAL_;
}

int symlink(const std::string& target, const std::string& path) {
    std::wstring wt = utf8_to_wide(target);
    std::string data(LINK_MAGIC);
    data += "\xff\xfe";
    data.append((const char*)wt.data(), wt.size() * 2);
    data.append(2, '\0');
    return create_marker(wpath(path), data.data(), data.size());
}

int mkfifo(const std::string& path, u32) { return create_marker(wpath(path), FIFO_MAGIC, sizeof FIFO_MAGIC - 1); }

int link(const std::string& o, const std::string& n) {
    return CreateHardLinkW(wpath(n).c_str(), wpath(o).c_str(), nullptr) ? 0 : last_err();
}

int unlink(const std::string& path) {
    std::wstring w = wpath(path);
    DWORD attrs = GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return last_err();
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return -lnx::EISDIR_;
    HANDLE h = CreateFileW(w.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return last_err();
    ULONG flags = kDispDelete | kDispPosix | kDispIgnoreReadOnly;
    BOOL ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)kFileDispositionInfoEx, &flags, sizeof flags);
    if (!ok) {
        // filesystems without POSIX semantics (FAT/exFAT): classic delete-on-close
        FILE_DISPOSITION_INFO di{TRUE};
        ok = SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di);
    }
    DWORD e = GetLastError();
    CloseHandle(h);
    return ok ? 0 : win_err(e);
}

int rmdir(const std::string& path) {
    std::wstring w = wpath(path);
    DWORD attrs = GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return last_err();
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -lnx::ENOTDIR_;
    return RemoveDirectoryW(w.c_str()) ? 0 : last_err();
}

int mkdir(const std::string& path, u32) {
    return CreateDirectoryW(wpath(path).c_str(), nullptr) ? 0 : last_err();
}

int rename(const std::string& o, const std::string& n) {
    std::wstring wo = wpath(o), wn = wpath(n);
    DWORD oattr = GetFileAttributesW(wo.c_str());
    if (oattr == INVALID_FILE_ATTRIBUTES) return last_err();
    DWORD nattr = GetFileAttributesW(wn.c_str());
    if (nattr != INVALID_FILE_ATTRIBUTES) {
        bool od = oattr & FILE_ATTRIBUTE_DIRECTORY, nd = nattr & FILE_ATTRIBUTE_DIRECTORY;
        if (od && !nd) return -lnx::ENOTDIR_;
        if (!od && nd) return -lnx::EISDIR_;
        if (od && nd && !RemoveDirectoryW(wn.c_str())) return last_err();  // POSIX: replace an empty directory
    }
    HANDLE h = CreateFileW(wo.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        // FileRenameInfoEx wants an NT path: "\??\X:\..."
        std::wstring nt = wn;
        if (nt.compare(0, 4, L"\\\\?\\") == 0) nt.replace(0, 4, L"\\??\\");
        size_t bytes = sizeof(FILE_RENAME_INFO) + nt.size() * sizeof(wchar_t);
        std::vector<u8> buf(bytes, 0);
        auto* ri = (FILE_RENAME_INFO*)buf.data();
        *(DWORD*)ri = kRenameReplace | kRenamePosix;  // union { BOOLEAN ReplaceIfExists; DWORD Flags; }
        ri->RootDirectory = nullptr;
        ri->FileNameLength = (DWORD)(nt.size() * sizeof(wchar_t));
        memcpy(ri->FileName, nt.c_str(), nt.size() * sizeof(wchar_t));
        BOOL ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)kFileRenameInfoEx, ri, (DWORD)bytes);
        CloseHandle(h);
        if (ok) return 0;
    }
    return MoveFileExW(wo.c_str(), wn.c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : last_err();
}

int truncate(const std::string& path, s64 len) {
    HANDLE h = open_raw(wpath(path), GENERIC_WRITE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return last_err();
    int r = set_eof(h, len);
    CloseHandle(h);
    return r;
}

bool exists(const std::string& path) { return GetFileAttributesW(wpath(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

int list_dir(const std::string& path, std::vector<std::pair<std::string, u8>>& out) {
    std::wstring w = wpath(path);
    std::wstring pat = w + (w.back() == L'\\' ? L"*" : L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) {
            out.push_back({".", 4});
            out.push_back({"..", 4});
            return 0;
        }
        return win_err(e);
    }
    bool dot = false, dotdot = false;
    do {
        std::string name = unmap_name(fd.cFileName);
        if (name == ".") dot = true;
        if (name == "..") dotdot = true;
        u8 type = 8;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) type = 4;
        else {
            u64 size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (may_be_marker(fd.dwFileAttributes, size)) {
                int k = classify_marker(w + L"\\" + fd.cFileName, nullptr);
                type = k == 1 ? 10 : k == 2 ? 1 : 8;
            }
        }
        out.push_back({name, type});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!dotdot) out.insert(out.begin(), {"..", 4});
    if (!dot) out.insert(out.begin(), {".", 4});
    return 0;
}

bool write_file(const std::string& path, const void* data, size_t n, u32) {
    HANDLE h = open_raw(wpath(path), GENERIC_WRITE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &put, nullptr);
    CloseHandle(h);
    return ok && put == n;
}

}  // namespace hfs
