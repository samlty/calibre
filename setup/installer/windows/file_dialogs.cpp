/*
 * file_dialogs.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#ifndef _UNICODE
#define _UNICODE
#endif
#include <Windows.h>
#include <Shobjidl.h>
#include <comdef.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

#define PRINTERR(x) fprintf(stderr, "%s", x); fflush(stderr);
#define SECRET_SIZE 32

bool write_bytes(HANDLE pipe, DWORD sz, const char* buf) {
    DWORD written = 0;
    if (!WriteFile(pipe, buf, sz, &written, NULL)) {
        fprintf(stderr, "Failed to write to pipe. GetLastError()=%d\n", GetLastError()); fflush(stderr); return false;
    }
    if (written != sz) {
        fprintf(stderr, "Failed to write to pipe. Incomplete write, leftover bytes: %d", sz - written); fflush(stderr); return false;
    }
    return true;
}

bool read_bytes(size_t sz, char* buf, bool allow_incomplete=false) {
    char *ptr = buf, *limit = buf + sz;
    while(limit > ptr && !feof(stdin) && !ferror(stdin)) {
        ptr += fread(ptr, 1, limit - ptr, stdin);
    }
    if (ferror(stdin)) { PRINTERR("Failed to read from stdin!"); return false; }
    if (ptr - buf != sz) { if (!allow_incomplete) PRINTERR("Truncated input!"); return false; }
    return true;
}

bool from_utf8(size_t sz, const char *src, LPWSTR* ans) {
    int asz = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, (int)sz, NULL, 0);
    if (!asz) { PRINTERR("Failed to get size of UTF-8 string"); return false; }
    *ans = (LPWSTR)calloc(asz+1, sizeof(wchar_t));
    if(*ans == NULL) { PRINTERR("Out of memory!"); return false; }
    asz = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, (int)sz, *ans, asz);
    if (!asz) { PRINTERR("Failed to convert UTF-8 string"); return false; }
    return true;
}

char* to_utf8(LPCWSTR src, int *sz) {
    // Convert to a null-terminated UTF-8 encoded bytearray, allocated on the heap
    char *ans = NULL;
    *sz = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, NULL, 0, NULL, NULL);
    if (!*sz) { PRINTERR("Failed to get size of UTF-16 string"); return NULL; }
    ans = (char*)calloc((*sz) + 1, sizeof(char));
    if (ans == NULL) { PRINTERR("Out of memory!"); return NULL; }
    *sz = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, ans, *sz, NULL, NULL);
    if (!*sz) { PRINTERR("Failed to convert UTF-16 string"); return NULL; }
    return ans;
}

static char* rsbuf = NULL;

bool read_string(unsigned short sz, LPWSTR* ans) {
    memset(rsbuf, 0, 65537);
    if (!read_bytes(sz, rsbuf)) return false;
    if (!from_utf8(sz, rsbuf, ans)) return false;
    return true;
}

COMDLG_FILTERSPEC *read_file_types(UINT *num_file_types) {
    char buf[10] = {0};
    COMDLG_FILTERSPEC *ans = NULL;

    if(!read_bytes(sizeof(unsigned short), buf)) return NULL;
    *num_file_types = *((unsigned short*)buf);
    if (*num_file_types < 1 || *num_file_types > 500) { PRINTERR("Invalid number of file types"); return NULL; }
    ans = (COMDLG_FILTERSPEC*)calloc((*num_file_types) + 1, sizeof(COMDLG_FILTERSPEC));
    if (ans == NULL) { PRINTERR("Out of memory!"); return NULL; }

    for(unsigned short i = 0; i < *num_file_types; i++) {
        if(!read_bytes(sizeof(unsigned short), buf)) return NULL;
        if(!read_string(*((unsigned short*)buf), (LPWSTR*)&(ans[i].pszName))) return NULL;
        if(!read_bytes(sizeof(unsigned short), buf)) return NULL;
        if(!read_string(*((unsigned short*)buf), (LPWSTR*)&(ans[i].pszSpec))) return NULL;
    }
    return ans;
}

static void print_com_error(HRESULT hr, const char *msg) {
    _com_error err(hr);
    LPCWSTR emsg = (LPCWSTR) err.ErrorMessage();
    int sz = 0;
    const char *buf = to_utf8(emsg, &sz);
    if (buf == NULL) { fprintf(stderr, "%s", msg); }
    else { fprintf(stderr, "%s: (HRESULT=0x%x) %s\n", msg, hr, buf); }
    fflush(stderr);
}

#define REPORTERR(hr, x) { print_com_error(hr, x); ret = 1; goto error; }
#define CALLCOM(x, err) hr = x; if(FAILED(hr)) REPORTERR(hr, err)

int show_dialog(HANDLE pipe, char *secret, HWND parent, bool save_dialog, LPWSTR title, LPWSTR folder, LPWSTR filename, LPWSTR save_path, bool multiselect, bool confirm_overwrite, bool only_dirs, bool no_symlinks, COMDLG_FILTERSPEC *file_types, UINT num_file_types) {
    int ret = 0, name_sz = 0;
    IFileDialog *pfd = NULL;
    IShellItemArray *items = NULL;
    IShellItem *item = NULL, *folder_item = NULL, *save_path_item = NULL;
    char *path = NULL;
    DWORD options = 0, item_count = 0;
    LPWSTR name = NULL;
    HRESULT hr = S_OK;
    hr = CoInitialize(NULL);
    if (FAILED(hr)) { PRINTERR("Failed to initialize COM"); return 1; }

    CALLCOM(CoCreateInstance((save_dialog ? CLSID_FileSaveDialog : CLSID_FileOpenDialog),
                NULL, CLSCTX_INPROC_SERVER, (save_dialog ? IID_IFileSaveDialog : IID_IFileOpenDialog),
                reinterpret_cast<LPVOID*>(&pfd)),
        "Failed to create COM object for file dialog")
    CALLCOM(pfd->GetOptions(&options), "Failed to get options")
    options |= FOS_PATHMUSTEXIST | FOS_FORCESHOWHIDDEN;
    if (no_symlinks) options |= FOS_NODEREFERENCELINKS;
    if (save_dialog) {
        options |= FOS_NOREADONLYRETURN;
        if (confirm_overwrite) options |= FOS_OVERWRITEPROMPT;
        if (save_path != NULL) {
            hr = SHCreateItemFromParsingName(save_path, NULL, IID_IShellItem, reinterpret_cast<void **>(&save_path_item));
            // Failure to set initial save path is not critical
            if (SUCCEEDED(hr)) ((IFileSaveDialog*)pfd)->SetSaveAsItem(save_path_item);
        }
    } else {
        if (multiselect) options |= FOS_ALLOWMULTISELECT;
        if (only_dirs) options |= FOS_PICKFOLDERS;
        options |= FOS_FILEMUSTEXIST;
    }
    CALLCOM(pfd->SetOptions(options), "Failed to set options")
    if (title != NULL) { CALLCOM(pfd->SetTitle(title), "Failed to set title") }
    if (folder != NULL) { 
        hr = SHCreateItemFromParsingName(folder, NULL, IID_IShellItem, reinterpret_cast<void **>(&folder_item));
        // Failure to set initial folder is not critical
        if (SUCCEEDED(hr)) pfd->SetFolder(folder_item);
    }
    if (filename != NULL) pfd->SetFileName(filename); // Failure is not critical
    if (!(options & FOS_PICKFOLDERS) && file_types != NULL && num_file_types > 0) {
        CALLCOM(pfd->SetFileTypes(num_file_types, file_types), "Failed to set file types")
    }
    hr = pfd->Show(parent);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) goto error;
    if (FAILED(hr)) REPORTERR(hr, "Failed to show dialog")

    if (save_dialog) {
        CALLCOM(pfd->GetResult(&item), "Failed to get save dialog result");
        CALLCOM(item->GetDisplayName(SIGDN_FILESYSPATH, &name), "Failed to get display name of save dialog result");
        path = to_utf8(name, &name_sz);
        CoTaskMemFree(name); name = NULL;
        if (path == NULL) return 1;
        if (!write_bytes(pipe, SECRET_SIZE+1, secret)) return 1;
        if (!write_bytes(pipe, name_sz, path)) return 1;
    } else {
        CALLCOM(((IFileOpenDialog*)pfd)->GetResults(&items), "Failed to get dialog results");
        CALLCOM(items->GetCount(&item_count), "Failed to get count of results");
        if (item_count > 0) {
            if (!write_bytes(pipe, SECRET_SIZE+1, secret)) return 1;
            for (DWORD i = 0; i < item_count; i++) {
                CALLCOM(items->GetItemAt(i, &item), "Failed to get result item");
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                    path = to_utf8(name, &name_sz);
                    CoTaskMemFree(name); name = NULL;
                    if (path == NULL) return 1;
                    if (!write_bytes(pipe, name_sz, path)) return 1;
                }
            }
        }
    }

error:
    if(pfd) pfd->Release();
    CoUninitialize();
    return ret;
}
#define READ(x, y) if (!read_bytes((x), (y))) return 1;
#define CHECK_KEY(x) (key_size == sizeof(x) - 1 && memcmp(buf, x, sizeof(x) - 1) == 0)
#define READSTR(x) READ(sizeof(unsigned short), buf); if(!read_string(*((unsigned short*)buf), &x)) return 1;
#define SETBINARY(x) if(_setmode(_fileno(x), _O_BINARY) == -1) { PRINTERR("Failed to set binary mode"); return 1; }
#define READBOOL(x)  READ(1, buf); x = !!buf[0]; 

HANDLE open_named_pipe(LPWSTR pipename) {
    HANDLE ans = INVALID_HANDLE_VALUE;
    while(true) {
        ans = CreateFileW(pipename, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (ans != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) {
            fprintf(stderr, "Failed to open pipe. GetLastError()=%d\n", GetLastError()); fflush(stderr); return ans;
        }
        if (!WaitNamedPipeW(pipename, 20000)) {
            fprintf(stderr, "Failed to open pipe. 20 second wait timed out.\n", GetLastError()); fflush(stderr); return ans;
        }
    }
    return ans;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    char buf[257] = {0}, secret[SECRET_SIZE + 1] = {0};
    size_t key_size = 0;
    HWND parent = NULL;
    bool save_dialog = false, multiselect = false, confirm_overwrite = false, only_dirs = false, no_symlinks = false;
    unsigned short len = 0;
    LPWSTR title = NULL, folder = NULL, filename = NULL, save_path = NULL, echo = NULL, pipename = NULL;
    COMDLG_FILTERSPEC *file_types = NULL;
    UINT num_file_types = 0;
    HANDLE pipe = INVALID_HANDLE_VALUE;

    SETBINARY(stdout); SETBINARY(stdin); SETBINARY(stderr);
    // The calibre executables call SetDllDirectory, we unset it here just in
    // case it interferes with some idiotic shell extension or the other
    SetDllDirectory(NULL);
    rsbuf = (char*)calloc(65537, sizeof(char));
    if(rsbuf == NULL) { PRINTERR("Out of memory!"); return 1; }

    while(!feof(stdin)) {
        memset(buf, 0, sizeof(buf));
        if(!read_bytes(1, buf, true)) { if (feof(stdin)) break; return 1;}
        key_size = (size_t)buf[0];
        READ(key_size, buf);
        if CHECK_KEY("HWND") {
            READ(sizeof(HWND), buf);
            if (sizeof(HWND) == 8) parent = (HWND)*((__int64*)buf);
            else if (sizeof(HWND) == 4) parent = (HWND)*((__int32*)buf);
            else { fprintf(stderr, "Unknown pointer size: %zd", sizeof(HWND)); fflush(stderr); return 1;}
        }

        else if CHECK_KEY("PIPENAME") { READSTR(pipename); pipe = open_named_pipe(pipename); if (pipe == INVALID_HANDLE_VALUE) return 1; }

        else if CHECK_KEY("SECRET") { if(!read_bytes(SECRET_SIZE, secret)) return 1; }

        else if CHECK_KEY("TITLE") { READSTR(title) }

        else if CHECK_KEY("FOLDER") { READSTR(folder) }

        else if CHECK_KEY("FILENAME") { READSTR(filename) }

        else if CHECK_KEY("SAVE_PATH") { READSTR(save_path) }

        else if CHECK_KEY("SAVE_AS") { READBOOL(save_dialog) }

        else if CHECK_KEY("MULTISELECT") { READBOOL(multiselect) }

        else if CHECK_KEY("CONFIRM_OVERWRITE") { READBOOL(confirm_overwrite) }

        else if CHECK_KEY("ONLY_DIRS") { READBOOL(only_dirs) }

        else if CHECK_KEY("NO_SYMLINKS") { READBOOL(no_symlinks) }

        else if CHECK_KEY("FILE_TYPES") { file_types = read_file_types(&num_file_types); if (file_types == NULL) return 1; }

        else if CHECK_KEY("ECHO") { READSTR(echo) }

        else {
            PRINTERR("Unknown key");
            return 1;
        }
    }

    if (pipe == INVALID_HANDLE_VALUE) { PRINTERR("No pipename received"); return 1; }
    if (secret == NULL) { PRINTERR("No secret received"); return 1; }

    if (echo != NULL) { 
        int echo_sz = 0;
        char *echo_buf = to_utf8(echo, &echo_sz);
        if (!write_bytes(pipe, SECRET_SIZE+1, secret)) return 1;
        return write_bytes(pipe, echo_sz, echo_buf) ? 0 : 1;
    }

    return show_dialog(pipe, secret, parent, save_dialog, title, folder, filename, save_path, multiselect, confirm_overwrite, only_dirs, no_symlinks, file_types, num_file_types);
}
