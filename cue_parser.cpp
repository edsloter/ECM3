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

#include "cue_parser.h"
#include "metadata.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <filesystem>

uint32_t cue_msf_to_sector(const std::string& msf) {
    int minutes = 0, seconds = 0, frames = 0;
    char colon1 = 0, colon2 = 0;
    sscanf(msf.c_str(), "%d%c%d%c%d", &minutes, &colon1, &seconds, &colon2, &frames);
    return (minutes * 60 + seconds) * 75 + frames;
}

std::string cue_sector_to_msf(uint32_t sector) {
    uint32_t frames = sector % 75;
    uint32_t total_seconds = sector / 75;
    uint32_t seconds = total_seconds % 60;
    uint32_t minutes = total_seconds / 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", minutes, seconds, frames);
    return std::string(buf);
}

cue_track_mode cue_parse_track_mode(const std::string& mode_str, const std::string& sector_size) {
    std::string mode_lower = mode_str;
    std::transform(mode_lower.begin(), mode_lower.end(), mode_lower.begin(), ::tolower);

    if (mode_lower == "audio") return CUE_MODE_AUDIO;

    std::string size_lower = sector_size;
    std::transform(size_lower.begin(), size_lower.end(), size_lower.begin(), ::tolower);
    std::string combined = mode_lower + "/" + size_lower;

    if (combined == "mode1/2048") return CUE_MODE_MODE1_2048;
    if (combined == "mode1/2352") return CUE_MODE_MODE1_2352;
    if (combined == "mode2/2336") return CUE_MODE_MODE2_2336;
    if (combined == "mode2/2352") return CUE_MODE_MODE2_2352;
    return CUE_MODE_AUDIO;
}

int cue_parse(const std::string& filename, cue_sheet& sheet) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "ERROR: Cannot open cue file: %s\n", filename.c_str());
        return 1;
    }

    sheet = cue_sheet();
    cue_track* current_track = nullptr;
    std::string current_file;
    uint64_t current_file_offset = 0;

    std::string line;
    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;

        std::string orig_line = line;
        std::transform(line.begin(), line.end(), line.begin(), ::tolower);

        if (line.compare(0, 4, "file") == 0) {
            size_t quote1 = orig_line.find('"');
            if (quote1 == std::string::npos) continue;
            size_t quote2 = orig_line.find('"', quote1 + 1);
            if (quote2 == std::string::npos) continue;
            std::string ref_filename = orig_line.substr(quote1 + 1, quote2 - quote1 - 1);
            std::string file_type = line.substr(quote2 + 1);
            while (!file_type.empty() && file_type.front() == ' ') file_type.erase(file_type.begin());
            current_file = ref_filename;
            current_file_offset = 0;
            bool found = false;
            for (size_t i = 0; i < sheet.file_order.size(); i++) {
                if (sheet.file_order[i] == current_file) {
                    found = true;
                    break;
                }
            }
            if (!found) sheet.file_order.push_back(current_file);
        }
        else if (line.compare(0, 5, "track") == 0) {
            int track_num = 0;
            char mode_str[32] = {0};
            char size_str[32] = {0};
            sscanf(line.c_str() + 5, " %d %31[^/]/%31s", &track_num, mode_str, size_str);
            cue_track track;
            track.number = (uint8_t)track_num;
            track.mode = cue_parse_track_mode(mode_str, size_str);
            track.flags = CUE_FLAG_NONE;
            track.file_reference = current_file;
            track.file_byte_offset = 0;
            sheet.tracks.push_back(track);
            current_track = &sheet.tracks.back();
        }
        else if (line.compare(0, 5, "index") == 0 && current_track) {
            int index_num = 0;
            char msf[16] = {0};
            sscanf(line.c_str() + 5, " %d %15s", &index_num, msf);
            cue_index idx;
            idx.number = (uint8_t)index_num;
            if (strcmp(msf, "00:00:00") == 0 && current_track->number > 1) {
                idx.sector_offset = 0;
            } else {
                idx.sector_offset = cue_msf_to_sector(msf);
            }
            current_track->indices.push_back(idx);
        }
        else if (line.compare(0, 4, "isrc") == 0 && current_track) {
            size_t isrc_start = orig_line.find_first_not_of(" \t", 4);
            if (isrc_start != std::string::npos) {
                std::string isrc = orig_line.substr(isrc_start);
                current_track->isrc = isrc;
            }
        }
        else if (line.compare(0, 4, "flag") == 0 && current_track) {
            if (line.find("dcp") != std::string::npos) current_track->flags |= CUE_FLAG_DCP;
            if (line.find("4ch") != std::string::npos) current_track->flags |= CUE_FLAG_4CH;
            if (line.find("pre") != std::string::npos) current_track->flags |= CUE_FLAG_PRE;
            if (line.find("scms") != std::string::npos) current_track->flags |= CUE_FLAG_SCMS;
        }
        else if (line.compare(0, 7, "catalog") == 0) {
            size_t cat_start = orig_line.find_first_not_of(" \t", 7);
            if (cat_start != std::string::npos) {
                sheet.catalog = orig_line.substr(cat_start);
            }
        }
        else if (line.compare(0, 8, "pregap") == 0 && current_track) {
            char msf[16] = {0};
            sscanf(line.c_str() + 7, " %15s", msf);
            current_track->flags |= CUE_FLAG_HAS_PREGAP;
        }
        else if (line.compare(0, 8, "postgap") == 0 && current_track) {
            current_track->flags |= CUE_FLAG_HAS_POSTGAP;
        }
        else if (line.compare(0, 7, "cdtextfile") == 0) {
            size_t q1 = orig_line.find('"');
            if (q1 != std::string::npos) {
                size_t q2 = orig_line.find('"', q1 + 1);
                sheet.cd_text_file = orig_line.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }

    if (sheet.tracks.empty()) {
        fprintf(stderr, "ERROR: No tracks found in cue file: %s\n", filename.c_str());
        return 1;
    }

    for (auto& track : sheet.tracks) {
        if (!(track.flags & CUE_FLAG_HAS_PREGAP)) {
            uint32_t idx00 = UINT32_MAX;
            uint32_t idx01 = UINT32_MAX;
            for (const auto& idx : track.indices) {
                if (idx.number == 0) idx00 = idx.sector_offset;
                if (idx.number == 1) idx01 = idx.sector_offset;
            }
            if (idx00 != UINT32_MAX && idx01 != UINT32_MAX && idx01 > idx00) {
                track.flags |= CUE_FLAG_HAS_PREGAP;
            }
        }
    }

    return 0;
}

std::string cue_mode_to_string(cue_track_mode mode) {
    switch (mode) {
        case CUE_MODE_AUDIO: return "AUDIO";
        case CUE_MODE_MODE1_2048: return "MODE1/2048";
        case CUE_MODE_MODE1_2352: return "MODE1/2352";
        case CUE_MODE_MODE2_2336: return "MODE2/2336";
        case CUE_MODE_MODE2_2352: return "MODE2/2352";
        default: return "MODE1/2352";
    }
}

int write_cue_file(const std::string& path, const cue_sheet& sheet, bool split_output, const std::string& bin_filename) {
    std::ofstream cue_file(path, std::ios::binary);
    if (!cue_file.good()) {
        fprintf(stderr, "ERROR: Could not create CUE file: %s\n", path.c_str());
        return 1;
    }

    if (!sheet.catalog.empty()) {
        cue_file << "CATALOG " << sheet.catalog << "\r\n";
    }
    if (!sheet.cd_text_file.empty()) {
        cue_file << "CDTEXTFILE \"" << sheet.cd_text_file << "\"\r\n";
    }

    if (split_output && sheet.tracks.size() > 1) {
        // Per-track FILE entries
        for (size_t i = 0; i < sheet.tracks.size(); i++) {
            const cue_track& trk = sheet.tracks[i];
            char track_buf[32];
            snprintf(track_buf, sizeof(track_buf), sheet.tracks.size() > 9 ? "%02u" : "%u", trk.number);
            std::string track_filename = get_basename(bin_filename) + " (Track " + track_buf + ").bin";

            cue_file << "FILE \"" << track_filename << "\" BINARY\r\n";
            cue_file << "  TRACK " << std::setw(2) << std::setfill('0') << (int)trk.number
                     << " " << cue_mode_to_string(trk.mode) << "\r\n";

            if (trk.flags & CUE_FLAG_DCP)  cue_file << "    FLAGS DCP\r\n";
            if (trk.flags & CUE_FLAG_4CH)  cue_file << "    FLAGS 4CH\r\n";
            if (trk.flags & CUE_FLAG_PRE)  cue_file << "    FLAGS PRE\r\n";
            if (trk.flags & CUE_FLAG_SCMS) cue_file << "    FLAGS SCMS\r\n";

            if (!trk.isrc.empty()) {
                cue_file << "    ISRC " << trk.isrc << "\r\n";
            }

            // Calculate relative INDEX positions
            uint32_t idx00 = UINT32_MAX, idx01 = UINT32_MAX;
            for (const auto& idx : trk.indices) {
                if (idx.number == 0) idx00 = idx.sector_offset;
                if (idx.number == 1) idx01 = idx.sector_offset;
            }

            if (idx00 != UINT32_MAX && idx01 != UINT32_MAX && idx01 > idx00) {
                uint32_t pregap = idx01 - idx00;
                cue_file << "    INDEX 00 " << cue_sector_to_msf(0) << "\r\n";
                cue_file << "    INDEX 01 " << cue_sector_to_msf(pregap) << "\r\n";
            } else if (idx01 != UINT32_MAX) {
                cue_file << "    INDEX 01 " << cue_sector_to_msf(0) << "\r\n";
            }

            if (trk.flags & CUE_FLAG_HAS_POSTGAP) {
                cue_file << "    POSTGAP\r\n";
            }
        }
    } else {
        // Single FILE entry (combined)
        cue_file << "FILE \"" << std::filesystem::path(bin_filename).filename().string() << "\" BINARY\r\n";
        for (size_t i = 0; i < sheet.tracks.size(); i++) {
            const cue_track& trk = sheet.tracks[i];
            cue_file << "  TRACK " << std::setw(2) << std::setfill('0') << (int)trk.number
                     << " " << cue_mode_to_string(trk.mode) << "\r\n";

            if (trk.flags & CUE_FLAG_DCP)  cue_file << "    FLAGS DCP\r\n";
            if (trk.flags & CUE_FLAG_4CH)  cue_file << "    FLAGS 4CH\r\n";
            if (trk.flags & CUE_FLAG_PRE)  cue_file << "    FLAGS PRE\r\n";
            if (trk.flags & CUE_FLAG_SCMS) cue_file << "    FLAGS SCMS\r\n";

            if (!trk.isrc.empty()) {
                cue_file << "    ISRC " << trk.isrc << "\r\n";
            }

            uint32_t idx00 = UINT32_MAX, idx01 = UINT32_MAX;
            for (const auto& idx : trk.indices) {
                if (idx.number == 0) idx00 = idx.sector_offset;
                if (idx.number == 1) idx01 = idx.sector_offset;
            }

            if (idx00 != UINT32_MAX && idx01 != UINT32_MAX && idx01 > idx00) {
                cue_file << "    INDEX 00 " << cue_sector_to_msf(idx00) << "\r\n";
                cue_file << "    INDEX 01 " << cue_sector_to_msf(idx01) << "\r\n";
            } else if (idx01 != UINT32_MAX) {
                cue_file << "    INDEX 01 " << cue_sector_to_msf(idx01) << "\r\n";
            }

            if (trk.flags & CUE_FLAG_HAS_POSTGAP) {
                cue_file << "    POSTGAP\r\n";
            }
        }
    }

    cue_file.close();
    printf("Created CUE file: %s\n", path.c_str());
    return 0;
}

uint32_t cue_sheet::total_sectors() const {
    if (tracks.empty()) return 0;
    const cue_track& last = tracks.back();
    if (last.indices.empty()) return 0;
    return last.indices.back().sector_offset;
}