// The JACK Audio Connection Kit's C client API, as far as Tiny DAW uses it (the "JACK" driver type,
// devices_jack.cpp).
//
// Declared here from the API's public behaviour (the functions a JACK client calls, their
// arguments, and the numbers of the flags and options passed), for interoperability with the JACK
// server and PipeWire's JACK. It is not JACK's own headers, which needn't be installed: the
// library, libjack.so.0, is loaded with dlopen when JACK is first asked for and is never linked, so
// the program starts and runs where JACK isn't installed at all. Only what Tiny DAW calls is here.
#pragma once
#include <cstdint>

namespace jack {

// --- types ---
struct Client;  // jack_client_t (opaque)
struct Port;    // jack_port_t (opaque)
using nframes_t = uint32_t;
using sample_t = float;  // jack_default_audio_sample_t

// jack_options_t
enum Options : int {
    NullOption = 0x00,
    NoStartServer = 0x01,   // don't start a server when none runs
    UseExactName = 0x02,
    ServerName = 0x04,      // (a server name follows as a vararg)
};

// jack_status_t (bits)
enum Status : int {
    Failure = 0x01,
    InvalidOption = 0x02,
    NameNotUnique = 0x04,
    ServerStarted = 0x08,
    ServerFailed = 0x10,
    ServerError = 0x20,
    NoSuchClient = 0x40,
    LoadFailure = 0x80,
    InitFailure = 0x100,
    ShmFailure = 0x200,
    VersionError = 0x400,
    BackendError = 0x800,
    ClientZombie = 0x1000,
};

// JackPortFlags
enum PortFlags : unsigned long {
    PortIsInput = 0x1,     // receives data (a playback port of the system is an input)
    PortIsOutput = 0x2,    // sends data (a capture port of the system is an output)
    PortIsPhysical = 0x4,  // the hardware's own
    PortCanMonitor = 0x8,
    PortIsTerminal = 0x10,
};

// jack_latency_callback_mode_t
enum LatencyMode : int { CaptureLatency = 0, PlaybackLatency = 1 };

struct LatencyRange {  // jack_latency_range_t (frames)
    nframes_t min;
    nframes_t max;
};

constexpr const char* DEFAULT_AUDIO_TYPE = "32 bit float mono audio";  // JACK_DEFAULT_AUDIO_TYPE

// --- callbacks (all on JACK's threads: the process one on its real-time thread) ---
using ProcessCallback = int (*)(nframes_t frames, void* arg);            // 0 = go on
using XRunCallback = int (*)(void* arg);
using ShutdownCallback = void (*)(void* arg);
using InfoShutdownCallback = void (*)(int status, const char* reason, void* arg);
using BufferSizeCallback = int (*)(nframes_t frames, void* arg);
using ErrorFunction = void (*)(const char* msg);

// --- the functions (C linkage in libjack.so.0; the names are those in the comments) ---
struct Api {
    Client* (*client_open)(const char* name, int options, int* status, ...);  // jack_client_open
    int (*client_close)(Client*);                                              // jack_client_close
    const char* (*get_client_name)(Client*);                                   // jack_get_client_name
    int (*activate)(Client*);                                                  // jack_activate
    int (*deactivate)(Client*);                                                // jack_deactivate
    int (*set_process_callback)(Client*, ProcessCallback, void* arg);          // jack_set_process_callback
    int (*set_xrun_callback)(Client*, XRunCallback, void* arg);                // jack_set_xrun_callback
    void (*on_shutdown)(Client*, ShutdownCallback, void* arg);                 // jack_on_shutdown
    void (*on_info_shutdown)(Client*, InfoShutdownCallback, void* arg);        // jack_on_info_shutdown (may be missing)
    int (*set_buffer_size_callback)(Client*, BufferSizeCallback, void* arg);   // jack_set_buffer_size_callback
    nframes_t (*get_sample_rate)(Client*);                                     // jack_get_sample_rate
    nframes_t (*get_buffer_size)(Client*);                                     // jack_get_buffer_size
    Port* (*port_register)(Client*, const char* name, const char* type, unsigned long flags,
                           unsigned long bufferSize);                          // jack_port_register
    int (*port_unregister)(Client*, Port*);                                    // jack_port_unregister
    void* (*port_get_buffer)(Port*, nframes_t frames);                         // jack_port_get_buffer
    const char* (*port_name)(const Port*);                                     // jack_port_name
    int (*connect)(Client*, const char* source, const char* destination);      // jack_connect
    const char** (*get_ports)(Client*, const char* namePattern, const char* typePattern,
                              unsigned long flags);                            // jack_get_ports (free with free_)
    void (*free_)(void* p);                                                    // jack_free
    void (*port_get_latency_range)(Port*, int mode, LatencyRange* range);      // jack_port_get_latency_range
    void (*set_error_function)(ErrorFunction);                                 // jack_set_error_function
    void (*set_info_function)(ErrorFunction);                                  // jack_set_info_function
};

// libjack.so.0's functions (loaded on the first call, kept loaded), or nullptr when the library
// isn't there or lacks one of them. JACK's own messages to stderr ("Cannot connect to server
// socket…" while no server runs) are silenced.
const Api* api();
// (tests) use these functions instead of libjack's; nullptr goes back to libjack
void setApiForTesting(const Api* api);

}  // namespace jack
