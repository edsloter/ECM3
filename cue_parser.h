/*******************************************************************************
 *
 * ecm3 — Enhanced ECM (Error Code Modeler) for CD-ROM images
 * Copyright (C) 2026 Edward Sloter
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

#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <cstdio>
#include <fstream>

enum cue_track_mode : uint8_t {
    CUE_MODE_AUDIO = 0,
    CUE_MODE_MODE1_2048,
    CUE_MODE_MODE1_2352,
    CUE_MODE_MODE2_2336,
    CUE_MODE_MODE2_2352
};

enum cue_track_flags : uint8_t {
    CUE_FLAG_NONE = 0,
    CUE_FLAG_HAS_PREGAP = 0x01,
    CUE_FLAG_HAS_POSTGAP = 0x02,
    CUE_FLAG_DCP = 0x04,
    CUE_FLAG_4CH = 0x08,
    CUE_FLAG_PRE = 0x10,
    CUE_FLAG_SCMS = 0x20
};

struct cue_index {
    uint8_t number;
    uint32_t sector_offset;
};

struct cue_track {
    uint8_t number;
    cue_track_mode mode;
    uint8_t flags;
    std::string isrc;
    std::vector<cue_index> indices;
    std::string file_reference;
    uint64_t file_byte_offset;
};

struct cue_sheet {
    std::string catalog;
    std::string cd_text_file;
    std::vector<cue_track> tracks;
    std::vector<std::string> file_order;

    uint32_t total_sectors() const;
};

int cue_parse(const std::string& filename, cue_sheet& sheet);
uint32_t cue_msf_to_sector(const std::string& msf);
std::string cue_sector_to_msf(uint32_t sector);
cue_track_mode cue_parse_track_mode(const std::string& mode_str, const std::string& sector_size);
std::string cue_mode_to_string(cue_track_mode mode);

// Write a cue_sheet struct to a .cue file (binary mode, CRLF).
int write_cue_file(const std::string& path, const cue_sheet& sheet, bool split_output, const std::string& bin_filename);