////////////////////////////////////////////////////////////////////////////////
//
// WavPack compression wrapper for ecm3
// Copyright (C) 2026 Edward Sloter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <vector>

#define WAVPACKZLIB_EXTREME_COMPRESSION (uint8_t(1) << 7)

enum wavpackzlib_flush_mode : uint8_t {
    WAVPACKZLIB_NO_FLUSH = 0,
    WAVPACKZLIB_PARTIAL_FLUSH,
    WAVPACKZLIB_SYNC_FLUSH,
    WAVPACKZLIB_FULL_FLUSH,
    WAVPACKZLIB_FINISH,
    WAVPACKZLIB_BLOCK,
};

enum wavpackzlib_return_code {
    WAVPACKZLIB_RC_OK = 0,
    WAVPACKZLIB_RC_NO_INITIALIZED = INT_MIN,
    WAVPACKZLIB_RC_INITIALIZATION_ERROR,
    WAVPACKZLIB_RC_COMPRESSION_ERROR,
    WAVPACKZLIB_RC_DECOMPRESSION_ERROR,
    WAVPACKZLIB_RC_METADATA_ERROR,
    WAVPACKZLIB_RC_BUFFER_ERROR,
    WAVPACKZLIB_RC_WRONG_OPTIONS,
    WAVPACKZLIB_RC_END_OF_STREAM,
    WAVPACKZLIB_RC_CLOSED
};

struct wavpackzlib_stream {
    uint8_t* next_in;
    size_t   avail_in;
    uint8_t* next_out;
    size_t   avail_out;

    uint8_t* decompress_buffer_data = nullptr;
    size_t   decompress_buffer_size = 0;
    size_t   decompress_buffer_size_real = 0;
    size_t   decompress_buffer_index = 0;

    char* msg = nullptr;
    wavpackzlib_return_code status = WAVPACKZLIB_RC_NO_INITIALIZED;
};

class wavpackzlib {
public:
    wavpackzlib(
        bool arg_is_compressor,
        uint8_t arg_channels = 2,
        uint8_t arg_bits_per_sample = 16,
        uint32_t arg_sample_rate = 44100,
        size_t arg_block_size = 0,
        int8_t arg_compression_level = 5
    );
    ~wavpackzlib();
    int compress(uint32_t samples, wavpackzlib_flush_mode flush_mode);
    int decompress();
    int decompress_partial(bool reset, long long seek_to = -1);
    wavpackzlib_return_code get_status() { return strm->status; }
    void close();
    wavpackzlib_stream* strm = nullptr;

    std::vector<uint8_t> overflow;

    struct {
        const uint8_t* next_in;
        size_t avail_in;
        size_t total_size;
        size_t current_pos;
    } input_state;

    int push_back_char = -1;

private:
    bool is_compressor;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint32_t sample_rate;
    int8_t compression_level;
    bool extreme_mode;
    void* wpc = nullptr;

    friend int32_t wp_block_output(void* id, void* data, int32_t bcount);
    friend int32_t wp_stream_read(void* id, void* data, int32_t bcount);
    friend int wp_stream_seek_abs(void* id, int64_t pos);
    friend int64_t wp_stream_tell(void* id);
    friend int64_t wp_stream_len(void* id);
    friend int wp_stream_can_seek(void*);
};