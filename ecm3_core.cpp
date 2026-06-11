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

#include "ecm3_core.h"
#include "ecm3.h"
#include "metadata.h"
#include "cue_gen.h"
#include "sector_tools.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <atomic>

extern std::atomic<bool> g_interrupted;

int ecm3_encode(
    const std::string& input_path,
    const std::string& output_path,
    const ecm_options& opts,
    const cue_sheet* parsed_cue,
    bool has_cue,
    ecm3_result& result
) {
    int return_code = 0;

    std::ifstream in_file(input_path, std::ios::binary);
    if (!in_file.good()) {
        fprintf(stderr, "ERROR: input file cannot be opened.\n");
        return 1;
    }
    {
        char dummy;
        if (!in_file.read(&dummy, 0)) {
            fprintf(stderr, "ERROR: input file cannot be opened.\n");
            return 1;
        }
    }

    in_file.seekg(0, std::ios::end);
    uint64_t in_total_size = in_file.tellg();
    in_file.seekg(0, std::ios::beg);
    result.total_sectors = in_total_size / 2352;

    if (!opts.force_rewrite && std::filesystem::exists(output_path)) {
        fprintf(stderr, "ERROR: output file already exists. Use -f to overwrite.\n");
        return 1;
    }

    std::fstream out_file(output_path, std::ios::out | std::ios::binary);
    if (!out_file.good()) {
        fprintf(stderr, "ERROR: output file cannot be opened.\n");
        return 1;
    }

    out_file << "ECM" << char(ECM_FILE_VERSION);
    uint64_t toc_position = 0;
    uint8_t toc_pos_buf[8];
    sector_tools::put64lsb(toc_pos_buf, toc_position);
    out_file.write(reinterpret_cast<char*>(toc_pos_buf), sizeof(toc_pos_buf));

    std::vector<blocks_toc> file_blocks_toc;
    file_blocks_toc.push_back(blocks_toc());
    file_blocks_toc.back().type = ECMFILE_BLOCK_TYPE_ECM;
    file_blocks_toc.back().start_position = out_file.tellp();

    result.sectors_type_summary.resize(13);
    std::vector<stream_script> encode_streams_script;
    ecm_options mutable_opts = opts;
    return_code = image_to_ecm_block(in_file, out_file, &mutable_opts, &result.sectors_type_summary, &encode_streams_script);
    if (return_code) {
        fprintf(stderr, "\n\nERROR: there was an error processing the input file.\n\n");
        return return_code;
    }

    result.encode_streams_script = std::move(encode_streams_script);

    if (!has_cue || !parsed_cue) {
        cue_sheet synthetic_cue = generate_cue_from_sectors(result.encode_streams_script);
        if (parsed_cue == nullptr && !has_cue) {
            build_metadata_from_cue(synthetic_cue, result.total_sectors, result.meta_header, result.meta_entries);
            result.has_metadata = true;
        }
    } else {
        build_metadata_from_cue(*parsed_cue, result.total_sectors, result.meta_header, result.meta_entries);
        result.has_metadata = true;
    }

    if (!result.meta_entries.empty()) {
        return_code = write_metadata_block(out_file, result.meta_header, result.meta_entries, file_blocks_toc);
        if (return_code) {
            fprintf(stderr, "ERROR: Failed to write track metadata block.\n");
            return return_code;
        }
    }

    toc_position = out_file.tellp();
    block_header toc_block_header = {ECMFILE_BLOCK_TYPE_TOC, 0, 0, 0};
    toc_block_header.real_block_size = file_blocks_toc.size() * ECM_BLOCKS_TOC_SIZE;
    toc_block_header.block_size = toc_block_header.real_block_size;
    {
        uint8_t toc_bh_buf[ECM_BLOCK_HEADER_SIZE];
        serialize_block_header(toc_bh_buf, toc_block_header);
        out_file.write(reinterpret_cast<char*>(toc_bh_buf), sizeof(toc_bh_buf));
    }
    {
        std::vector<uint8_t> toc_data(file_blocks_toc.size() * ECM_BLOCKS_TOC_SIZE);
        for (size_t i = 0; i < file_blocks_toc.size(); i++) {
            serialize_blocks_toc(toc_data.data() + i * ECM_BLOCKS_TOC_SIZE, file_blocks_toc[i]);
        }
        out_file.write(reinterpret_cast<char*>(toc_data.data()), toc_data.size());
    }

    out_file.seekp(4);
    if (!out_file.good()) {
        fprintf(stderr, "ERROR: Failed to seek to TOC position write location.\n");
        return 1;
    }
    uint8_t toc_pos_buf2[8];
    sector_tools::put64lsb(toc_pos_buf2, toc_position);
    out_file.write(reinterpret_cast<char*>(toc_pos_buf2), sizeof(toc_pos_buf2));
    if (!out_file.good()) {
        fprintf(stderr, "ERROR: Failed to write TOC position.\n");
        return 1;
    }

    if (opts.delete_source && return_code == 0) {
        if (has_cue && parsed_cue) {
            result.source_paths.push_back(opts.cue_filename);
            std::string cue_dir = get_cue_dir(opts.cue_filename);
            for (const auto& fname : parsed_cue->file_order) {
                result.source_paths.push_back((std::filesystem::path(cue_dir) / fname).string());
            }
        } else {
            result.source_paths.push_back(input_path);
        }
    }

    return 0;
}

int ecm3_decode(
    const std::string& input_path,
    const std::string& output_path,
    const ecm_options& opts,
    ecm3_result& result
) {
    int return_code = 0;
    std::vector<blocks_toc> file_blocks_toc;

    std::ifstream in_file(input_path, std::ios::binary);
    if (!in_file.good()) {
        fprintf(stderr, "ERROR: input file cannot be opened.\n");
        return 1;
    }

    if (!opts.verify && !opts.force_rewrite && std::filesystem::exists(output_path)) {
        fprintf(stderr, "ERROR: output file already exists. Use -f to overwrite.\n");
        return 1;
    }

    std::fstream out_file;
    if (opts.verify) {
        out_file.open(
#ifdef _WIN32
            "NUL",
#else
            "/dev/null",
#endif
            std::ios::out | std::ios::binary
        );
        if (!out_file.good()) {
            fprintf(stderr, "ERROR: cannot open null output device.\n");
            return 1;
        }
    } else {
        out_file.open(output_path, std::ios::out | std::ios::binary);
        if (!out_file.good()) {
            fprintf(stderr, "ERROR: output file cannot be opened.\n");
            return 1;
        }
    }

    uint64_t toc_position = 0;
    block_header toc_block_header;

    in_file.seekg(4, std::ios_base::beg);
    {
        uint8_t toc_pos_buf[8];
        in_file.read(reinterpret_cast<char*>(toc_pos_buf), sizeof(toc_pos_buf));
        if (!in_file.good()) {
            fprintf(stderr, "ERROR: Failed to read TOC position.\n");
            return ECMTOOL_CORRUPTED_STREAM;
        }
        toc_position = sector_tools::get64lsb(toc_pos_buf);
    }

    in_file.seekg(toc_position, std::ios_base::beg);
    {
        uint8_t bh_buf[ECM_BLOCK_HEADER_SIZE];
        in_file.read(reinterpret_cast<char*>(bh_buf), sizeof(bh_buf));
        if (!in_file.good()) {
            fprintf(stderr, "ERROR: Failed to read TOC block header.\n");
            return ECMTOOL_CORRUPTED_STREAM;
        }
        deserialize_block_header(bh_buf, toc_block_header);
    }

    {
        size_t toc_count = toc_block_header.real_block_size / ECM_BLOCKS_TOC_SIZE;
        if (toc_count == 0 || toc_count > 1000) {
            fprintf(stderr, "ERROR: Invalid TOC block count (%zu).\n", toc_count);
            return ECMTOOL_CORRUPTED_STREAM;
        }
        file_blocks_toc.resize(toc_count);
        std::vector<uint8_t> toc_data(toc_block_header.real_block_size);
        in_file.read(reinterpret_cast<char*>(toc_data.data()), toc_data.size());
        if (!in_file.good()) {
            fprintf(stderr, "ERROR: Failed to read TOC data.\n");
            return ECMTOOL_CORRUPTED_STREAM;
        }
        for (size_t i = 0; i < toc_count; i++) {
            deserialize_blocks_toc(toc_data.data() + i * ECM_BLOCKS_TOC_SIZE, file_blocks_toc[i]);
        }
    }

    for (size_t i = 0; i < file_blocks_toc.size(); i++) {
        if (file_blocks_toc[i].type == ECMFILE_BLOCK_TYPE_ECM) {
            in_file.seekg(file_blocks_toc[i].start_position, std::ios_base::beg);
            try {
                ecm_options mut_opts = opts;
                return_code = ecm_block_to_image(in_file, out_file, &mut_opts);
            } catch (const std::exception& e) {
                fprintf(stderr, "\n\nERROR: Corrupt or invalid ECM3 file: %s\n\n", e.what());
                return_code = ECMTOOL_CORRUPTED_STREAM;
            } catch (...) {
                fprintf(stderr, "\n\nERROR: Corrupt or invalid ECM3 file.\n\n");
                return_code = ECMTOOL_CORRUPTED_STREAM;
            }
            if (return_code) { break; }
        }
        else if (file_blocks_toc[i].type == ECMFILE_BLOCK_TYPE_METADATA) {
            if (read_metadata_block(in_file, file_blocks_toc[i].start_position, result.meta_header, result.meta_entries) == 0) {
                result.has_metadata = true;
            }
        }
    }

    if (opts.delete_source && return_code == 0) {
        result.source_paths.push_back(input_path);
    }

    return return_code;
}