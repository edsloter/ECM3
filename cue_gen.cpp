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

#include "cue_gen.h"

cue_sheet generate_cue_from_sectors(
    const std::vector<stream_script>& streams_script
) {
    cue_sheet syn_cue;
    syn_cue.file_order.push_back("synthetic.bin");

    auto sector_to_cue = [](uint8_t mode) -> cue_track_mode {
        switch ((sector_tools_types)mode) {
            case STT_CDDA: case STT_CDDA_GAP: return CUE_MODE_AUDIO;
            case STT_MODE1: case STT_MODE1_GAP: case STT_MODE1_RAW: return CUE_MODE_MODE1_2352;
            case STT_MODE2: case STT_MODE2_GAP: case STT_MODE2_1: case STT_MODE2_1_GAP:
            case STT_MODE2_2: case STT_MODE2_2_GAP: case STT_MODEX: return CUE_MODE_MODE2_2352;
            default: return CUE_MODE_MODE1_2352;
        }
    };
    auto is_gap = [](uint8_t mode) -> bool {
        switch ((sector_tools_types)mode) {
            case STT_CDDA_GAP: case STT_MODE1_GAP: case STT_MODE2_GAP:
            case STT_MODE2_1_GAP: case STT_MODE2_2_GAP: return true;
            default: return false;
        }
    };

    struct type_run { cue_track_mode mode; uint32_t count; bool gap; };
    std::vector<type_run> runs;
    for (auto& s : streams_script) {
        for (auto& sec : s.sectors_data) {
            cue_track_mode m = sector_to_cue(sec.mode);
            bool g = is_gap(sec.mode);
            if (!runs.empty() && runs.back().mode == m && runs.back().gap == g) {
                runs.back().count += sec.sector_count;
            } else {
                runs.push_back({m, sec.sector_count, g});
            }
        }
    }

    uint32_t pos = 0;
    uint8_t tnum = 1;
    for (size_t i = 0; i < runs.size(); i++) {
        if (runs[i].gap) {
            pos += runs[i].count;
            continue;
        }
        cue_track trk;
        trk.number = tnum++;
        trk.mode = runs[i].mode;
        trk.flags = 0;
        trk.file_reference = "synthetic.bin";
        trk.file_byte_offset = 0;
        cue_index idx01;
        idx01.number = 1;
        idx01.sector_offset = pos;
        trk.indices.push_back(idx01);
        pos += runs[i].count;
        while (i + 1 < runs.size() && runs[i + 1].mode == runs[i].mode) {
            i++;
            pos += runs[i].count;
        }
        syn_cue.tracks.push_back(trk);
    }

    return syn_cue;
}
