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

#pragma once

#include "ecm3.h"
#include "cue_parser.h"
#include <string>
#include <vector>
#include <functional>

struct ecm3_result {
    track_metadata_header meta_header = {};
    std::vector<track_metadata_entry> meta_entries;
    bool has_metadata = false;
    uint64_t total_sectors = 0;
    std::vector<uint32_t> sectors_type_summary;
    std::vector<stream_script> encode_streams_script;
    std::vector<std::string> source_paths;
};

int ecm3_encode(
    const std::string& input_path,
    const std::string& output_path,
    const ecm_options& opts,
    const cue_sheet* parsed_cue,
    bool has_cue,
    ecm3_result& result
);

int ecm3_decode(
    const std::string& input_path,
    const std::string& output_path,
    const ecm_options& opts,
    ecm3_result& result
);