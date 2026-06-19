/*******************************************************************************
 *
 * Original work by Daniel Carrasco at https://www.electrosoftcloud.com
 * Modifications copyright (C) 2026 Edward Sloter
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#include <stdexcept>
#include "compressor.h"

compressor::compressor(sector_tools_compression mode, bool is_compression, int32_t comp_level) {
    comp_mode = mode;
    compression = is_compression;
    compression_level = comp_level;
    int ret;
    // Class initialzer
    switch(mode) {
    case C_ZLIB:
        strm_zlib.zalloc = Z_NULL;
        strm_zlib.zfree = Z_NULL;
        strm_zlib.opaque = Z_NULL;

        if (is_compression) {
            ret = deflateInit(&strm_zlib, comp_level);
        }
        else {
            ret = inflateInit(&strm_zlib);
        }
        
        if (ret != Z_OK) {
            fprintf(stderr, "There was an error initializing the ZLIB encoder/decoder\n");
            //throw std::runtime_error("Error initializing the zlib compressor/decompressor.");
        }

        break;

    case C_LZMA:
        strm_lzma = LZMA_STREAM_INIT;

        if (is_compression) {
            lzma_lzma_preset(&opt_lzma2, comp_level);

            lzma_filter filters[] = {
                { LZMA_FILTER_X86, NULL },
                { LZMA_FILTER_LZMA2, &opt_lzma2 },
                { LZMA_VLI_UNKNOWN, NULL },
            };
            ret = lzma_stream_encoder(&strm_lzma, filters, LZMA_CHECK_NONE);
        }
        else {
            ret = lzma_stream_decoder(
                &strm_lzma,
                UINT64_MAX,
                LZMA_IGNORE_CHECK
            );
        }

        if (ret != LZMA_OK) {
            fprintf(stderr, "There was an error initializing LZMA encoder/decoder\n");
        }

        break;

    case C_LZMA2:
        strm_lzma2 = LZMA_STREAM_INIT;

        if (is_compression) {
            lzma_lzma_preset(&opt_lzma2_opts, comp_level);

            lzma_filter filters[] = {
                { LZMA_FILTER_LZMA2, &opt_lzma2_opts },
                { LZMA_VLI_UNKNOWN, NULL },
            };
            ret = lzma_stream_encoder(&strm_lzma2, filters, LZMA_CHECK_NONE);
        }
        else {
            ret = lzma_stream_decoder(
                &strm_lzma2,
                UINT64_MAX,
                LZMA_IGNORE_CHECK
            );
        }

        if (ret != LZMA_OK) {
            fprintf(stderr, "There was an error initializing LZMA2 encoder/decoder\n");
        }

        break;

    case C_LZ4:
        if (is_compression) {
            // We will create blocks of 1Mb and data will not be split between blocks.
            strm_lz4 = new lzlib4((size_t)1048576, LZLIB4_INPUT_NOSPLIT, (int8_t)((134 * comp_level) / 100));
        }
        else {
            strm_lz4 = new lzlib4();
        }
        break;

    case C_FLAC:
        if (is_compression) {
            int32_t base_level = comp_level & 0x7F;
            if (base_level > 8) base_level = 8;
            int32_t flac_comp_level = base_level | (comp_level & ~0x7F);
            strm_flac = new flaczlib(true, 2, 16, 44100, 0, flac_comp_level);

        }
        else {
            strm_flac = new flaczlib(false);
        }
        break;

    case C_ZSTD:
        if (is_compression) {
            strm_zstd_cctx = ZSTD_createCCtx();
            if (!strm_zstd_cctx) {
                fprintf(stderr, "There was an error initializing the ZSTD encoder\n");
            }
            ZSTD_CCtx_setParameter(strm_zstd_cctx, ZSTD_c_compressionLevel, comp_level);
            ZSTD_CCtx_setParameter(strm_zstd_cctx, ZSTD_c_checksumFlag, 0);
        } else {
            strm_zstd_dctx = ZSTD_createDCtx();
            if (!strm_zstd_dctx) {
                fprintf(stderr, "There was an error initializing the ZSTD decoder\n");
            }
        }
        strm_zstd_in.src = NULL;
        strm_zstd_in.size = 0;
        strm_zstd_in.pos = 0;
        strm_zstd_out.dst = NULL;
        strm_zstd_out.size = 0;
        strm_zstd_out.pos = 0;
        break;

    case C_WAVPACK:
        if (is_compression) {
            int32_t base_level = comp_level & 0x7F;
            if (base_level > 9) base_level = 9;
            strm_wavpack = new wavpackzlib(true, 2, 16, 44100, 0, static_cast<int8_t>(comp_level));
        } else {
            strm_wavpack = new wavpackzlib(false, 2, 16, 44100, 0, 5);
        }
        break;
    }
}


// Destructor function that will call close()
compressor::~compressor(void) {
    close();
}


int8_t compressor::set_input(uint8_t* in, size_t &in_size){
    if (!compression) {
        switch(comp_mode) {
        case C_ZLIB:
            strm_zlib.avail_in = in_size;
            strm_zlib.next_in = in;

            return 0;
            break;

        case C_LZMA:
            strm_lzma.avail_in = in_size;
            strm_lzma.next_in = in;

            return 0;
            break;

        case C_LZ4:
            strm_lz4->strm.avail_in = in_size;
            strm_lz4->strm.next_in = in;

            return 0;
            break;

        case C_FLAC:
            strm_flac->strm.avail_in = in_size;
            strm_flac->strm.next_in = in;

            return 0;
            break;

        case C_LZMA2:
            strm_lzma2.avail_in = in_size;
            strm_lzma2.next_in = in;

            return 0;
            break;

        case C_ZSTD:
            strm_zstd_in.src = in;
            strm_zstd_in.size = in_size;
            strm_zstd_in.pos = 0;
            return 0;
            break;

        case C_WAVPACK:
            strm_wavpack->strm->avail_in = in_size;
            strm_wavpack->strm->next_in = in;
            return 0;
            break;
        }

        return 0;
    }
    else {
        //throw std::runtime_error("Trying to use a decompression object to compress.");
        return -1;
    }
}


int8_t compressor::set_output(uint8_t* out, size_t &out_size){
    if (compression) {
        switch(comp_mode) {
        case C_ZLIB:
            strm_zlib.avail_out = out_size;
            strm_zlib.next_out = out;

            return 0;
            break;

        case C_LZMA:
            strm_lzma.avail_out = out_size;
            strm_lzma.next_out = out;

            return 0;
            break;

        case C_LZ4:
            strm_lz4->strm.avail_out = out_size;
            strm_lz4->strm.next_out = out;

            return 0;
            break;

        case C_FLAC:
            strm_flac->strm.avail_out = out_size;
            strm_flac->strm.next_out = out;

            return 0;
            break;

        case C_LZMA2:
            strm_lzma2.avail_out = out_size;
            strm_lzma2.next_out = out;

            return 0;
            break;

        case C_ZSTD:
            strm_zstd_out.dst = out;
            strm_zstd_out.size = out_size;
            strm_zstd_out.pos = 0;
            return 0;
            break;

        case C_WAVPACK:
            if (!strm_wavpack->overflow.empty()) {
                size_t to_copy = std::min(strm_wavpack->overflow.size(), out_size);
                memcpy(out, strm_wavpack->overflow.data(), to_copy);
                strm_wavpack->strm->next_out = out + to_copy;
                strm_wavpack->strm->avail_out = out_size - to_copy;
                if (to_copy < strm_wavpack->overflow.size()) {
                    size_t remainder = strm_wavpack->overflow.size() - to_copy;
                    memmove(strm_wavpack->overflow.data(), strm_wavpack->overflow.data() + to_copy, remainder);
                    strm_wavpack->overflow.resize(remainder);
                } else {
                    strm_wavpack->overflow.clear();
                }
            } else {
                strm_wavpack->strm->avail_out = out_size;
                strm_wavpack->strm->next_out = out;
            }
            return 0;
            break;
        }

        return 0;
    }
    else {
        //throw std::runtime_error("Trying to use a compression object to decompress.");
        return -1;
    }
}


int8_t compressor::compress(size_t &out_size, uint8_t* in, size_t in_size, uint8_t flush_mode){
    if (compression) {
        int8_t return_code;
        lzma_action flushmode_lzma = LZMA_RUN;
        size_t processed;

        switch(comp_mode) {
        case C_ZLIB:
            strm_zlib.avail_in = in_size;
            strm_zlib.next_in = in;

            return_code = deflate(&strm_zlib, flush_mode);

            // If is the end of the stream, then is OK
            if (return_code == Z_STREAM_END) {
                return_code = Z_OK;
            }
            // If the buffer is 0 bytes the compressor will give BUF_ERROR.
            // Can be ignored because no data was sent so nothing failed
            if (return_code == Z_BUF_ERROR && in_size == 0) {
                return_code = Z_OK;
            }

            out_size = strm_zlib.avail_out;
            return return_code;
            break;

        case C_LZMA:
            strm_lzma.avail_in = in_size;
            strm_lzma.next_in = in;

            switch (flush_mode) {
                case Z_FULL_FLUSH:
                    flushmode_lzma = LZMA_FULL_FLUSH;
                    break;

                case Z_FINISH:
                    flushmode_lzma = LZMA_FINISH;
                    break;
            }

            return_code = lzma_code(&strm_lzma, flushmode_lzma);
            
            if (return_code == LZMA_STREAM_END) {
                return_code = LZMA_OK;
            }
            if (return_code == LZMA_BUF_ERROR && in_size == 0) {
                return_code = LZMA_OK;
            }

            out_size = strm_lzma.avail_out;
            return return_code;
            break;

        case C_LZMA2:
            strm_lzma2.avail_in = in_size;
            strm_lzma2.next_in = in;

            switch (flush_mode) {
                case Z_FULL_FLUSH:
                    flushmode_lzma = LZMA_FULL_FLUSH;
                    break;

                case Z_FINISH:
                    flushmode_lzma = LZMA_FINISH;
                    break;
            }

            return_code = lzma_code(&strm_lzma2, flushmode_lzma);
            
            if (return_code == LZMA_STREAM_END) {
                return_code = LZMA_OK;
            }
            if (return_code == LZMA_BUF_ERROR && in_size == 0) {
                return_code = LZMA_OK;
            }

            out_size = strm_lzma2.avail_out;
            return return_code;
            break;

        case C_LZ4:
            strm_lz4->strm.avail_in = in_size;
            strm_lz4->strm.next_in = in;

            strm_lz4->compress((lzlib4_flush_mode)flush_mode);

            out_size = strm_lz4->strm.avail_out;
            break;

        case C_FLAC:
            strm_flac->strm.avail_in = in_size;
            strm_flac->strm.next_in = in;

            strm_flac->compress(in_size / 4, (flaczlib_flush_mode)flush_mode);

            out_size = strm_flac->strm.avail_out;
            break;

        case C_ZSTD:
            {
                ZSTD_EndDirective zstd_flush = ZSTD_e_continue;
                if (flush_mode == Z_FULL_FLUSH) {
                    zstd_flush = ZSTD_e_flush;
                } else if (flush_mode == Z_FINISH) {
                    zstd_flush = ZSTD_e_end;
                }
                strm_zstd_in.src = in;
                strm_zstd_in.size = in_size;
                strm_zstd_in.pos = 0;
                size_t rc = ZSTD_compressStream2(strm_zstd_cctx, &strm_zstd_out, &strm_zstd_in, zstd_flush);
                if (ZSTD_isError(rc)) {
                    fprintf(stderr, "ZSTD compress error: %s\n", ZSTD_getErrorName(rc));
                    return -1;
                }
                out_size = strm_zstd_out.size - strm_zstd_out.pos;
            }
            break;

        case C_WAVPACK:
            {
                strm_wavpack->strm->avail_in = in_size;
                strm_wavpack->strm->next_in = in;
                uint32_t samples = static_cast<uint32_t>(in_size / 4);
                wavpackzlib_flush_mode wp_flush = WAVPACKZLIB_NO_FLUSH;
                if (flush_mode == Z_FULL_FLUSH) wp_flush = WAVPACKZLIB_FULL_FLUSH;
                if (flush_mode == Z_FINISH) wp_flush = WAVPACKZLIB_FINISH;
                int rc = strm_wavpack->compress(samples, wp_flush);
                out_size = strm_wavpack->strm->avail_out;
                return rc;
            }
            break;
        }

        return 0;
    }
    else {
        return -1;
    }
}


int8_t compressor::decompress(uint8_t* out, size_t &out_size, size_t &in_size, uint8_t flusmode){
    if (!compression) {
        int8_t return_code;
        switch(comp_mode) {
        case C_ZLIB:
            {
                strm_zlib.avail_out = out_size;
                strm_zlib.next_out = out;
                return_code = inflate(&strm_zlib, flusmode);

                in_size = strm_zlib.avail_in;
                return return_code;
                break;
            }

        case C_LZMA:
            {
                strm_lzma.avail_out = out_size;
                strm_lzma.next_out = out;
                return_code = lzma_code(&strm_lzma, LZMA_RUN);

                in_size = strm_lzma.avail_in;
                return return_code;
                break;
            }

        case C_LZMA2:
            {
                strm_lzma2.avail_out = out_size;
                strm_lzma2.next_out = out;
                return_code = lzma_code(&strm_lzma2, LZMA_RUN);

                in_size = strm_lzma2.avail_in;
                return return_code;
                break;
            }

        case C_LZ4:
            {
                strm_lz4->strm.avail_out = out_size;
                strm_lz4->strm.next_out = out;
                int return_code = strm_lz4->decompress_partial(false, -1);

                in_size = strm_lz4->strm.avail_in;
                return return_code;
                break;
            }

        case C_FLAC:
            {
                strm_flac->strm.avail_out = out_size;
                strm_flac->strm.next_out = out;
                int return_code = strm_flac->decompress_partial(false, -1);

                in_size = strm_flac->strm.avail_in;
                return return_code;
                break;
            }

        case C_ZSTD:
            {
                strm_zstd_out.dst = out;
                strm_zstd_out.size = out_size;
                strm_zstd_out.pos = 0;
                size_t rc = ZSTD_decompressStream(strm_zstd_dctx, &strm_zstd_out, &strm_zstd_in);
                if (ZSTD_isError(rc)) {
                    // Not enough input data is expected (ZSTD_error_srcSize_wrong)
                    // Only report actual decompression errors
                    if (ZSTD_getErrorCode(rc) != ZSTD_error_srcSize_wrong) {
                        fprintf(stderr, "ZSTD decompress error: %s\n", ZSTD_getErrorName(rc));
                    }
                    in_size = strm_zstd_in.size - strm_zstd_in.pos;
                    return -1;
                }
                in_size = strm_zstd_in.size - strm_zstd_in.pos;
                return 0;
                break;
            }

        case C_WAVPACK:
            {
                strm_wavpack->strm->avail_out = out_size;
                strm_wavpack->strm->next_out = out;
                int return_code = strm_wavpack->decompress();
                in_size = strm_wavpack->strm->avail_in;
                // Map WavPack error codes to proper int8_t values for the caller
                if (return_code != WAVPACKZLIB_RC_OK) {
                    if (return_code == WAVPACKZLIB_RC_END_OF_STREAM) {
                        return 1; // signal end of stream, not an error
                    }
                    return -1; // generic error
                }
                return 0;
                break;
            }
        }

        return 0;
    }
    else {
        return -1;
    }
}


int8_t compressor::close(){
    switch(comp_mode) {
    case C_ZLIB:
        if (compression) {
            (void)deflateEnd(&strm_zlib);
            strm_zlib = {};
        }
        else {
            (void)inflateEnd(&strm_zlib);
            strm_zlib = {};
        }
        return 0;
        break;

    case C_LZMA:
        lzma_end(&strm_lzma);
        strm_lzma = {};
        return 0;
        break;

    case C_LZMA2:
        lzma_end(&strm_lzma2);
        strm_lzma2 = {};
        return 0;
        break;

    case C_LZ4:
        delete strm_lz4; //strm_lz4->close();
        return 0;
        break;

    case C_FLAC:
        delete strm_flac; //strm_lz4->close();
        return 0;
        break;

    case C_ZSTD:
        if (compression) {
            if (strm_zstd_cctx) {
                ZSTD_freeCCtx(strm_zstd_cctx);
                strm_zstd_cctx = NULL;
            }
        } else {
            if (strm_zstd_dctx) {
                ZSTD_freeDCtx(strm_zstd_dctx);
                strm_zstd_dctx = NULL;
            }
        }
        return 0;
        break;

    case C_WAVPACK:
        delete strm_wavpack;
        strm_wavpack = NULL;
        return 0;
        break;
    }

    return -1;
}


int8_t compressor::set_dictionary(const uint8_t* dict, size_t dict_size) {
    if (!dict || dict_size == 0) return 0;

    switch (comp_mode) {
    case C_ZLIB:
        if (compression) {
            int ret = deflateSetDictionary(&strm_zlib, dict, (uInt)dict_size);
            return (ret == Z_OK) ? 0 : -1;
        } else {
            int ret = inflateSetDictionary(&strm_zlib, dict, (uInt)dict_size);
            return (ret == Z_OK) ? 0 : -1;
        }
    case C_LZMA:
    case C_LZMA2:
        // LZMA preset_dict requires raw encoder/decoder which changes
        // the stream format. Skip cross-stream dicts for LZMA.
    case C_LZ4:
    case C_FLAC:
    case C_ZSTD:
    case C_WAVPACK:
        return 0;  // No dictionary API
    }
    return 0;
}


size_t compressor::data_left_in() {
    switch(comp_mode) {
    case C_ZLIB:
        return strm_zlib.avail_in;
        break;

    case C_LZMA:
        return strm_lzma.avail_in;
        break;
    case C_LZMA2:
        return strm_lzma2.avail_in;
        break;
    case C_LZ4:
        return strm_lz4->strm.avail_in;
        break;
    case C_FLAC:
        return strm_flac->strm.avail_in;
        break;

    case C_ZSTD:
        return strm_zstd_in.size - strm_zstd_in.pos;
        break;

    case C_WAVPACK:
        return strm_wavpack->strm->avail_in;
        break;
    }

    return -1;
};

size_t compressor::data_left_out() {
    switch(comp_mode) {
    case C_ZLIB:
        return strm_zlib.avail_out;
        break;

    case C_LZMA:
        return strm_lzma.avail_out;
        break;
    
    case C_LZMA2:
        return strm_lzma2.avail_out;
        break;
    
    case C_LZ4:
        return strm_lz4->strm.avail_out;
        break;
    case C_FLAC:
        return strm_flac->strm.avail_out;
        break;

    case C_ZSTD:
        return strm_zstd_out.size - strm_zstd_out.pos;
        break;

    case C_WAVPACK:
        return strm_wavpack->strm->avail_out;
        break;
    }

    return -1;
};