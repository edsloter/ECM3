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

#include "cuesplit.h"
#include "cue_parser.h"
#include "metadata.h"
#include "util.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <iomanip>

// ── Split a combined .cue (single FILE) into per-track .bin + .cue ──

int cue_cmd_split(const std::string& cue_path, const std::string& output_dir, bool force_rewrite) {
    namespace fs = std::filesystem;

    cue_sheet sheet;
    if (cue_parse(cue_path, sheet) != 0) {
        std::cerr << "ERROR: Failed to parse CUE file: " << cue_path << "\n";
        return 1;
    }

    if (sheet.file_order.size() != 1) {
        std::cerr << "ERROR: CUE has " << sheet.file_order.size()
                  << " FILE entries; expected 1 (combined format).\n";
        return 1;
    }

    std::string cue_dir = get_cue_dir(cue_path);
    fs::path bin_path = fs::path(cue_dir) / sheet.file_order[0];

    std::ifstream bin_file(bin_path, std::ios::binary);
    if (!bin_file.good()) {
        std::cerr << "ERROR: Cannot open BIN file: " << bin_path.string() << "\n";
        return 1;
    }
    bin_file.seekg(0, std::ios::end);
    uint64_t bin_size = (uint64_t)bin_file.tellg();
    bin_file.seekg(0, std::ios::beg);

    if (bin_size % 2352 != 0) {
        std::cerr << "ERROR: BIN file size " << (unsigned long long)bin_size
                  << " is not a multiple of 2352.\n";
        return 1;
    }
    uint64_t total_sectors = bin_size / 2352;

    std::cout << "Splitting combined image: " << sheet.tracks.size() << " track(s) from "
              << sheet.file_order[0] << " (" << (unsigned long long)total_sectors << " sectors, "
              << std::fixed << std::setprecision(1) << (bin_size / (1024.0 * 1024.0)) << " MB)\n";

    // Verify each track has valid INDEX entries
    for (size_t i = 0; i < sheet.tracks.size(); i++) {
        const cue_track& trk = sheet.tracks[i];
        // Find first INDEX (00 or 01)
        if (trk.indices.empty()) {
            std::cerr << "ERROR: Track " << (unsigned)trk.number << " has no INDEX entries.\n";
            return 1;
        }
        uint32_t first_idx_sector = trk.indices[0].sector_offset;
        if (first_idx_sector >= total_sectors) {
            std::cerr << "ERROR: Track " << (unsigned)trk.number
                      << " INDEX starts at sector " << first_idx_sector
                      << " but image has only " << (unsigned long long)total_sectors << " sectors.\n";
            return 1;
        }
    }

    // Single-track discs don't get split (matches decode split behavior)
    if (sheet.tracks.size() <= 1) {
        std::cout << "Single track disc, no split needed.\n";
        return 0;
    }

    // Ensure output directory exists
    fs::create_directories(output_dir);

    // Compute and write per-track files
    std::string base_name = get_basename(bin_path.string());

    for (size_t i = 0; i < sheet.tracks.size(); i++) {
        const cue_track& trk = sheet.tracks[i];

        uint32_t track_start = trk.indices[0].sector_offset;

        uint32_t track_end;
        if (i + 1 < sheet.tracks.size()) {
            track_end = sheet.tracks[i + 1].indices[0].sector_offset - 1;
        } else {
            track_end = (uint32_t)(total_sectors - 1);
        }

        uint64_t byte_offset = (uint64_t)track_start * 2352;
        uint64_t sector_count = (uint64_t)(track_end - track_start + 1);
        uint64_t byte_count = sector_count * 2352;

        char track_buf[4];
        snprintf(track_buf, sizeof(track_buf), sheet.tracks.size() > 9 ? "%02u" : "%u", trk.number);
        std::string track_name = base_name + " (Track " + track_buf + ")";
        std::string track_outpath = (fs::path(output_dir) / (track_name + ".bin")).string();

        if (!force_rewrite && fs::exists(track_outpath)) {
            std::cerr << "ERROR: Output file already exists: " << track_outpath
                      << " (use -f to overwrite)\n";
            return 1;
        }

        bin_file.seekg((std::streamoff)byte_offset);
        std::vector<uint8_t> buf((size_t)byte_count);
        bin_file.read((char*)buf.data(), (std::streamsize)byte_count);

        std::ofstream out(track_outpath, std::ios::binary);
        if (!out.good()) {
            std::cerr << "ERROR: Cannot create output file: " << track_outpath << "\n";
            return 1;
        }
        out.write((char*)buf.data(), (std::streamsize)byte_count);
        out.close();

        std::cout << "  Track " << track_buf << ": " << track_name << ".bin"
                  << " (" << (unsigned long long)sector_count << " sectors, "
                  << std::fixed << std::setprecision(1) << (byte_count / (1024.0 * 1024.0)) << " MB)\n";
    }
    bin_file.close();

    // Write the split CUE
    std::string out_cue_path = (fs::path(output_dir) / (base_name + ".cue")).string();
    write_cue_file(out_cue_path, sheet, true, bin_path.string());

    std::cout << "Split CUE: " << out_cue_path << "\n";
    std::cout << "Split complete: " << sheet.tracks.size() << " tracks -> " << output_dir << "\n";
    return 0;
}

// ── Combine per-track .bin files (multi-FILE .cue) into a single .bin + .cue ──

int cue_cmd_combine(const std::string& cue_path, const std::string& output_dir, bool force_rewrite) {
    namespace fs = std::filesystem;

    cue_sheet sheet;
    if (cue_parse(cue_path, sheet) != 0) {
        std::cerr << "ERROR: Failed to parse CUE file: " << cue_path << "\n";
        return 1;
    }

    if (sheet.file_order.size() <= 1) {
        std::cerr << "ERROR: CUE has only " << sheet.file_order.size()
                  << " FILE entry; expected multiple (split format).\n";
        return 1;
    }

    std::string cue_dir = get_cue_dir(cue_path);

    // Determine output base name from first track file
    std::string first_bin = (fs::path(cue_dir) / sheet.file_order[0]).string();
    std::string base_name = get_basename(first_bin);
    // Strip " (Track NN)" suffix if present
    {
        std::string lower_name = base_name;
        for (auto& c : lower_name) c = (char)tolower(c);
        size_t pos = lower_name.find(" (track ");
        if (pos != std::string::npos) {
            base_name = base_name.substr(0, pos);
        }
    }

    std::cout << "Combining " << sheet.file_order.size() << " track file(s) from: "
              << cue_path << "\n";

    // Ensure output directory exists
    fs::create_directories(output_dir);

    std::string out_bin_path = (fs::path(output_dir) / (base_name + ".bin")).string();
    std::string out_cue_path = (fs::path(output_dir) / (base_name + ".cue")).string();

    if (!force_rewrite && fs::exists(out_bin_path)) {
        std::cerr << "ERROR: Output file already exists: " << out_bin_path
                  << " (use -f to overwrite)\n";
        return 1;
    }
    if (!force_rewrite && fs::exists(out_cue_path)) {
        std::cerr << "ERROR: Output file already exists: " << out_cue_path
                  << " (use -f to overwrite)\n";
        return 1;
    }

    std::ofstream out_bin(out_bin_path, std::ios::binary);
    if (!out_bin.good()) {
        std::cerr << "ERROR: Cannot create output BIN file: " << out_bin_path << "\n";
        return 1;
    }

    // Build a new combined cue_sheet with absolute sector positions
    cue_sheet combined;
    combined.catalog = sheet.catalog;
    combined.cd_text_file = sheet.cd_text_file;
    combined.file_order.push_back(base_name + ".bin");

    uint64_t combined_offset_sectors = 0;
    uint64_t combined_bytes = 0;
    const size_t buf_size = 0x100000;
    std::vector<char> io_buf(buf_size);

    for (size_t f = 0; f < sheet.file_order.size(); f++) {
        const std::string& ref = sheet.file_order[f];
        fs::path bin_path = fs::path(cue_dir) / ref;

        std::ifstream in(bin_path, std::ios::binary);
        if (!in.good()) {
            std::cerr << "ERROR: Cannot open BIN file: " << bin_path.string() << "\n";
            return 1;
        }
        in.seekg(0, std::ios::end);
        uint64_t file_size = (uint64_t)in.tellg();
        in.seekg(0, std::ios::beg);

        if (file_size % 2352 != 0) {
            std::cerr << "ERROR: BIN file is not a multiple of 2352: " << bin_path.string() << "\n";
            return 1;
        }
        uint64_t file_sectors = file_size / 2352;

        // Copy data
        while (in) {
            in.read(io_buf.data(), buf_size);
            std::streamsize n = in.gcount();
            if (n > 0) out_bin.write(io_buf.data(), n);
        }
        in.close();

        // Find tracks belonging to this FILE
        size_t track_count = 0;
        for (size_t t = 0; t < sheet.tracks.size(); t++) {
            if (sheet.tracks[t].file_reference != ref) continue;
            cue_track trk = sheet.tracks[t];

            // Adjust INDEX offsets to absolute positions
            uint32_t offset_adj = (uint32_t)combined_offset_sectors;
            for (auto& idx : trk.indices) {
                // In split CUE, indices are relative to the track file
                // In combined CUE, they are absolute
                // Skip indices already set to absolute position
                idx.sector_offset += offset_adj;
            }
            // file_reference points to the one combined FILE
            trk.file_reference = combined.file_order[0];
            combined.tracks.push_back(trk);
            track_count++;
        }

        std::cout << "  Added: " << ref << " (" << (unsigned long long)file_sectors
                  << " sectors, " << (unsigned long long)file_size << " bytes, "
                  << track_count << " track(s))\n";
        combined_offset_sectors += file_sectors;
        combined_bytes += file_size;
    }
    out_bin.close();

    std::cout << "  Wrote: " << out_bin_path
              << " (" << (unsigned long long)combined_offset_sectors << " sectors, "
              << std::fixed << std::setprecision(1)
              << (combined_bytes / (1024.0 * 1024.0)) << " MB)\n";

    // Write combined CUE
    write_cue_file(out_cue_path, combined, false, out_bin_path);

    std::cout << "Combined CUE: " << out_cue_path << "\n";
    std::cout << "Combine complete: " << sheet.tracks.size() << " tracks -> " << output_dir << "\n";
    return 0;
}
