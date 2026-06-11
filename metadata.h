#pragma once

#include "ecm3.h"
#include "cue_parser.h"
#include "util.h"
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

std::string get_cue_dir(const std::string& cue_path);
std::string get_basename(const std::string& filepath);
std::string fmt_track_number(uint8_t track_number, size_t total_tracks);
int concat_split_bins(cue_sheet& sheet, const std::string& cue_dir, temp_file& tf);

void build_metadata_from_cue(
    const cue_sheet& sheet,
    uint64_t total_image_sectors,
    track_metadata_header& meta_header,
    std::vector<track_metadata_entry>& meta_entries
);

int write_metadata_block(
    std::fstream& out_file,
    const track_metadata_header& meta_header,
    const std::vector<track_metadata_entry>& meta_entries,
    std::vector<blocks_toc>& file_blocks_toc
);

int read_metadata_block(
    std::ifstream& in_file,
    uint64_t block_start,
    track_metadata_header& meta_header,
    std::vector<track_metadata_entry>& meta_entries
);

void write_cue_from_metadata(
    const track_metadata_header& meta_header,
    const std::vector<track_metadata_entry>& meta_entries,
    const std::string& out_path,
    bool split_output
);

int split_output_bin(
    const std::string& out_path,
    const track_metadata_header& meta_header,
    const std::vector<track_metadata_entry>& meta_entries,
    uint64_t total_image_sectors,
    bool force_rewrite = false
);