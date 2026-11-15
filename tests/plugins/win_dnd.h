// Drag and drop for the Windows builds of the test plugins (make win-plugins; test_wine): a drag of
// one file out of a plugin's window (DoDragDrop with a CF_HDROP data object, as JUCE plugins drag MIDI
// out), and what a drop on it is answered with: the dropped file gets a sibling "<file>.seen-<tag>"
// (so the test sees it arrived without a parameter of its own). Windows only.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>  // (DROPFILES)
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace win_dnd {

// the files of a CF_HDROP block
inline std::vector<std::wstring> filesIn(HDROP drop) {
    std::vector<std::wstring> out;
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; i++) {
        wchar_t name[MAX_PATH * 2];
        if (DragQueryFileW(drop, i, name, MAX_PATH * 2)) out.push_back(name);
    }
    return out;
}

// "<file>.seen-<tag>": the drop arrived
inline void markSeen(const std::vector<std::wstring>& files, const wchar_t* tag) {
    for (const auto& f : files) {
        std::wstring mark = f + L".seen-" + tag;
        if (FILE* out = _wfopen(mark.c_str(), L"wb")) fclose(out);
    }
}

// a data object holding one file as CF_HDROP
class FileData final : public IDataObject {
public:
    explicit FileData(const std::wstring& file) : file_(file) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *object = this;
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override {
        if (QueryGetData(f) != S_OK) return DV_E_FORMATETC;
        size_t bytes = sizeof(DROPFILES) + (file_.size() + 2) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        auto* df = (DROPFILES*)GlobalLock(g);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(df + 1, file_.c_str(), file_.size() * sizeof(wchar_t));
        GlobalUnlock(g);
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = g;
        m->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
        return f && f->cfFormat == CF_HDROP && (f->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC** e) override {
        if (e) *e = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::wstring file_;
    ULONG refs_ = 1;
};

class Source final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *object = this;
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        return escape ? DRAGDROP_S_CANCEL : (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG refs_ = 1;
};

// drags out a small MIDI file it writes in %TEMP% (as a chord tool would): what DoDragDrop said
inline HRESULT dragMidiOut(const wchar_t* name) {
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return E_FAIL;
    std::wstring file = std::wstring(temp) + name;
    if (FILE* out = _wfopen(file.c_str(), L"wb")) {
        static const unsigned char midi[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
                                             'M', 'T', 'r', 'k', 0, 0, 0, 4, 0, 0xFF, 0x2F, 0};
        fwrite(midi, 1, sizeof(midi), out);
        fclose(out);
    }
    OleInitialize(nullptr);
    auto* data = new FileData(file);
    auto* source = new Source();
    DWORD effect = DROPEFFECT_NONE;
    HRESULT r = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    data->Release();
    source->Release();
    return r;
}

// a window's OLE drop target: marks the files dropped on it
class Target final : public IDropTarget {
public:
    explicit Target(const wchar_t* tag) : tag_(tag) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = this;
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        *effect = data && data->QueryGetData(&f) == S_OK ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM m{};
        *effect = DROPEFFECT_NONE;
        if (data && data->GetData(&f, &m) == S_OK) {
            markSeen(filesIn((HDROP)m.hGlobal), tag_);
            ReleaseStgMedium(&m);
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

private:
    const wchar_t* tag_;
    ULONG refs_ = 1;
};

}  // namespace win_dnd
