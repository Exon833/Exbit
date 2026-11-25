// Xlib and the X extensions Exbit calls (platform/x11.cpp, plugins/bridged.cpp): loaded with dlopen
// when the window system starts (xlib::load), never linked, so the program starts where X11 isn't
// installed at all (exbit --tui on a text console). The real headers give the functions' types; each
// pointer here has the name of the function it stands for, and the files that call them bring the names
// into their own namespace (EXBIT_X11(EXBIT_X11_USE)), where they hide the headers' declarations:
// the calls are written as ever. A function newly called must be added to the list (the link fails until
// it is: it would be an undefined reference to libX11).
#pragma once
#include <string>

#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>
#undef min  // Xlibint.h's
#undef max

// F(library, function): the library is one of X11 (libX11.so.6), Xext, Xrandr, Xcursor
#define EXBIT_X11(F) \
    F(X11, _XGetRequest) F(X11, _XReply) F(X11, XAllocSizeHints) F(X11, XBell) F(X11, XChangeProperty) \
    F(X11, XCheckIfEvent) F(X11, XCheckTypedEvent) F(X11, XCheckTypedWindowEvent) F(X11, XCloseDisplay) \
    F(X11, XCloseIM) F(X11, XConvertSelection) F(X11, XCreateBitmapFromData) F(X11, XCreateColormap) \
    F(X11, XCreateFontCursor) F(X11, XCreateGC) F(X11, XCreateIC) F(X11, XCreateImage) F(X11, XCreatePixmapCursor) \
    F(X11, XCreateWindow) F(X11, XDefineCursor) F(X11, XDeleteProperty) F(X11, XDestroyIC) F(X11, XDestroyWindow) \
    F(X11, XDisplayKeycodes) F(X11, XEventsQueued) F(X11, XExtendedMaxRequestSize) F(X11, XFilterEvent) \
    F(X11, XFlush) F(X11, XFree) F(X11, XFreeColormap) F(X11, XFreeCursor) F(X11, XFreeGC) F(X11, XFreePixmap) \
    F(X11, XGetErrorText) F(X11, XGetGeometry) F(X11, XGetICValues) F(X11, XGetImage) F(X11, XGetIMValues) \
    F(X11, XGetInputFocus) F(X11, XGetSelectionOwner) F(X11, XGetVisualInfo) F(X11, XGetWindowAttributes) \
    F(X11, XGetWindowProperty) F(X11, XGrabKey) F(X11, XGrabKeyboard) F(X11, XGrabPointer) F(X11, XIconifyWindow) \
    F(X11, XInitThreads) F(X11, XInternAtom) F(X11, XInternAtoms) F(X11, XkbKeycodeToKeysym) \
    F(X11, XkbSetDetectableAutoRepeat) F(X11, XLookupKeysym) F(X11, XLookupString) F(X11, XMapRaised) \
    F(X11, XMapWindow) F(X11, XMatchVisualInfo) F(X11, XMaxRequestSize) F(X11, XMoveResizeWindow) \
    F(X11, XMoveWindow) F(X11, XNextEvent) F(X11, XOpenDisplay) F(X11, XOpenIM) F(X11, XPeekEvent) F(X11, XPending) \
    F(X11, XPutImage) F(X11, XQueryExtension) F(X11, XQueryPointer) F(X11, XQueryTree) F(X11, XRaiseWindow) \
    F(X11, XRefreshKeyboardMapping) F(X11, XReparentWindow) F(X11, XResizeWindow) F(X11, XSelectInput) \
    F(X11, XSendEvent) F(X11, XSetErrorHandler) F(X11, XSetGraphicsExposures) F(X11, XSetICFocus) \
    F(X11, XSetInputFocus) F(X11, XSetIOErrorHandler) F(X11, XSetLocaleModifiers) F(X11, XSetSelectionOwner) \
    F(X11, XSetTransientForHint) F(X11, XSetWMName) F(X11, XSetWMNormalHints) F(X11, XSetWMProtocols) \
    F(X11, XSupportsLocale) F(X11, XSync) F(X11, XTranslateCoordinates) F(X11, XUngrabKey) F(X11, XUngrabKeyboard) \
    F(X11, XUngrabPointer) F(X11, XUnmapWindow) F(X11, XUnsetICFocus) F(X11, Xutf8LookupString) \
    F(X11, Xutf8SetWMProperties) F(X11, Xutf8TextListToTextProperty) F(X11, XWithdrawWindow) \
    F(Xext, XShapeCombineMask) F(Xext, XShapeCombineRectangles) F(Xext, XShmAttach) F(Xext, XShmCreateImage) \
    F(Xext, XShmDetach) F(Xext, XShmGetEventBase) F(Xext, XShmPutImage) F(Xext, XShmQueryExtension) \
    F(Xext, XShmQueryVersion) \
    F(Xrandr, XRRFreeCrtcInfo) F(Xrandr, XRRFreeMonitors) F(Xrandr, XRRFreeScreenResources) \
    F(Xrandr, XRRGetCrtcInfo) F(Xrandr, XRRGetMonitors) F(Xrandr, XRRGetScreenResourcesCurrent) \
    F(Xrandr, XRRQueryExtension) \
    F(Xcursor, XcursorLibraryLoadCursor)

namespace xlib {
#define EXBIT_X11_DECLARE(lib, f) extern decltype(&::f) f;
EXBIT_X11(EXBIT_X11_DECLARE)
#undef EXBIT_X11_DECLARE
// every library and function found (once; later calls give the first answer); false, and why, if not
bool load(std::string& why);
}  // namespace xlib

#define EXBIT_X11_USE(lib, f) using xlib::f;
