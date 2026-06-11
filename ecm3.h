#pragma once

/*******************************************************************************
 *
 * ecm3 — Enhanced ECM (Error Code Modeler) for CD-ROM images
 * Copyright (C) 2026 Edward Sloter
 *
 * Based on the original ECM by Neill Corlett and the ecmtool project by
 * Daniel Carrasco (https://www.electrosoftcloud.com).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#define TITLE "ecm3 v3.0.1.0"
#define COPYR "Update on Error Code Modeler with advanced features"
#define SUBTL "Based on the original ECM by Neill Corlett, reloaded by Daniel Carrasco."
#define AUTHR "Copyright (C) 2026 Edward Sloter"
#define VERSI "3.0.1.0"

#include "banner.h"
#include "sector_tools.h"
#include "cue_parser.h"
#include <getopt.h>
//#include <stdbool.h>
#include <algorithm>
//#include <stdexcept>
#include <stdio.h>
//#include <stdlib.h>
//#include <errno.h>
//#include <time.h>
//#include <limits.h>
//#include <ctype.h>
#include <vector>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>


// Configurations
#define SECTORS_PER_BLOCK 100
#define ECM_FILE_VERSION 3
#define BUFFER_SIZE 0x500000lu

// MB Macro
#define MB(x) ((float)(x) / 1024 / 1024)

////////////////////////////////////////////////////////////////////////////////
//
// Try to figure out integer types
//
#if defined(_STDINT_H) || defined(_EXACT_WIDTH_INTS)

// _STDINT_H_ - presume stdint.h has already been included
// _EXACT_WIDTH_INTS - OpenWatcom already provides int*_t in sys/types.h

#elif defined(__STDC__) && __STDC__ && __STDC_VERSION__ >= 199901L

// Assume C99 compliance when the compiler specifically tells us it is
#include <stdint.h>

#elif defined(_MSC_VER)

// On Visual Studio, use its integral types
typedef   signed __int8   int8_t;
typedef unsigned __int8  uint8_t;
typedef   signed __int16  int16_t;
typedef unsigned __int16 uint16_t;
typedef   signed __int32  int32_t;
typedef unsigned __int32 uint32_t;

#endif

////////////////////////////////////////////////////////////////////////////////

// Streams and sectors structs
struct stream {
    uint8_t type : 1;
    uint8_t compression : 3;
    uint8_t cross_stream_dict : 1;
    // 3 bits unused
    uint32_t end_sector = 0;
    uint32_t out_end_position = 0;
};

struct sector {
    uint8_t mode : 4;
    uint32_t sector_count = 0;
};

struct block_header {
    uint8_t type = 0;
    uint8_t compression = 0;
    uint64_t block_size = 0;
    uint64_t real_block_size = 0;
};

struct blocks_toc {
    uint8_t type;
    uint64_t start_position;
};

struct ecm_header {
    uint8_t optimizations;
    uint8_t sectors_per_block;
    uint64_t crc_mode;
    uint64_t streams_toc_pos;
    uint64_t sectors_toc_pos;
    uint64_t ecm_data_pos;
    uint8_t title_length;
    uint8_t id_length;
    std::string title;
    std::string id;
};

struct sec_str_size {
    sector_tools_compression compression;
    uint32_t count;
    uint32_t uncompressed_size;
    uint32_t compressed_size;
};

// On-disk format sizes (explicit little-endian serialization)
#define ECM_STREAM_SIZE       9
#define ECM_SECTOR_SIZE       5
#define ECM_BLOCK_HEADER_SIZE 18
#define ECM_BLOCKS_TOC_SIZE   9
#define ECM_HEADER_FIXED_SIZE 36
#define ECM_SEC_STR_SIZE     13

static inline void serialize_stream(uint8_t* dst, const stream& s) {
    dst[0] = (uint8_t)((s.type & 0x01) | ((s.compression & 0x07) << 1) | ((s.cross_stream_dict ? 1 : 0) << 4));
    sector_tools::put32lsb(dst + 1, s.end_sector);
    sector_tools::put32lsb(dst + 5, s.out_end_position);
}

static inline void deserialize_stream(const uint8_t* src, stream& s) {
    s.type = src[0] & 0x01;
    s.compression = (src[0] >> 1) & 0x07;
    s.cross_stream_dict = (src[0] >> 4) & 0x01;
    s.end_sector = sector_tools::get32lsb(src + 1);
    s.out_end_position = sector_tools::get32lsb(src + 5);
}

static inline void serialize_sector(uint8_t* dst, const sector& s) {
    dst[0] = (uint8_t)(s.mode & 0x0F);
    sector_tools::put32lsb(dst + 1, s.sector_count);
}

static inline void deserialize_sector(const uint8_t* src, sector& s) {
    s.mode = src[0] & 0x0F;
    s.sector_count = sector_tools::get32lsb(src + 1);
}

static inline void serialize_block_header(uint8_t* dst, const block_header& h) {
    dst[0] = h.type;
    dst[1] = h.compression;
    sector_tools::put64lsb(dst + 2, h.block_size);
    sector_tools::put64lsb(dst + 10, h.real_block_size);
}

static inline void deserialize_block_header(const uint8_t* src, block_header& h) {
    h.type = src[0];
    h.compression = src[1];
    h.block_size = sector_tools::get64lsb(src + 2);
    h.real_block_size = sector_tools::get64lsb(src + 10);
}

static inline void serialize_blocks_toc(uint8_t* dst, const blocks_toc& t) {
    dst[0] = t.type;
    sector_tools::put64lsb(dst + 1, t.start_position);
}

static inline void deserialize_blocks_toc(const uint8_t* src, blocks_toc& t) {
    t.type = src[0];
    t.start_position = sector_tools::get64lsb(src + 1);
}

static inline void serialize_ecm_header(uint8_t* dst, const ecm_header& h) {
    dst[0] = h.optimizations;
    dst[1] = h.sectors_per_block;
    sector_tools::put64lsb(dst + 2, h.crc_mode);
    sector_tools::put64lsb(dst + 10, h.streams_toc_pos);
    sector_tools::put64lsb(dst + 18, h.sectors_toc_pos);
    sector_tools::put64lsb(dst + 26, h.ecm_data_pos);
    dst[34] = h.title_length;
    dst[35] = h.id_length;
}

static inline void deserialize_ecm_header(const uint8_t* src, ecm_header& h) {
    h.optimizations = src[0];
    h.sectors_per_block = src[1];
    h.crc_mode = sector_tools::get64lsb(src + 2);
    h.streams_toc_pos = sector_tools::get64lsb(src + 10);
    h.sectors_toc_pos = sector_tools::get64lsb(src + 18);
    h.ecm_data_pos = sector_tools::get64lsb(src + 26);
    h.title_length = src[34];
    h.id_length = src[35];
}

static inline void serialize_sec_str_size(uint8_t* dst, const sec_str_size& s) {
    dst[0] = (uint8_t)s.compression;
    sector_tools::put32lsb(dst + 1, s.count);
    sector_tools::put32lsb(dst + 5, s.uncompressed_size);
    sector_tools::put32lsb(dst + 9, s.compressed_size);
}

static inline void deserialize_sec_str_size(const uint8_t* src, sec_str_size& s) {
    s.compression = (sector_tools_compression)src[0];
    s.count = sector_tools::get32lsb(src + 1);
    s.uncompressed_size = sector_tools::get32lsb(src + 5);
    s.compressed_size = sector_tools::get32lsb(src + 9);
}

struct track_metadata_entry {
    uint8_t track_number;
    uint8_t track_mode;
    uint8_t track_flags;
    uint32_t start_sector;
    uint32_t end_sector;
    uint16_t pregap_sectors;
};

struct track_metadata_header {
    uint8_t version;
    uint8_t flags;
    uint16_t track_count;
    uint32_t total_sectors;
    uint8_t media_catalog_number[13];
};

#define ECM_TRACK_METADATA_ENTRY_SIZE 13
#define ECM_TRACK_METADATA_HEADER_SIZE 21

// Struct for script vector
struct stream_script {
    stream stream_data;
    std::vector<sector> sectors_data;
};

// Ecmify options struct
struct ecm_options {
    bool force_rewrite = false;
    bool keep_output = false;
    bool split_output = false;
    bool verify = false;
    sector_tools_compression data_compression = C_NONE;
    sector_tools_compression audio_compression = C_NONE;
    uint8_t compression_level = 5;
    bool extreme_compression = false;
    bool seekable = false;
    uint8_t sectors_per_block = SECTORS_PER_BLOCK;
    uint32_t jobs = 0;
    std::string in_filename;
    std::string out_filename;
    std::string cue_filename;
    std::string image_title;
    bool batch_cue_mode = false;
    bool batch_decode_mode = false;
    std::string batch_directory;
    bool delete_source = false;
    std::vector<std::string> delete_paths;
    bool cue_split = false;
    bool cue_combine = false;
    std::string cue_split_combine_file;
    optimization_options optimizations = (
        OO_REMOVE_SYNC |
        OO_REMOVE_MSF |
        OO_REMOVE_MODE |
        OO_REMOVE_BLANKS |
        OO_REMOVE_REDUNDANT_FLAG |
        OO_REMOVE_ECC |
        OO_REMOVE_EDC |
        OO_REMOVE_GAP
    );
};

// Return codes
enum ecm3_return_code {
    ECMTOOL_OK = 0,
    ECMTOOL_FILE_READ_ERROR = INT_MIN,
    ECMTOOL_FILE_WRITE_ERROR,
    ECMTOOL_HEADER_COMPRESSION_ERROR,
    ECMTOOL_BUFFER_MEMORY_ERROR,
    ECMTOOL_PROCESSING_ERROR,
    ECMTOOL_CORRUPTED_STREAM,
    ECMTOOL_CORRUPTED_HEADER
};

enum ecmfile_block_type {
    ECMFILE_BLOCK_TYPE_DELETED = 0,
    ECMFILE_BLOCK_TYPE_METADATA,
    ECMFILE_BLOCK_TYPE_TOC,
    ECMFILE_BLOCK_TYPE_ECM,
    ECMFILE_BLOCK_TYPE_FILE,
};

////////////////////////////////////////////////////////////////////////////////

// Declare the functions
void print_help();
int get_options(
    int argc,
    char **argv,
    ecm_options *options
);
int image_to_ecm_block(
    std::ifstream &in_file,
    std::fstream &out_file,
    ecm_options *options,
    std::vector<uint32_t> *sectors_type_summary,
    std::vector<stream_script> *streams_script_out = nullptr
);
int ecm_block_to_image(
    std::ifstream &in_file,
    std::fstream &out_file,
    ecm_options *options
);
int write_block_header(
    std::fstream &out_file,
    block_header *block_header
);
int read_block_header(
    std::ifstream &out_file,
    block_header *block_header
);
static ecm3_return_code disk_analyzer (
    sector_tools *sTools,
    std::ifstream &in_file,
    size_t image_file_size,
    std::vector<stream_script> &streams_script,
    ecm_header *ecm_data_header,
    ecm_options *options
);
int compress_header (
    uint8_t *dest,
    uint32_t &destLen,
    uint8_t *source,
    uint32_t sourceLen,
    int level
);
#include "decompress_header.h"

static ecm3_return_code task_maker (
    stream *streams_toc,
    sec_str_size &streams_toc_count,
    sector *sectors_toc,
    sec_str_size &sectors_toc_count,
    std::vector<stream_script> &streams_script
);
static ecm3_return_code task_to_streams_header (
    stream *&streams_toc,
    sec_str_size &streams_toc_count,
    std::vector<stream_script> &streams_script
);
static ecm3_return_code task_to_sectors_header (
    sector *&sectors_toc,
    sec_str_size &sectors_toc_count,
    std::vector<stream_script> &streams_script
);

static ecm3_return_code disk_encode (
    sector_tools *sTools,
    std::ifstream &in_file,
    std::fstream &out_file,
    std::vector<stream_script> &streams_script,
    ecm_options *options,
    std::vector<uint32_t> *sectors_type,
    uint64_t ecm_block_start_position
);
static ecm3_return_code disk_decode (
    sector_tools *sTools,
    std::ifstream &in_file,
    std::fstream &out_file,
    std::vector<stream_script> &streams_script,
    ecm_options *options,
    uint64_t ecm_block_start_position
);
static void resetcounter(uint64_t total, sector_tools_compression audio_comp = C_NONE, sector_tools_compression data_comp = C_NONE, uint64_t audio_comp_total = 0, uint64_t data_comp_total = 0);
static void encode_progress(void);
static void decode_progress(void);
static void setcounter_analyze(uint64_t n);
static void setcounter_encode(uint64_t n);
static void setcounter_audio_comp_encode(uint64_t n);
static void setcounter_data_comp_encode(uint64_t n);
static void setcounter_decode(uint64_t n);

static void summary (
    std::vector<uint32_t> *sectors_type,
    ecm_options *options,
    size_t compressed_size,
    std::vector<stream_script> *streams_script = nullptr
);

void print_task(
    std::vector<stream_script> &streams_script
);

int detect_id_psx(std::string &id, uint8_t *data, uint64_t data_size);

// Optional GUI progress callback (called from worker thread with 0-100 percent)
typedef void (*progress_cb_t)(int percent);
void set_progress_callback(progress_cb_t cb);

/*
void write_to_file(std::string filename, uint8_t *data, uint64_t size) {
    FILE *out_file = fopen(filename.c_str(), "wb");
    fwrite(data, size, 1, out_file);
    fclose(out_file);
}
*/