/* EasyRadio core platform for the plugin: Win32 only (kernel32), UTF-8 paths converted to UTF-16. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "er_core.h"

void *er_alloc(size_t size) {
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}
void er_free(void *memory) {
    if (memory) HeapFree(GetProcessHeap(), 0, memory);
}

/* UTF-8 -> UTF-16; \\?\ for long absolute drive paths. Freed with er_free. */
static wchar_t *wide(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) return NULL;
    int prefix = n > 240 && path[0] && path[1] == ':' ? 4 : 0;
    wchar_t *w = er_alloc(((size_t)n + 4) * sizeof(wchar_t));
    if (!w) return NULL;
    if (prefix) w[0] = L'\\', w[1] = L'\\', w[2] = L'?', w[3] = L'\\';
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w + prefix, n);
    for (int i = prefix; w[i]; ++i)
        if (w[i] == L'/') w[i] = L'\\';
    return w;
}

struct ErFile {
    HANDLE handle;
};
ErFile *er_open(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    er_free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    ErFile *f = er_alloc(sizeof(*f));
    if (!f) {
        CloseHandle(h);
        return NULL;
    }
    f->handle = h;
    return f;
}
uint64_t er_file_size(ErFile *f) {
    LARGE_INTEGER size;
    return GetFileSizeEx(f->handle, &size) ? (uint64_t)size.QuadPart : 0;
}
int er_read_at(ErFile *f, uint64_t offset, void *buffer, size_t size) {
    LARGE_INTEGER to;
    to.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f->handle, to, NULL, FILE_BEGIN)) return 0;
    size_t done = 0;
    while (done < size) {
        DWORD want = size - done > 0x40000000 ? 0x40000000 : (DWORD)(size - done), got = 0;
        if (!ReadFile(f->handle, (char *)buffer + done, want, &got, NULL) || !got) return 0;
        done += got;
    }
    return 1;
}
void er_close(ErFile *f) {
    if (f) {
        CloseHandle(f->handle);
        er_free(f);
    }
}
int er_write_file(const char *path, const void *data, size_t size) {
    wchar_t *w = wide(path);
    if (!w) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    er_free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &written, NULL) && written == size;
    return CloseHandle(h) && ok;
}
int er_make_dirs(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return 0;
    int start = w[0] == L'\\' && w[1] == L'\\' ? 4 : 0;
    for (int i = start + 3; ; ++i) {
        if (w[i] == L'\\' || !w[i]) {
            wchar_t c = w[i];
            w[i] = 0;
            if (i > start + 2) CreateDirectoryW(w, NULL);
            w[i] = c;
            if (!c) break;
        }
    }
    DWORD a = GetFileAttributesW(w);
    er_free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int er_delete_file(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return 0;
    int ok = DeleteFileW(w) || GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES;
    er_free(w);
    return ok;
}
int er_list_dir(const char *path, ErVisit visit, void *context) {
    char *pattern = er_join(path, "\\*", NULL);
    wchar_t *w = pattern ? wide(pattern) : NULL;
    er_free(pattern);
    if (!w) return 0;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(w, &data);
    er_free(w);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (data.cFileName[0] == L'.' && (!data.cFileName[1] || (data.cFileName[1] == L'.' && !data.cFileName[2]))) continue;
        char name[MAX_PATH * 3 + 4];
        if (WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, sizeof(name), NULL, NULL) <= 0) continue;
        uint64_t size = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
        uint64_t mtime = (uint64_t)data.ftLastWriteTime.dwHighDateTime << 32 | data.ftLastWriteTime.dwLowDateTime;
        visit(name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, size, mtime, context);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return 1;
}
uint64_t er_random_seed(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t seed = __builtin_ia32_rdtsc() ^ ((uint64_t)counter.QuadPart << 17) ^ GetTickCount64() ^ ((uint64_t)GetCurrentProcessId() << 40);
    return seed;
}
