// libasound, the ALSA functions Tiny DAW calls (audio/devices_alsa.cpp, audio/midi_in.cpp): loaded with dlopen
// the first time a device or MIDI port is looked for (alsa::load), never linked, so the program starts where
// it isn't installed (then: no ALSA devices or MIDI inputs, and PulseAudio or JACK if they are there). As
// platform/x11_api.h: the real header gives the types; each pointer has its function's name, and the files
// calling them bring the names into their own namespace (TINYDAW_ALSA(TINYDAW_ALSA_USE)), where they hide the
// header's declarations. A function newly called must be added to the list (the link fails until it is).
#pragma once
#include <string>

#include <alsa/asoundlib.h>

// (the *_alloca macros call the *_sizeof functions)
#define TINYDAW_ALSA(F) \
    F(snd_lib_error_set_handler) F(snd_strerror) F(snd_device_name_hint) F(snd_device_name_get_hint) \
    F(snd_device_name_free_hint) \
    F(snd_pcm_open) F(snd_pcm_close) F(snd_pcm_start) F(snd_pcm_prepare) F(snd_pcm_drop) F(snd_pcm_resume) \
    F(snd_pcm_link) F(snd_pcm_writei) F(snd_pcm_readi) F(snd_pcm_wait) F(snd_pcm_avail_update) F(snd_pcm_forward) \
    F(snd_pcm_hw_params_sizeof) F(snd_pcm_hw_params) F(snd_pcm_hw_params_any) F(snd_pcm_hw_params_set_access) \
    F(snd_pcm_hw_params_set_format) F(snd_pcm_hw_params_test_format) F(snd_pcm_hw_params_test_rate) \
    F(snd_pcm_hw_params_set_rate_near) F(snd_pcm_hw_params_get_rate_min) F(snd_pcm_hw_params_set_channels_near) \
    F(snd_pcm_hw_params_get_channels_max) F(snd_pcm_hw_params_set_period_size_near) \
    F(snd_pcm_hw_params_get_period_size) F(snd_pcm_hw_params_set_buffer_size_near) \
    F(snd_pcm_hw_params_get_buffer_size) \
    F(snd_pcm_sw_params_sizeof) F(snd_pcm_sw_params) F(snd_pcm_sw_params_current) \
    F(snd_pcm_sw_params_set_start_threshold) F(snd_pcm_sw_params_set_avail_min) \
    F(snd_seq_open) F(snd_seq_close) F(snd_seq_client_id) F(snd_seq_set_client_name) F(snd_seq_create_simple_port) \
    F(snd_seq_connect_from) F(snd_seq_event_input) F(snd_seq_poll_descriptors_count) F(snd_seq_poll_descriptors) \
    F(snd_seq_query_next_client) F(snd_seq_query_next_port) \
    F(snd_seq_client_info_sizeof) F(snd_seq_client_info_set_client) F(snd_seq_client_info_get_client) \
    F(snd_seq_client_info_get_name) \
    F(snd_seq_port_info_sizeof) F(snd_seq_port_info_set_client) F(snd_seq_port_info_set_port) \
    F(snd_seq_port_info_get_client) F(snd_seq_port_info_get_port) F(snd_seq_port_info_get_name) \
    F(snd_seq_port_info_get_type) F(snd_seq_port_info_get_capability)

namespace alsa {
#define TINYDAW_ALSA_DECLARE(f) extern decltype(&::f) f;
TINYDAW_ALSA(TINYDAW_ALSA_DECLARE)
#undef TINYDAW_ALSA_DECLARE
// libasound.so.2 and every function found (once; later calls give the first answer); false, and why, if not
bool load(std::string& why);
}  // namespace alsa

#define TINYDAW_ALSA_USE(f) using alsa::f;
