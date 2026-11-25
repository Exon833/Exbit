// Drag and drop between a plugin's window and Exbit (see dragdrop.h).
#include "winehost/dragdrop.h"
#include "core/program_environment.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

typedef HRESULT(WINAPI* DoDragDropFn)(void* data, void* source, DWORD okEffects, DWORD* effect);

DoDragDropFn realDoDragDrop = nullptr;
std::mutex lock;
std::vector<std::string> dragged;  // (under `lock`)
bool tracing = environment::readProgramVariable("WINE_TRACE") != nullptr;

std::u16string wide(const char* s) {
    std::u16string out;
    int n = MultiByteToWideChar(0, 0, s, -1, nullptr, 0);  // (CP_ACP: an ANSI CF_HDROP)
    if (n > 0) {
        out.resize((size_t)n);
        MultiByteToWideChar(0, 0, s, -1, (WCHAR*)&out[0], n);
        out.pop_back();  // (its ending 0)
    }
    return out;
}

// the DOS names a CF_HDROP block holds
std::vector<std::u16string> namesIn(HGLOBAL memory) {
    std::vector<std::u16string> out;
    auto* df = (const DROPFILES*)GlobalLock(memory);
    if (!df)
        return out;
    SIZE_T size = GlobalSize(memory);
    const char* end = (const char*)df + size;
    const char* at = (const char*)df + df->pFiles;
    if (df->pFiles >= sizeof(DROPFILES) && df->pFiles < size) {
        if (df->fWide) {
            for (auto* p = (const char16_t*)at; (const char*)(p + 1) <= end && *p;) {
                std::u16string name;
                while ((const char*)(p + 1) <= end && *p)
                    name += *p++;
                out.push_back(name);
                p++;
            }
        } else {
            for (const char* p = at; p < end && *p;) {
                std::string name;
                while (p < end && *p)
                    name += *p++;
                out.push_back(wide(name.c_str()));
                p++;
            }
        }
    }
    GlobalUnlock(memory);
    return out;
}

std::string unixName(const std::u16string& dos) {
    char* u = wine_get_unix_file_name((LPCWSTR)dos.c_str());
    if (!u)
        return "";
    std::string out = u;
    HeapFree(GetProcessHeap(), 0, u);
    return out;
}

// the plugin's drag: files taken at once, anything else to Wine
HRESULT WINAPI ourDoDragDrop(void* data, void* source, DWORD okEffects, DWORD* effect) {
    std::vector<std::string> files;
    if (data) {
        auto* table = *(IDataObjectVtbl**)data;
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (table->GetData(data, &format, &medium) == S_OK) {
            if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
                for (const auto& dos : namesIn(medium.hGlobal)) {
                    std::string u = unixName(dos);
                    if (!u.empty())
                        files.push_back(u);
                }
            ReleaseStgMedium(&medium);
        }
    }
    if (files.empty())
        return realDoDragDrop ? realDoDragDrop(data, source, okEffects, effect) : E_NOTIMPL;
    if (tracing)
        fprintf(stderr, "exbit-wine-host: the plugin dragged out %zu file(s), the first %s\n", files.size(),
                files[0].c_str());
    {
        std::lock_guard<std::mutex> hold(lock);
        dragged.insert(dragged.end(), files.begin(), files.end());
    }
    if (effect)  // (copied, never moved: the plugin keeps its file, Exbit reads it)
        *effect = okEffects & DROPEFFECT_COPY ? DROPEFFECT_COPY : okEffects & (0u - okEffects);
    return DRAGDROP_S_DROP;
}

// --- a data object holding files (for drops into the plugin) ---

const GUID IID_IUnknown_ = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const GUID IID_IDataObject_ = {0x0000010E, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

// a CF_HDROP block of these DOS names (GlobalAlloc'd: WM_DROPFILES' receiver frees it with DragFinish)
HGLOBAL makeHdrop(const std::vector<std::u16string>& names, POINT at) {
    size_t chars = 1;
    for (const auto& n : names)
        chars += n.size() + 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(char16_t));
    if (!memory)
        return nullptr;
    auto* df = (DROPFILES*)GlobalLock(memory);
    df->pFiles = sizeof(DROPFILES);
    df->pt = at;
    df->fNC = FALSE;
    df->fWide = TRUE;
    auto* p = (char16_t*)(df + 1);
    for (const auto& n : names) {
        std::memcpy(p, n.data(), n.size() * sizeof(char16_t));
        p += n.size() + 1;  // (its 0: zeroed already; the list's last 0 too)
    }
    GlobalUnlock(memory);
    return memory;
}

struct FileData {
    const IDataObjectVtbl* table;
    LONG refs;
    std::vector<std::u16string> names;
    POINT at;
};

bool sameGuid(const GUID* a, const GUID& b) { return a && std::memcmp(a, &b, sizeof(GUID)) == 0; }
bool wantsFiles(const FORMATETC* f) { return f && f->cfFormat == CF_HDROP && (f->tymed & TYMED_HGLOBAL); }

HRESULT WINAPI fdQueryInterface(void* self, const GUID* iid, void** object) {
    if (!object)
        return E_NOINTERFACE;
    if (sameGuid(iid, IID_IUnknown_) || sameGuid(iid, IID_IDataObject_)) {
        *object = self;
        ((FileData*)self)->refs++;
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}
ULONG WINAPI fdAddRef(void* self) { return (ULONG)++((FileData*)self)->refs; }
ULONG WINAPI fdRelease(void* self) {
    auto* d = (FileData*)self;
    LONG left = --d->refs;
    if (left == 0)
        delete d;
    return (ULONG)left;
}
HRESULT WINAPI fdGetData(void* self, FORMATETC* format, STGMEDIUM* medium) {
    auto* d = (FileData*)self;
    if (!wantsFiles(format) || !medium)
        return DV_E_FORMATETC;
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = makeHdrop(d->names, d->at);
    medium->pUnkForRelease = nullptr;
    return medium->hGlobal ? S_OK : E_NOTIMPL;
}
HRESULT WINAPI fdGetDataHere(void*, FORMATETC*, STGMEDIUM*) { return E_NOTIMPL; }
HRESULT WINAPI fdQueryGetData(void*, FORMATETC* format) { return wantsFiles(format) ? S_OK : DV_E_FORMATETC; }
HRESULT WINAPI fdGetCanonical(void*, FORMATETC*, FORMATETC* out) {
    if (out)
        out->ptd = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI fdSetData(void*, FORMATETC*, STGMEDIUM*, BOOL) { return E_NOTIMPL; }
HRESULT WINAPI fdEnumFormatEtc(void*, DWORD, void** enumerator) {
    if (enumerator)
        *enumerator = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI fdDAdvise(void*, FORMATETC*, DWORD, void*, DWORD*) { return OLE_E_ADVISENOTSUPPORTED; }
HRESULT WINAPI fdDUnadvise(void*, DWORD) { return OLE_E_ADVISENOTSUPPORTED; }
HRESULT WINAPI fdEnumDAdvise(void*, void**) { return OLE_E_ADVISENOTSUPPORTED; }

const IDataObjectVtbl fileDataTable = {fdQueryInterface, fdAddRef,       fdRelease,        fdGetData,
                                       fdGetDataHere,    fdQueryGetData, fdGetCanonical,   fdSetData,
                                       fdEnumFormatEtc,  fdDAdvise,      fdDUnadvise,      fdEnumDAdvise};

// the window of `top` (itself or one inside it) under the screen point
HWND windowAt(HWND top, POINT screen) {
    HWND h = WindowFromPoint(screen);
    if (h && (h == top || IsChild(top, h)))
        return h;
    h = top;  // (Wine's idea of the screen may put another window there: down from ours by hand)
    for (int depth = 0; depth < 32; depth++) {
        POINT local = screen;
        ScreenToClient(h, &local);
        HWND inner = ChildWindowFromPointEx(h, local, CWP_SKIPINVISIBLE);
        if (!inner || inner == h)
            break;
        h = inner;
    }
    return h;
}

}  // namespace

void catchDragsOut() {
    HMODULE ole32 = LoadLibraryW(u"ole32.dll");
    void* real = ole32 ? GetProcAddress(ole32, "DoDragDrop") : nullptr;
    if (!real)
        return;
    realDoDragDrop = (DoDragDropFn)real;
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed))
        return;
    int patched = 0;
    for (DWORD m = 0; m < (needed / sizeof(HMODULE) < 1024 ? needed / sizeof(HMODULE) : 1024); m++) {
        auto* base = (unsigned char*)modules[m];
        uint32_t pe = *(const uint32_t*)(base + 0x3C);
        uint16_t magic = *(const uint16_t*)(base + pe + 24);
        // (the import table's place: the second data directory, after the optional header's fields)
        uint32_t directories = magic == 0x20B ? 112 : magic == 0x10B ? 96 : 0;
        if (!directories || sizeof(void*) != (magic == 0x20B ? 8u : 4u))
            continue;
        uint32_t imports = *(const uint32_t*)(base + pe + 24 + directories + 8);
        if (!imports)
            continue;
        for (const uint32_t* d = (const uint32_t*)(base + imports); d[3]; d += 5)
            for (void** slot = (void**)(base + d[4]); *slot; slot++) {
                if (*slot != real)
                    continue;
                DWORD old = 0;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    *slot = (void*)ourDoDragDrop;
                    VirtualProtect(slot, sizeof(void*), old, &old);
                    patched++;
                }
            }
    }
    if (tracing && patched)
        fprintf(stderr, "exbit-wine-host: %d imports of DoDragDrop caught\n", patched);
}

std::vector<std::string> takeDraggedFiles() {
    std::lock_guard<std::mutex> hold(lock);
    std::vector<std::string> out;
    out.swap(dragged);
    return out;
}

std::string dropFiles(HWND window, int x, int y, const std::vector<std::string>& files) {
    if (!window || files.empty())
        return "";
    std::vector<std::u16string> names;
    for (const auto& f : files) {
        WCHAR* dos = wine_get_dos_file_name(f.c_str());
        if (!dos)
            continue;
        names.push_back((const char16_t*)dos);
        HeapFree(GetProcessHeap(), 0, dos);
    }
    if (names.empty())
        return "";
    POINT screen{x, y};
    ClientToScreen(window, &screen);
    HWND under = windowAt(window, screen);
    // an OLE drop target: on the window under the point or one it is inside (up to ours)
    for (HWND h = under; h; h = h == window ? nullptr : GetParent(h)) {
        void* target = GetPropW(h, u"OleDropTargetInterface");
        if (!target)
            continue;
        auto* table = *(IDropTargetVtbl**)target;
        auto* data = new FileData{&fileDataTable, 1, names, POINT{x, y}};
        POINTL at{screen.x, screen.y};
        DWORD effect = DROPEFFECT_COPY;
        table->AddRef(target);
        bool taken = false;
        if (table->DragEnter(target, data, MK_LBUTTON, at, &effect) == S_OK && effect != DROPEFFECT_NONE) {
            effect = DROPEFFECT_COPY;
            table->DragOver(target, MK_LBUTTON, at, &effect);
            if (effect != DROPEFFECT_NONE) {
                effect = DROPEFFECT_COPY;
                taken = table->Drop(target, data, 0, at, &effect) == S_OK && effect != DROPEFFECT_NONE;
            } else {
                table->DragLeave(target);
            }
        } else {
            table->DragLeave(target);
        }
        table->Release(target);
        fdRelease(data);
        if (tracing)
            fprintf(stderr, "exbit-wine-host: %zu file(s) dropped on a drop target: %s\n", names.size(),
                    taken ? "taken" : "refused");
        return taken ? "ole" : "";
    }
    // else a window that accepts files (DragAcceptFiles): WM_DROPFILES, which it frees
    for (HWND h = under; h; h = h == window ? nullptr : GetParent(h)) {
        if (!(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_ACCEPTFILES))
            continue;
        POINT local = screen;
        ScreenToClient(h, &local);
        HGLOBAL drop = makeHdrop(names, local);
        if (drop && PostMessageW(h, WM_DROPFILES, (WPARAM)drop, 0))
            return "dropfiles";
        if (drop)
            GlobalFree(drop);
        return "";
    }
    return "";
}
