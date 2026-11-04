// The emulated FM-1 (rust/fm1-ffi): fm1-emu's machine thread, resampler and saves behind a C API.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C"
{
    struct Fm1;

    struct Fm1Status
    {
        int32_t booted;
        int32_t halted;
        int32_t board; // 0 FM-1, 1 SMK-37
        float speed;   // how much faster than real time the machine could run
        uint64_t late; // frames the host found missing in the last second
    };

    struct Fm1Buf
    {
        uint8_t* data;
        size_t len;
    };

    Fm1* fm1_new();
    void fm1_free (Fm1*);

    // audio thread
    int32_t fm1_prepare (const Fm1*, double sampleRate, int32_t maxBlock); // returns latency (samples)
    void fm1_midi (const Fm1*, int32_t offset, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t len);
    void fm1_process (const Fm1*, float* left, float* right, int32_t n);

    // controls
    void fm1_key (const Fm1*, int32_t id, int32_t down);
    void fm1_enc (const Fm1*, int32_t enc, int32_t clicks);
    void fm1_set_master (const Fm1*, float v);
    void fm1_set_clock (const Fm1*, int32_t mhz);

    // display
    void fm1_view (const Fm1*, uint32_t* argb240x240, float* leds41, Fm1Status* status);
    void fm1_firmware_name (const Fm1*, char* buf, size_t cap);
    int32_t fm1_has_firmware (const Fm1*);

    // files (blocking: background thread)
    int32_t fm1_load_file (const Fm1*, const char* path, char* msg, size_t cap);
    int32_t fm1_export_file (const Fm1*, const char* path, char* msg, size_t cap);

    // project state (a portable .fm1save)
    Fm1Buf fm1_get_state (const Fm1*);
    void fm1_buf_free (Fm1Buf);
    int32_t fm1_set_state (const Fm1*, const uint8_t* data, size_t len);
}
