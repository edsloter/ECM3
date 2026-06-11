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

#include "wavpackzlib.h"
#include <wavpack.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

int32_t wp_block_output(void* id, void* data, int32_t bcount) {
    wavpackzlib* self = static_cast<wavpackzlib*>(id);
    if (bcount <= 0 || !data) return 0;

    size_t to_write = static_cast<size_t>(bcount);
    size_t avail = self->strm->avail_out;

    if (to_write <= avail) {
        memcpy(self->strm->next_out, data, to_write);
        self->strm->next_out += to_write;
        self->strm->avail_out -= to_write;
    } else {
        memcpy(self->strm->next_out, data, avail);
        self->strm->next_out += avail;
        self->strm->avail_out = 0;

        size_t remainder = to_write - avail;
        size_t old_size = self->overflow.size();
        self->overflow.resize(old_size + remainder);
        memcpy(self->overflow.data() + old_size, static_cast<const uint8_t*>(data) + avail, remainder);
    }
    return bcount;
}

int32_t wp_stream_read(void* id, void* data, int32_t bcount) {
    wavpackzlib* self = static_cast<wavpackzlib*>(id);
    if (bcount <= 0) return 0;

    size_t written = 0;
    uint8_t* out = static_cast<uint8_t*>(data);

    if (self->push_back_char >= 0 && bcount > 0) {
        out[0] = static_cast<uint8_t>(self->push_back_char);
        self->push_back_char = -1;
        written = 1;
        self->input_state.current_pos++;
        if (written >= static_cast<size_t>(bcount))
            return static_cast<int32_t>(written);
    }

    int32_t to_read = static_cast<int32_t>(static_cast<size_t>(bcount) - written);
    if (static_cast<size_t>(to_read) > self->input_state.avail_in) {
        to_read = static_cast<int32_t>(self->input_state.avail_in);
    }
    if (to_read > 0) {
        memcpy(out + written, self->input_state.next_in, to_read);
        self->input_state.next_in += to_read;
        self->input_state.avail_in -= to_read;
        self->input_state.current_pos += to_read;
        written += to_read;
    }
    return static_cast<int32_t>(written);
}

int wp_stream_seek_abs(void* id, int64_t pos) {
    wavpackzlib* self = static_cast<wavpackzlib*>(id);
    if (pos < 0) return -1;
    self->push_back_char = -1;
    if (static_cast<uint64_t>(pos) > self->input_state.current_pos) {
        uint64_t forward = static_cast<uint64_t>(pos) - self->input_state.current_pos;
        if (forward <= self->input_state.avail_in) {
            self->input_state.next_in += forward;
            self->input_state.avail_in -= forward;
            self->input_state.current_pos += forward;
        } else {
            return -1;
        }
    } else if (static_cast<uint64_t>(pos) < self->input_state.current_pos) {
        uint64_t backward = self->input_state.current_pos - static_cast<uint64_t>(pos);
        if (backward <= (self->input_state.total_size - self->input_state.avail_in)) {
            size_t consumed = self->input_state.total_size - self->input_state.avail_in;
            if (backward <= consumed) {
                self->input_state.next_in -= backward;
                self->input_state.avail_in += backward;
                self->input_state.current_pos -= backward;
            } else {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

int64_t wp_stream_tell(void* id) {
    wavpackzlib* self = static_cast<wavpackzlib*>(id);
    return static_cast<int64_t>(self->input_state.current_pos);
}

int64_t wp_stream_len(void* id) {
    wavpackzlib* self = static_cast<wavpackzlib*>(id);
    return static_cast<int64_t>(self->input_state.total_size);
}

int wp_stream_can_seek(void* id) {
    (void)id;
    return 0;
}

int wp_set_pos_rel(void* id, int64_t delta, int mode) {
    wavpackzlib* self = static_cast<wavpackzlib*>(id);
    int64_t new_pos;
    if (mode == SEEK_CUR) {
        new_pos = static_cast<int64_t>(self->input_state.current_pos) + delta;
    } else if (mode == SEEK_END) {
        new_pos = static_cast<int64_t>(self->input_state.total_size) + delta;
    } else {
        return -1;
    }
    if (new_pos < 0) return -1;
    return wp_stream_seek_abs(id, new_pos);
}

int wp_push_back_byte(void* id, int c) {
    wavpackzlib* self = static_cast<wavpackzlib*>(id);
    self->push_back_char = c;
    self->input_state.current_pos--;
    return c;
}

static WavpackStreamReader64 wp_reader = {
    wp_stream_read,
    nullptr,
    wp_stream_tell,
    wp_stream_seek_abs,
    wp_set_pos_rel,
    wp_push_back_byte,
    wp_stream_len,
    wp_stream_can_seek,
    nullptr,
    nullptr
};

wavpackzlib::wavpackzlib(
    bool arg_is_compressor,
    uint8_t arg_channels,
    uint8_t arg_bits_per_sample,
    uint32_t arg_sample_rate,
    size_t,
    int8_t arg_compression_level
) : is_compressor(arg_is_compressor),
    channels(arg_channels),
    bits_per_sample(arg_bits_per_sample),
    sample_rate(arg_sample_rate),
    compression_level(arg_compression_level & 0x7F),
    extreme_mode((arg_compression_level & WAVPACKZLIB_EXTREME_COMPRESSION) != 0),
    strm(nullptr),
    wpc(nullptr) {
    strm = new wavpackzlib_stream();
    memset(strm, 0, sizeof(wavpackzlib_stream));
    strm->status = WAVPACKZLIB_RC_NO_INITIALIZED;
}

wavpackzlib::~wavpackzlib() {
    close();
    delete strm;
}

int wavpackzlib::compress(uint32_t samples, wavpackzlib_flush_mode flush_mode) {
    if (!is_compressor) {
        strm->status = WAVPACKZLIB_RC_WRONG_OPTIONS;
        return WAVPACKZLIB_RC_WRONG_OPTIONS;
    }

    if (!wpc) {
        WavpackConfig config;
        memset(&config, 0, sizeof(config));
        config.num_channels = channels;
        config.bits_per_sample = bits_per_sample;
        config.sample_rate = sample_rate;
        config.bytes_per_sample = (bits_per_sample + 7) / 8;
        config.block_samples = 0;

        // Map -c N (0-9) to WavPack quality modes:
        //   0-1: Fast          (CONFIG_FAST_FLAG)         — less CPU, slightly worse ratio
        //   2-4: Normal        (no flags)                 — balanced default
        //   5-7: High          (CONFIG_HIGH_FLAG)         — more CPU, better ratio
        //   8-9: Very High     (CONFIG_HIGH_FLAG|VERY_HIGH_FLAG) — best ratio, most CPU
        //
        // -e adds CONFIG_EXTRA_MODE (extra decorrelation processing pass).
        // The xmode value controls its aggressiveness:
        //   1 = fast extra,  3 = normal extra,  6 = high extra
        int level = compression_level;
        if (level >= 0 && level <= 1) {
            config.flags |= CONFIG_FAST_FLAG;
        } else if (level >= 5 && level <= 7) {
            config.flags |= CONFIG_HIGH_FLAG;
        } else if (level >= 8) {
            config.flags |= CONFIG_HIGH_FLAG;
            config.flags |= CONFIG_VERY_HIGH_FLAG;
        }
        if (extreme_mode) {
            config.flags |= CONFIG_EXTRA_MODE;
            if (level <= 1) {
                config.xmode = 1;
            } else if (level <= 4) {
                config.xmode = 3;
            } else {
                config.xmode = 6;
            }
        }

        // Safety: never enable hybrid mode. Hybrid mode produces lossy output
        // which is incompatible with our bit-identical roundtrip requirement.
        config.flags &= ~CONFIG_HYBRID_FLAG;

        wpc = WavpackOpenFileOutput(wp_block_output, this, nullptr);
        if (!wpc) {
            strm->status = WAVPACKZLIB_RC_INITIALIZATION_ERROR;
            return WAVPACKZLIB_RC_INITIALIZATION_ERROR;
        }

        if (!WavpackSetConfiguration64(static_cast<WavpackContext*>(wpc), &config, -1, nullptr)) {
            strm->status = WAVPACKZLIB_RC_INITIALIZATION_ERROR;
            return WAVPACKZLIB_RC_INITIALIZATION_ERROR;
        }

        if (!WavpackPackInit(static_cast<WavpackContext*>(wpc))) {
            strm->status = WAVPACKZLIB_RC_INITIALIZATION_ERROR;
            return WAVPACKZLIB_RC_INITIALIZATION_ERROR;
        }

        strm->status = WAVPACKZLIB_RC_OK;
    }

    if (!overflow.empty()) {
        size_t to_copy = (overflow.size() < strm->avail_out) ? overflow.size() : strm->avail_out;
        memcpy(strm->next_out, overflow.data(), to_copy);
        strm->next_out += to_copy;
        strm->avail_out -= to_copy;
        if (to_copy < overflow.size()) {
            size_t remainder = overflow.size() - to_copy;
            memmove(overflow.data(), overflow.data() + to_copy, remainder);
            overflow.resize(remainder);
            return 0;
        }
        overflow.clear();
    }

    if (samples == 0 || strm->avail_in == 0) {
        if (flush_mode == WAVPACKZLIB_FINISH) {
            if (!wpc) {
                WavpackConfig config;
                memset(&config, 0, sizeof(config));
                config.num_channels = channels;
                config.bits_per_sample = bits_per_sample;
                config.sample_rate = sample_rate;
                config.bytes_per_sample = (bits_per_sample + 7) / 8;
                config.block_samples = 0;

                int level = compression_level;
                if (level >= 0 && level <= 1) {
                    config.flags |= CONFIG_FAST_FLAG;
                } else if (level >= 5 && level <= 7) {
                    config.flags |= CONFIG_HIGH_FLAG;
                } else if (level >= 8) {
                    config.flags |= CONFIG_HIGH_FLAG;
                    config.flags |= CONFIG_VERY_HIGH_FLAG;
                }
                if (extreme_mode) {
                    config.flags |= CONFIG_EXTRA_MODE;
                    config.xmode = (level <= 1) ? 1 : (level <= 4) ? 3 : 6;
                }
                config.flags &= ~CONFIG_HYBRID_FLAG;

                wpc = WavpackOpenFileOutput(wp_block_output, this, nullptr);
                if (!wpc) {
                    strm->status = WAVPACKZLIB_RC_INITIALIZATION_ERROR;
                    return WAVPACKZLIB_RC_INITIALIZATION_ERROR;
                }
                WavpackSetConfiguration64(static_cast<WavpackContext*>(wpc), &config, -1, nullptr);
                if (!WavpackPackInit(static_cast<WavpackContext*>(wpc))) {
                    strm->status = WAVPACKZLIB_RC_INITIALIZATION_ERROR;
                    return WAVPACKZLIB_RC_INITIALIZATION_ERROR;
                }
            }
            WavpackFlushSamples(static_cast<WavpackContext*>(wpc));
            WavpackCloseFile(static_cast<WavpackContext*>(wpc));
            wpc = nullptr;

            if (!overflow.empty()) {
                size_t to_copy = (overflow.size() < strm->avail_out) ? overflow.size() : strm->avail_out;
                memcpy(strm->next_out, overflow.data(), to_copy);
                strm->next_out += to_copy;
                strm->avail_out -= to_copy;
                if (to_copy < overflow.size()) {
                    size_t remainder = overflow.size() - to_copy;
                    memmove(overflow.data(), overflow.data() + to_copy, remainder);
                    overflow.resize(remainder);
                } else {
                    overflow.clear();
                }
            }
            strm->status = WAVPACKZLIB_RC_CLOSED;
        }
        return 0;
    }

    size_t bytes_per_frame = channels * (bits_per_sample / 8);
    uint32_t sample_frames = static_cast<uint32_t>(strm->avail_in / bytes_per_frame);
    if (samples > 0 && samples < sample_frames) {
        sample_frames = samples;
    }
    if (sample_frames == 0) return 0;

    std::vector<int32_t> decoded(sample_frames * channels);
    const uint8_t* src = strm->next_in;

    if (bits_per_sample == 16) {
        for (uint32_t i = 0; i < sample_frames * channels; i++) {
            decoded[i] = static_cast<int16_t>(
                static_cast<uint16_t>(src[2 * i]) |
                (static_cast<uint16_t>(src[2 * i + 1]) << 8)
            );
        }
    } else if (bits_per_sample == 8) {
        for (uint32_t i = 0; i < sample_frames * channels; i++) {
            decoded[i] = static_cast<int8_t>(src[i]);
        }
    } else {
        size_t bps = bits_per_sample / 8;
        for (uint32_t i = 0; i < sample_frames * channels; i++) {
            int32_t val = 0;
            for (size_t b = 0; b < bps; b++) {
                val |= static_cast<int32_t>(src[i * bps + b]) << (8 * b);
            }
            if (bits_per_sample <= 24) {
                val = static_cast<int32_t>(static_cast<uint32_t>(val) << (32 - bits_per_sample)) >> (32 - bits_per_sample);
            }
            decoded[i] = val;
        }
    }

    size_t bytes_consumed = static_cast<size_t>(sample_frames) * bytes_per_frame;
    strm->next_in += bytes_consumed;
    strm->avail_in -= bytes_consumed;

    if (!WavpackPackSamples(static_cast<WavpackContext*>(wpc), decoded.data(), sample_frames)) {
        strm->status = WAVPACKZLIB_RC_COMPRESSION_ERROR;
        return WAVPACKZLIB_RC_COMPRESSION_ERROR;
    }

    if (flush_mode == WAVPACKZLIB_FINISH) {
        WavpackFlushSamples(static_cast<WavpackContext*>(wpc));
        WavpackCloseFile(static_cast<WavpackContext*>(wpc));
        wpc = nullptr;
    }

    if (!overflow.empty()) {
        size_t to_copy = (overflow.size() < strm->avail_out) ? overflow.size() : strm->avail_out;
        memcpy(strm->next_out, overflow.data(), to_copy);
        strm->next_out += to_copy;
        strm->avail_out -= to_copy;
        if (to_copy < overflow.size()) {
            size_t remainder = overflow.size() - to_copy;
            memmove(overflow.data(), overflow.data() + to_copy, remainder);
            overflow.resize(remainder);
        } else {
            overflow.clear();
        }
    }

    strm->status = (flush_mode == WAVPACKZLIB_FINISH) ? WAVPACKZLIB_RC_CLOSED : WAVPACKZLIB_RC_OK;
    return 0;
}

int wavpackzlib::decompress() {
    return decompress_partial(true, -1);
}

int wavpackzlib::decompress_partial(bool, long long) {
    if (is_compressor) {
        strm->status = WAVPACKZLIB_RC_WRONG_OPTIONS;
        return WAVPACKZLIB_RC_WRONG_OPTIONS;
    }

    size_t orig_avail_out = strm->avail_out;
    std::vector<int32_t> decoded;
    while (true) {
        // Drain any buffered data to output
        if (strm->decompress_buffer_index < strm->decompress_buffer_size) {
            size_t available = strm->decompress_buffer_size - strm->decompress_buffer_index;
            size_t to_copy = (available < strm->avail_out) ? available : strm->avail_out;
            memcpy(strm->next_out, strm->decompress_buffer_data + strm->decompress_buffer_index, to_copy);
            strm->next_out += to_copy;
            strm->avail_out -= to_copy;
            strm->decompress_buffer_index += to_copy;
            if (strm->avail_out == 0) {
                strm->status = WAVPACKZLIB_RC_OK;
                return 0;
            }
        }

        // Buffer is empty, get more data from WavPack
        if (!wpc) {
            input_state.next_in = strm->next_in;
            input_state.avail_in = strm->avail_in;
            input_state.total_size = INT64_MAX;
            input_state.current_pos = 0;

            // Skip RIFF/WAV container if present - the WavPack library's
            // 'R' first-byte check only handles legacy v3 files. For v4/v5
            // blocks inside a RIFF wrapper, scan ahead to the first "wvpk"
            // block so the library uses the normal streaming decode path.
            if (input_state.avail_in >= 12 &&
                input_state.next_in[0] == 'R' &&
                input_state.next_in[1] == 'I' &&
                input_state.next_in[2] == 'F' &&
                input_state.next_in[3] == 'F') {
                for (size_t i = 0; i < input_state.avail_in - 8; i++) {
                    if (input_state.next_in[i] == 'w' &&
                        input_state.next_in[i+1] == 'v' &&
                        input_state.next_in[i+2] == 'p' &&
                        input_state.next_in[i+3] == 'k') {
                        input_state.next_in += i;
                        input_state.avail_in -= i;
                        break;
                    }
                }
            }

            char error[80] = {0};
            wpc = WavpackOpenFileInputEx64(&wp_reader, this, nullptr, error, OPEN_STREAMING | OPEN_NO_CHECKSUM, 0);
            if (!wpc) {
                strm->status = WAVPACKZLIB_RC_INITIALIZATION_ERROR;
                return WAVPACKZLIB_RC_INITIALIZATION_ERROR;
            }

            // Reject hybrid mode — it produces lossy output incompatible with
            // our bit-identical roundtrip requirement.
            int mode = WavpackGetMode(static_cast<WavpackContext*>(wpc));
            if (mode & MODE_HYBRID) {
                WavpackCloseFile(static_cast<WavpackContext*>(wpc));
                wpc = nullptr;
                strm->status = WAVPACKZLIB_RC_INITIALIZATION_ERROR;
                return WAVPACKZLIB_RC_INITIALIZATION_ERROR;
            }

            size_t block_samples = 1024;
            size_t bps = (bits_per_sample > 0) ? (bits_per_sample + 7) / 8 : 2;
            size_t buf_size = block_samples * channels * bps + 4096;

            if (strm->decompress_buffer_data) {
                delete[] strm->decompress_buffer_data;
            }
            strm->decompress_buffer_data = new uint8_t[buf_size];
            strm->decompress_buffer_size_real = buf_size;
        } else {
            input_state.next_in = strm->next_in;
            input_state.avail_in = strm->avail_in;
        }

        size_t bpsf = (bits_per_sample + 7) / 8;
        size_t max_samples = strm->decompress_buffer_size_real / (channels * bpsf);
        if (max_samples > 65536) max_samples = 65536;

        if (decoded.size() < max_samples * channels)
            decoded.resize(max_samples * channels);

        uint32_t samples_got = WavpackUnpackSamples(static_cast<WavpackContext*>(wpc), decoded.data(), static_cast<uint32_t>(max_samples));

        strm->avail_in = input_state.avail_in;
        strm->next_in = const_cast<uint8_t*>(input_state.next_in);

        if (samples_got == 0) {
            int nerrors = WavpackGetNumErrors(static_cast<WavpackContext*>(wpc));
            if (nerrors > 0) {
                strm->status = WAVPACKZLIB_RC_DECOMPRESSION_ERROR;
                return WAVPACKZLIB_RC_DECOMPRESSION_ERROR;
            }
            // If buffer still has data, return OK for caller to drain next time
            if (strm->decompress_buffer_index < strm->decompress_buffer_size) {
                strm->status = WAVPACKZLIB_RC_OK;
                return 0;
            }
            // Stream ended. If partial data was written (last buffer drain), 
            // zero-fill remaining output and return OK for this last sector.
            if (strm->avail_out < orig_avail_out) {
                memset(strm->next_out, 0, strm->avail_out);
                strm->next_out += strm->avail_out;
                strm->avail_out = 0;
                strm->status = WAVPACKZLIB_RC_OK;
                return 0;
            }
            strm->status = WAVPACKZLIB_RC_END_OF_STREAM;
            return WAVPACKZLIB_RC_END_OF_STREAM;
        }

        size_t total_bytes = static_cast<size_t>(samples_got) * channels * bpsf;

        if (strm->decompress_buffer_size_real < total_bytes) {
            if (strm->decompress_buffer_data) {
                delete[] strm->decompress_buffer_data;
            }
            strm->decompress_buffer_size_real = total_bytes + 4096;
            strm->decompress_buffer_data = new uint8_t[strm->decompress_buffer_size_real];
        }

        uint8_t* dst = strm->decompress_buffer_data;
        if (bpsf == 2) {
            for (uint32_t i = 0; i < samples_got * channels; i++) {
                int16_t val = static_cast<int16_t>(decoded[i]);
                dst[2 * i] = static_cast<uint8_t>(val & 0xFF);
                dst[2 * i + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            }
        } else if (bpsf == 1) {
            for (uint32_t i = 0; i < samples_got * channels; i++) {
                dst[i] = static_cast<uint8_t>(static_cast<int8_t>(decoded[i]) + 128);
            }
        } else {
            for (uint32_t i = 0; i < samples_got * channels; i++) {
                for (size_t b = 0; b < bpsf; b++) {
                    dst[i * bpsf + b] = static_cast<uint8_t>((decoded[i] >> (8 * b)) & 0xFF);
                }
            }
        }

        strm->decompress_buffer_size = total_bytes;
        strm->decompress_buffer_index = 0;

        // Loop back to drain the new buffer data to output
    }
}

void wavpackzlib::close() {
    if (wpc) {
        if (is_compressor) {
            WavpackFlushSamples(static_cast<WavpackContext*>(wpc));
        }
        WavpackCloseFile(static_cast<WavpackContext*>(wpc));
        wpc = nullptr;
    }

    if (strm && strm->decompress_buffer_data) {
        delete[] strm->decompress_buffer_data;
        strm->decompress_buffer_data = nullptr;
        strm->decompress_buffer_size = 0;
        strm->decompress_buffer_size_real = 0;
        strm->decompress_buffer_index = 0;
    }

    overflow.clear();
}