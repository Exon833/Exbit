#include "audio/rtkit.h"

#include <dlfcn.h>

#include <mutex>
#include <string>
#include <type_traits>

namespace realtime {

namespace {

// libdbus's types, as far as these calls need them (dbus/dbus-errors.h, dbus/dbus-message.h)
struct DBusErrorLayout {
    const char* name;
    const char* message;
    unsigned int flagBits;
    void* padding;
};
struct DBusIteratorStorage {  // (DBusMessageIter: opaque, 72 bytes on 64-bit; room to spare)
    alignas(8) unsigned char bytes[128];
};
using DBusBool = uint32_t;
constexpr int DBUS_BUS_SYSTEM = 1;
constexpr int DBUS_TYPE_INVALID = 0, DBUS_TYPE_INT32 = 'i', DBUS_TYPE_INT64 = 'x', DBUS_TYPE_UINT32 = 'u',
              DBUS_TYPE_UINT64 = 't', DBUS_TYPE_STRING = 's', DBUS_TYPE_VARIANT = 'v';
constexpr int REPLY_TIMEOUT_MS = 1000;

const char* const RTKIT_SERVICE = "org.freedesktop.RealtimeKit1";
const char* const RTKIT_PATH = "/org/freedesktop/RealtimeKit1";

// the libdbus functions used, loaded from libdbus-1.so.3
struct DBusLibrary {
    void* (*busGetPrivate)(int, DBusErrorLayout*) = nullptr;
    void (*setExitOnDisconnect)(void*, DBusBool) = nullptr;
    void* (*newMethodCall)(const char*, const char*, const char*, const char*) = nullptr;
    DBusBool (*appendArguments)(void*, int, ...) = nullptr;
    void* (*sendAndWaitForReply)(void*, void*, int, DBusErrorLayout*) = nullptr;
    void (*unreferenceMessage)(void*) = nullptr;
    void (*initError)(DBusErrorLayout*) = nullptr;
    void (*freeError)(DBusErrorLayout*) = nullptr;
    DBusBool (*startIterator)(void*, DBusIteratorStorage*) = nullptr;
    int (*argumentType)(DBusIteratorStorage*) = nullptr;
    void (*enterVariant)(DBusIteratorStorage*, DBusIteratorStorage*) = nullptr;
    void (*readBasicValue)(DBusIteratorStorage*, void*) = nullptr;
    bool loaded = false;
};

// loads libdbus once; false (with `error`) when it isn't installed
const DBusLibrary* dbusLibrary(std::string& error) {
    static DBusLibrary library = [] {
        DBusLibrary lib;
        void* handle = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!handle) return lib;
        auto find = [handle](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(handle, name));
            return function != nullptr;
        };
        lib.loaded = find(lib.busGetPrivate, "dbus_bus_get_private") &&
                     find(lib.setExitOnDisconnect, "dbus_connection_set_exit_on_disconnect") &&
                     find(lib.newMethodCall, "dbus_message_new_method_call") &&
                     find(lib.appendArguments, "dbus_message_append_args") &&
                     find(lib.sendAndWaitForReply, "dbus_connection_send_with_reply_and_block") &&
                     find(lib.unreferenceMessage, "dbus_message_unref") && find(lib.initError, "dbus_error_init") &&
                     find(lib.freeError, "dbus_error_free") && find(lib.startIterator, "dbus_message_iter_init") &&
                     find(lib.argumentType, "dbus_message_iter_get_arg_type") &&
                     find(lib.enterVariant, "dbus_message_iter_recurse") &&
                     find(lib.readBasicValue, "dbus_message_iter_get_basic");
        return lib;
    }();
    if (!library.loaded) error = "rtkit can't be reached: libdbus-1.so.3 isn't installed";
    return library.loaded ? &library : nullptr;
}

std::mutex busLock;    // (one call at a time on the one connection)
void* systemBus = nullptr;

// the system bus's connection, made the first time (busLock held); null with `error` when there is none
void* connectedBus(const DBusLibrary& dbus, std::string& error) {
    if (systemBus) return systemBus;
    DBusErrorLayout failure;
    dbus.initError(&failure);
    systemBus = dbus.busGetPrivate(DBUS_BUS_SYSTEM, &failure);
    if (systemBus) {
        dbus.setExitOnDisconnect(systemBus, 0);  // (libdbus would otherwise end the program if the bus goes away)
    } else {
        error = std::string("rtkit can't be reached: no system bus (") + (failure.message ? failure.message : "") + ")";
        dbus.freeError(&failure);
    }
    return systemBus;
}

// sends a call made by `fill` (busLock held) and hands its reply to `read`; false with `error` if it failed
template <typename Fill, typename Read>
bool callRtkit(const char* interfaceName, const char* method, Fill fill, Read read, std::string& error) {
    const DBusLibrary* dbus = dbusLibrary(error);
    if (!dbus) return false;
    std::lock_guard<std::mutex> hold(busLock);
    void* bus = connectedBus(*dbus, error);
    if (!bus) return false;
    void* message = dbus->newMethodCall(RTKIT_SERVICE, RTKIT_PATH, interfaceName, method);
    if (!message || !fill(*dbus, message)) {
        if (message) dbus->unreferenceMessage(message);
        error = std::string("rtkit: the call ") + method + " couldn't be made";
        return false;
    }
    DBusErrorLayout failure;
    dbus->initError(&failure);
    void* reply = dbus->sendAndWaitForReply(bus, message, REPLY_TIMEOUT_MS, &failure);
    dbus->unreferenceMessage(message);
    if (!reply) {
        error = std::string("rtkit refused (") + (failure.name ? failure.name : "") +
                (failure.message ? std::string(": ") + failure.message : "") + ")";
        dbus->freeError(&failure);
        return false;
    }
    bool ok = read(*dbus, reply);
    dbus->unreferenceMessage(reply);
    if (!ok) error = std::string("rtkit: an unexpected reply to ") + method;
    return ok;
}

// one of rtkit's properties, an integer (i or x) in a variant
bool readRtkitProperty(const char* name, int64_t& value, std::string& error) {
    return callRtkit(
        "org.freedesktop.DBus.Properties", "Get",
        [name](const DBusLibrary& dbus, void* message) {
            const char* interfaceName = RTKIT_SERVICE;
            return dbus.appendArguments(message, DBUS_TYPE_STRING, &interfaceName, DBUS_TYPE_STRING, &name,
                                        DBUS_TYPE_INVALID) != 0;
        },
        [&value](const DBusLibrary& dbus, void* reply) {
            DBusIteratorStorage outer, inner;
            if (!dbus.startIterator(reply, &outer) || dbus.argumentType(&outer) != DBUS_TYPE_VARIANT) return false;
            dbus.enterVariant(&outer, &inner);
            int type = dbus.argumentType(&inner);
            if (type == DBUS_TYPE_INT32) {
                int32_t small = 0;
                dbus.readBasicValue(&inner, &small);
                value = small;
                return true;
            }
            if (type != DBUS_TYPE_INT64) return false;
            dbus.readBasicValue(&inner, &value);
            return true;
        },
        error);
}

// a call that takes (process, thread, one 32-bit number) and answers nothing
bool callForThread(const char* method, int processId, long threadId, int argumentType, int32_t argument,
                   std::string& error) {
    return callRtkit(
        RTKIT_SERVICE, method,
        [&](const DBusLibrary& dbus, void* message) {
            uint64_t process = (uint64_t)processId, thread = (uint64_t)threadId;
            return dbus.appendArguments(message, DBUS_TYPE_UINT64, &process, DBUS_TYPE_UINT64, &thread, argumentType,
                                        &argument, DBUS_TYPE_INVALID) != 0;
        },
        [](const DBusLibrary&, void*) { return true; }, error);
}

}  // namespace

bool readRtkitLimits(RtkitLimits& limits, std::string& error) {
    static std::mutex cacheLock;
    static RtkitLimits cached;
    static bool known = false;  // (rtkit's limits don't change while it runs: read once)
    std::lock_guard<std::mutex> hold(cacheLock);
    int64_t priority = 0, nice = 0, microseconds = 0;
    if (!known) {
        if (!readRtkitProperty("MaxRealtimePriority", priority, error) || !readRtkitProperty("MinNiceLevel", nice, error) ||
            !readRtkitProperty("RTTimeUSecMax", microseconds, error))
            return false;
        cached = {(int)priority, (int)nice, microseconds};
        known = true;
    }
    limits = cached;
    return true;
}

bool rtkitMakeRealtime(int processId, long threadId, int priority, std::string& error) {
    return callForThread("MakeThreadRealtimeWithPID", processId, threadId, DBUS_TYPE_UINT32, priority, error);
}

bool rtkitMakeHighPriority(int processId, long threadId, int niceLevel, std::string& error) {
    return callForThread("MakeThreadHighPriorityWithPID", processId, threadId, DBUS_TYPE_INT32, niceLevel, error);
}

}  // namespace realtime
