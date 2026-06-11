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

#include "ecm3.h"
#include "ecm3_core.h"
#include "metadata.h"
#include "util.h"
#include "cue_gen.h"
#include "cuesplit.h"
#include <filesystem>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <functional>

#define LZMA_BUF_ERR 10

static bool is_decompress_error(int8_t rc) {
    if (rc == 0 || rc == 1) return false;
    if (rc == Z_BUF_ERROR) return false;
    if (rc == LZMA_BUF_ERR) return false;
    return true;
}

// Maximum dictionary size for cross-stream compression dictionary sharing.
// Matches zlib's default 32 KB sliding-window size.
static constexpr size_t CROSS_STREAM_DICT_MAX = 32768;

// Append data to a rolling dictionary buffer, keeping only the last
// CROSS_STREAM_DICT_MAX bytes.  Used to capture cleaned/decompressed
// data from one stream so it can be used as a preset dictionary for
// the next stream of the same type (audio / data).
static void update_dict(std::vector<uint8_t>& dict, const uint8_t* data, size_t size) {
    if (size >= CROSS_STREAM_DICT_MAX) {
        dict.assign(data + size - CROSS_STREAM_DICT_MAX, data + size);
    } else if (dict.size() + size > CROSS_STREAM_DICT_MAX) {
        dict.erase(dict.begin(), dict.begin() + (dict.size() + size - CROSS_STREAM_DICT_MAX));
        dict.insert(dict.end(), data, data + size);
    } else {
        dict.insert(dict.end(), data, data + size);
    }
}

#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif



#define METADATA_VERSION 1

class mmap_file {
public:
    mmap_file() : data_(nullptr), size_(0) {}
    ~mmap_file() { unmap(); }

    bool open(const std::string& path) {
        unmap();
#ifdef _WIN32
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(hFile, &file_size)) { CloseHandle(hFile); return false; }
        size_ = (uint64_t)file_size.QuadPart;
        HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (hMap == NULL) { CloseHandle(hFile); return false; }
        data_ = (uint8_t*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(hMap);
        CloseHandle(hFile);
        if (!data_) return false;
        prefetch();
        return true;
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) return false;
        struct stat st;
        if (fstat(fd_, &st) < 0) { ::close(fd_); fd_ = -1; return false; }
        size_ = (uint64_t)st.st_size;
        data_ = (uint8_t*)mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (data_ == MAP_FAILED) { data_ = nullptr; ::close(fd_); fd_ = -1; return false; }
        prefetch();
        return true;
#endif
    }

    void unmap() {
        if (data_) {
#ifdef _WIN32
            UnmapViewOfFile(data_);
#else
            munmap(data_, size_);
            if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
            data_ = nullptr;
        }
        size_ = 0;
    }

    const uint8_t* data() const { return data_; }
    uint64_t size() const { return size_; }
    bool is_open() const { return data_ != nullptr; }

private:
    void prefetch() {
        if (!data_ || size_ == 0) return;
#if defined(_WIN32) && defined(PrefetchVirtualMemory)
        WIN32_MEMORY_RANGE_ENTRY entry;
        entry.VirtualAddress = (PVOID)data_;
        entry.NumberOfBytes = (SIZE_T)size_;
        PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, sizeof(entry));
#elif defined(POSIX_MADV_SEQUENTIAL)
        posix_madvise(data_, size_, POSIX_MADV_SEQUENTIAL);
#elif defined(MADV_SEQUENTIAL)
        madvise(data_, size_, MADV_SEQUENTIAL);
#endif
    }

    uint8_t* data_;
    uint64_t size_;
#ifndef _WIN32
    int fd_ = -1;
#endif
};

// Some necessary variables
static std::atomic<uint64_t> mycounter_progress{0};
static uint64_t mycounter_total = 0;
static uint64_t mycounter_audio_comp_total = 0;
static uint64_t mycounter_data_comp_total = 0;
static uint8_t mycounter_analyze_display = 0;
static uint8_t mycounter_encode_display = 0;
static uint8_t mycounter_decode_display = 0;
static std::atomic<uint64_t> mycounter_audio_comp_progress{0};
static std::atomic<uint64_t> mycounter_data_comp_progress{0};
static uint8_t mycounter_audio_comp_encode_display = 0;
static uint8_t mycounter_data_comp_encode_display = 0;
static std::atomic<uint64_t> mycounter_ecm_progress{0};
static uint8_t mycounter_ecm_encode_display = 0;
static std::atomic<bool> mycounter_is_decode{false};
static bool mycounter_is_verify = false;
static std::chrono::steady_clock::time_point mycounter_start_time;
static sector_tools_compression mycounter_audio_compression = C_NONE;
static sector_tools_compression mycounter_data_compression = C_NONE;
std::atomic<bool> g_interrupted{false};

static progress_cb_t g_progress_cb = nullptr;

void set_progress_callback(progress_cb_t cb) {
    g_progress_cb = cb;
}
static std::string g_output_filename;



#define CHECK_INTERRUPT() do { if (g_interrupted.load(std::memory_order_relaxed)) { fprintf(stderr, "\nInterrupted.\n"); return ECMTOOL_PROCESSING_ERROR; } } while(0)

static void signal_handler(int sig) {
    g_interrupted.store(true, std::memory_order_relaxed);
}

static void setcounter_decode_mode(bool is_decode);
static void resetcounter(uint64_t total, sector_tools_compression audio_comp, sector_tools_compression data_comp, uint64_t audio_comp_total, uint64_t data_comp_total);
static void encode_progress(void);
static void decode_progress(void);
static void setcounter_analyze(uint64_t n);
static void setcounter_encode(uint64_t n);
static void setcounter_audio_comp_encode(uint64_t n);
static void setcounter_data_comp_encode(uint64_t n);
static void setcounter_ecm_encode(uint64_t n);
static void setcounter_decode(uint64_t n);
static const char* compression_name(uint8_t comp);

std::string get_cue_dir(const std::string& cue_path) {
    namespace fs = std::filesystem;
    return fs::path(cue_path).parent_path().string();
}

std::string get_basename(const std::string& filepath) {
    namespace fs = std::filesystem;
    return fs::path(filepath).stem().string();
}

std::string fmt_track_number(uint8_t track_number, size_t total_tracks) {
    char buf[4];
    snprintf(buf, sizeof(buf), total_tracks > 9 ? "%02u" : "%u", track_number);
    return std::string(buf);
}

int concat_split_bins(cue_sheet& sheet, const std::string& cue_dir, temp_file& tf) {
    namespace fs = std::filesystem;

    if (sheet.file_order.size() <= 1) {
        return 0;
    }

    fs::path tmp_dir = fs::temp_directory_path();
    std::string temp_path = (tmp_dir / (get_basename(sheet.file_order[0]) + ".concat.bin")).string();
    tf = temp_file(temp_path);

    scope_guard cleanup([&tf]() { tf.release(); });

    std::ofstream out(temp_path, std::ios::binary);
    if (!out.good()) {
        fprintf(stderr, "ERROR: Cannot create temp file for concatenation: %s\n", temp_path.c_str());
        return 1;
    }

    uint64_t total_offset_sectors = 0;
    for (size_t f = 0; f < sheet.file_order.size(); f++) {
        const std::string& ref = sheet.file_order[f];
        fs::path bin_path = fs::path(cue_dir) / ref;

        std::ifstream in(bin_path.string(), std::ios::binary);
        if (!in.good()) {
            fprintf(stderr, "ERROR: Cannot open BIN file: %s\n", bin_path.string().c_str());
            return 1;
        }

        in.seekg(0, std::ios::end);
        uint64_t file_size = in.tellg();
        in.seekg(0, std::ios::beg);

        if (file_size % 2352 != 0) {
            fprintf(stderr, "ERROR: BIN file is not a multiple of 2352: %s\n", bin_path.string().c_str());
            return 1;
        }

        const size_t buf_size = 0x100000;
        std::vector<char> buffer(buf_size);
        while (in) {
            in.read(buffer.data(), buf_size);
            std::streamsize bytes_read = in.gcount();
            if (bytes_read > 0) {
                out.write(buffer.data(), bytes_read);
            }
        }

        for (size_t t = 0; t < sheet.tracks.size(); t++) {
            if (sheet.tracks[t].file_reference == ref) {
                sheet.tracks[t].file_byte_offset = total_offset_sectors * 2352;
            }
        }

        total_offset_sectors += file_size / 2352;
        in.close();
    }

    out.close();
    cleanup.dismiss();
    return 0;
}

static cue_track_mode sector_mode_to_cue_mode(uint8_t mode) {
    switch (mode) {
        case 0: return CUE_MODE_AUDIO;
        case 1: return CUE_MODE_MODE1_2352;
        case 2: return CUE_MODE_MODE2_2352;
        default: return CUE_MODE_MODE1_2352;
    }
}

static uint8_t cue_mode_to_sector_mode(cue_track_mode mode) {
    switch (mode) {
        case CUE_MODE_AUDIO: return 0;
        case CUE_MODE_MODE1_2048: return 1;
        case CUE_MODE_MODE1_2352: return 1;
        case CUE_MODE_MODE2_2336: return 2;
        case CUE_MODE_MODE2_2352: return 2;
        default: return 1;
    }
}

static uint32_t find_track_index01(const cue_track& trk) {
    for (size_t j = 0; j < trk.indices.size(); j++) {
        if (trk.indices[j].number == 1) {
            return trk.indices[j].sector_offset;
        }
    }
    return 0;
}

void build_metadata_from_cue(
    const cue_sheet& sheet,
    uint64_t total_image_sectors,
    track_metadata_header& meta_header,
    std::vector<track_metadata_entry>& meta_entries
) {
    // DEBUG
    memset(&meta_header, 0, sizeof(meta_header));
    meta_header.version = METADATA_VERSION;
    meta_header.flags = 0;
    meta_header.track_count = (uint16_t)sheet.tracks.size();
    meta_header.total_sectors = (uint32_t)total_image_sectors;
    memset(meta_header.media_catalog_number, 0, sizeof(meta_header.media_catalog_number));

    if (!sheet.catalog.empty() && sheet.catalog.size() <= 13) {
        memcpy(meta_header.media_catalog_number, sheet.catalog.c_str(), sheet.catalog.size());
    }

    meta_entries.resize(sheet.tracks.size());

    // First pass: calculate pregap_sectors for all tracks
    for (size_t i = 0; i < sheet.tracks.size(); i++) {
        const cue_track& trk = sheet.tracks[i];
        meta_entries[i].track_number = trk.number;
        meta_entries[i].track_mode = (uint8_t)trk.mode;
        meta_entries[i].track_flags = trk.flags;

        meta_entries[i].pregap_sectors = 0;
        if (trk.flags & CUE_FLAG_HAS_PREGAP) {
            if (!trk.indices.empty() && trk.indices[0].number == 0 && trk.indices.size() > 1) {
                uint32_t idx0_sector = trk.indices[0].sector_offset;
                uint32_t idx1_sector = trk.indices[1].sector_offset;
                if (idx1_sector > idx0_sector) {
                    meta_entries[i].pregap_sectors = (uint16_t)(idx1_sector - idx0_sector);
                }
            }
        }
    }

    // Second pass: set start_sector and end_sector using pregap-aware boundaries
    for (size_t i = 0; i < sheet.tracks.size(); i++) {
        const cue_track& trk = sheet.tracks[i];
        uint32_t file_offset_sectors = (uint32_t)(trk.file_byte_offset / 2352);
        uint32_t index01_sector = find_track_index01(trk);
        uint32_t pregap = meta_entries[i].pregap_sectors;

        // start_sector: file boundary (different file) or INDEX 01 minus pregap (same file)
        if (i > 0) {
            uint32_t prev_file_offset = (uint32_t)(sheet.tracks[i - 1].file_byte_offset / 2352);
            if (file_offset_sectors != prev_file_offset) {
                meta_entries[i].start_sector = file_offset_sectors;
            } else {
                meta_entries[i].start_sector = file_offset_sectors + index01_sector;
                if (pregap > 0 && meta_entries[i].start_sector >= pregap) {
                    meta_entries[i].start_sector -= pregap;
                }
            }
        } else {
            meta_entries[i].start_sector = file_offset_sectors + index01_sector;
        }

        // end_sector: file boundary (next track in different file),
        // next INDEX 01 minus next pregap (same file), or total - 1 (last track)
        if (i + 1 < sheet.tracks.size()) {
            uint32_t next_file_offset_sectors = (uint32_t)(sheet.tracks[i + 1].file_byte_offset / 2352);
            if (next_file_offset_sectors != file_offset_sectors) {
                meta_entries[i].end_sector = next_file_offset_sectors - 1;
            } else {
                uint32_t next_index01 = find_track_index01(sheet.tracks[i + 1]);
                uint32_t next_pregap = meta_entries[i + 1].pregap_sectors;
                meta_entries[i].end_sector = next_file_offset_sectors + next_index01 - 1;
                if (next_pregap > 0 && meta_entries[i].end_sector >= next_pregap) {
                    meta_entries[i].end_sector -= next_pregap;
                }
            }
        } else {
            meta_entries[i].end_sector = (uint32_t)total_image_sectors - 1;
        }
    }
}

static void build_metadata_from_sectors(
    const std::vector<stream_script>& streams_script,
    uint64_t total_image_sectors,
    track_metadata_header& meta_header,
    std::vector<track_metadata_entry>& meta_entries
) {
    memset(&meta_header, 0, sizeof(meta_header));
    meta_header.version = METADATA_VERSION;
    meta_header.flags = 0;
    meta_header.total_sectors = (uint32_t)total_image_sectors;

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

    meta_entries.clear();
    uint32_t pos = 0;
    uint8_t tnum = 1;
    for (size_t i = 0; i < runs.size(); i++) {
        if (runs[i].gap) {
            pos += runs[i].count;
            continue;
        }
        track_metadata_entry e;
        e.track_number = tnum++;
        e.track_mode = (uint8_t)runs[i].mode;
        e.track_flags = 0;
        e.pregap_sectors = 0;
        e.start_sector = pos;
        uint32_t end = pos + runs[i].count - 1;
        pos += runs[i].count;
        while (i + 1 < runs.size() && runs[i + 1].mode == runs[i].mode && !runs[i + 1].gap) {
            i++;
            end = pos + runs[i].count - 1;
            pos += runs[i].count;
        }
        e.end_sector = end;
        meta_entries.push_back(e);
    }
    meta_header.track_count = (uint16_t)meta_entries.size();
}

int write_metadata_block(
    std::fstream& out_file,
    const track_metadata_header& meta_header,
    const std::vector<track_metadata_entry>& meta_entries,
    std::vector<blocks_toc>& file_blocks_toc
) {
    uint64_t block_start = out_file.tellp();

    file_blocks_toc.push_back(blocks_toc());
    file_blocks_toc.back().type = ECMFILE_BLOCK_TYPE_METADATA;
    file_blocks_toc.back().start_position = block_start;

    block_header hdr;
    hdr.type = ECMFILE_BLOCK_TYPE_METADATA;
    hdr.compression = 0;
    hdr.real_block_size = ECM_TRACK_METADATA_HEADER_SIZE + meta_entries.size() * ECM_TRACK_METADATA_ENTRY_SIZE;
    hdr.block_size = hdr.real_block_size;

    uint8_t bh_buf[ECM_BLOCK_HEADER_SIZE];
    serialize_block_header(bh_buf, hdr);
    out_file.write(reinterpret_cast<char*>(bh_buf), sizeof(bh_buf));
    if (!out_file.good()) { return 1; }

    uint8_t hdr_buf[ECM_TRACK_METADATA_HEADER_SIZE] = {};
    hdr_buf[0] = meta_header.version;
    hdr_buf[1] = meta_header.flags;
    sector_tools::put16lsb(hdr_buf + 2, meta_header.track_count);
    sector_tools::put32lsb(hdr_buf + 4, meta_header.total_sectors);
    memcpy(hdr_buf + 8, meta_header.media_catalog_number, 13);
    out_file.write(reinterpret_cast<char*>(hdr_buf), sizeof(hdr_buf));
    if (!out_file.good()) { return 1; }

    for (size_t i = 0; i < meta_entries.size(); i++) {
        const track_metadata_entry& entry = meta_entries[i];
        uint8_t ent_buf[ECM_TRACK_METADATA_ENTRY_SIZE] = {};
        ent_buf[0] = entry.track_number;
        ent_buf[1] = entry.track_mode;
        ent_buf[2] = entry.track_flags;
        sector_tools::put32lsb(ent_buf + 3, entry.start_sector);
        sector_tools::put32lsb(ent_buf + 7, entry.end_sector);
        sector_tools::put16lsb(ent_buf + 11, entry.pregap_sectors);
        out_file.write(reinterpret_cast<char*>(ent_buf), sizeof(ent_buf));
        if (!out_file.good()) { return 1; }
    }

    return 0;
}

int read_metadata_block(
    std::ifstream& in_file,
    uint64_t block_start,
    track_metadata_header& meta_header,
    std::vector<track_metadata_entry>& meta_entries
) {
    in_file.seekg(block_start, std::ios_base::beg);

    uint8_t bh_buf[ECM_BLOCK_HEADER_SIZE];
    in_file.read(reinterpret_cast<char*>(bh_buf), sizeof(bh_buf));
    if (!in_file.good()) { return 1; }

    block_header hdr;
    deserialize_block_header(bh_buf, hdr);

    if (hdr.type != ECMFILE_BLOCK_TYPE_METADATA) {
        fprintf(stderr, "ERROR: Expected metadata block, got type %d\n", hdr.type);
        return 1;
    }

    uint8_t hdr_buf[ECM_TRACK_METADATA_HEADER_SIZE] = {};
    in_file.read(reinterpret_cast<char*>(hdr_buf), sizeof(hdr_buf));
    if (!in_file.good()) { return 1; }

    memset(&meta_header, 0, sizeof(meta_header));
    meta_header.version = hdr_buf[0];
    meta_header.flags = hdr_buf[1];
    meta_header.track_count = sector_tools::get16lsb(hdr_buf + 2);
    meta_header.total_sectors = sector_tools::get32lsb(hdr_buf + 4);
    memcpy(meta_header.media_catalog_number, hdr_buf + 8, 13);

    if (meta_header.version > METADATA_VERSION) {
        fprintf(stderr, "ERROR: Unsupported metadata version %d\n", meta_header.version);
        return 1;
    }

    meta_entries.resize(meta_header.track_count);
    if (meta_header.track_count > 0) {
        for (size_t i = 0; i < meta_header.track_count; i++) {
            uint8_t ent_buf[ECM_TRACK_METADATA_ENTRY_SIZE] = {};
            in_file.read(reinterpret_cast<char*>(ent_buf), sizeof(ent_buf));
            if (!in_file.good()) { return 1; }
            meta_entries[i].track_number = ent_buf[0];
            meta_entries[i].track_mode = ent_buf[1];
            meta_entries[i].track_flags = ent_buf[2];
            meta_entries[i].start_sector = sector_tools::get32lsb(ent_buf + 3);
            meta_entries[i].end_sector = sector_tools::get32lsb(ent_buf + 7);
            meta_entries[i].pregap_sectors = sector_tools::get16lsb(ent_buf + 11);
        }
    }

    return 0;
}

void write_cue_from_metadata(
    const track_metadata_header& meta_header,
    const std::vector<track_metadata_entry>& meta_entries,
    const std::string& out_path,
    bool split_output
) {
    namespace fs = std::filesystem;
    std::string base_name = get_basename(out_path);
    fs::path out_dir = fs::path(out_path).parent_path();
    fs::path cue_path_obj = fs::path(out_path);
    cue_path_obj.replace_extension(".cue");
    std::string cue_path = cue_path_obj.string();
    std::ofstream cue_file(cue_path, std::ios::binary);

    if (!cue_file.good()) {
        fprintf(stderr, "WARNING: Could not create CUE file: %s\n", cue_path.c_str());
        return;
    }

    if (!meta_header.media_catalog_number[0]) {
        // no catalog
    } else {
        char catalog[14] = {};
        memcpy(catalog, meta_header.media_catalog_number, 13);
        cue_file << "CATALOG " << catalog << "\r\n";
    }

    if (split_output && meta_entries.size() > 1) {
        for (size_t i = 0; i < meta_entries.size(); i++) {
            const track_metadata_entry& entry = meta_entries[i];
            std::string track_filename = base_name + " (Track " + fmt_track_number(entry.track_number, meta_entries.size()) + ").bin";

            cue_file << "FILE \"" << track_filename << "\" BINARY\r\n";
            cue_file << "  TRACK " << std::setw(2) << std::setfill('0') << (int)entry.track_number
                     << " " << cue_mode_to_string((cue_track_mode)entry.track_mode) << "\r\n";

            uint32_t pregap = entry.pregap_sectors;

            if (pregap > 0) {
                cue_file << "    INDEX 00 " << cue_sector_to_msf(0) << "\r\n";
                cue_file << "    INDEX 01 " << cue_sector_to_msf(pregap) << "\r\n";
            } else {
                cue_file << "    INDEX 01 " << cue_sector_to_msf(0) << "\r\n";
            }
        }
    } else {
        std::string bin_filename = fs::path(out_path).filename().string();
        cue_file << "FILE \"" << bin_filename << "\" BINARY\r\n";

        for (size_t i = 0; i < meta_entries.size(); i++) {
            const track_metadata_entry& entry = meta_entries[i];

            cue_file << "  TRACK " << std::setw(2) << std::setfill('0') << (int)entry.track_number
                     << " " << cue_mode_to_string((cue_track_mode)entry.track_mode) << "\r\n";

            uint32_t start = entry.start_sector;

            if (entry.pregap_sectors > 0) {
                cue_file << "    INDEX 00 " << cue_sector_to_msf(start) << "\r\n";
                cue_file << "    INDEX 01 " << cue_sector_to_msf(start + entry.pregap_sectors) << "\r\n";
            } else {
                cue_file << "    INDEX 01 " << cue_sector_to_msf(start) << "\r\n";
            }
        }
    }

    cue_file.close();
    fprintf(stdout, "Created CUE file: %s\n", cue_path.c_str());
}

int split_output_bin(
    const std::string& out_path,
    const track_metadata_header& meta_header,
    const std::vector<track_metadata_entry>& meta_entries,
    uint64_t total_image_sectors,
    bool force_rewrite
) {
    if (meta_entries.size() <= 1) {
        return 0;
    }

    namespace fs = std::filesystem;
    std::string base_name = get_basename(out_path);
    fs::path out_dir = fs::path(out_path).parent_path();

    std::ifstream big_bin(out_path, std::ios::binary);
    if (!big_bin.good()) {
        fprintf(stderr, "ERROR: Cannot open output BIN for splitting: %s\n", out_path.c_str());
        return 1;
    }

    const size_t buf_size = 0x100000;
    std::vector<char> buffer(buf_size);

    for (size_t i = 0; i < meta_entries.size(); i++) {
        const track_metadata_entry& entry = meta_entries[i];
        uint32_t start_sector = entry.start_sector;
        uint32_t end_sector = entry.end_sector;
        uint32_t sector_count = end_sector - start_sector + 1;
        uint64_t byte_count = (uint64_t)sector_count * 2352;
        uint64_t byte_offset = (uint64_t)start_sector * 2352;

        std::string track_filename = base_name + " (Track " + fmt_track_number(entry.track_number, meta_entries.size()) + ").bin";
        fs::path track_path = out_dir / track_filename;

        if (!force_rewrite && fs::exists(track_path)) {
            fprintf(stderr, "ERROR: Track file already exists: %s. Use -f to overwrite.\n", track_path.string().c_str());
            big_bin.close();
            return 1;
        }

        std::ofstream track_out(track_path.string(), std::ios::binary);
        if (!track_out.good()) {
            fprintf(stderr, "ERROR: Cannot create track file: %s\n", track_path.string().c_str());
            big_bin.close();
            return 1;
        }

        big_bin.seekg(byte_offset, std::ios::beg);
        uint64_t remaining = byte_count;
        while (remaining > 0) {
            size_t to_read = (size_t)std::min((uint64_t)buf_size, remaining);
            big_bin.read(buffer.data(), to_read);
            std::streamsize bytes_read = big_bin.gcount();
            if (bytes_read <= 0) break;
            track_out.write(buffer.data(), bytes_read);
            remaining -= bytes_read;
        }
        track_out.close();
        fprintf(stdout, "Created track file: %s (%u sectors)\n", track_path.string().c_str(), sector_count);
    }

    big_bin.close();
    return 0;
}


static struct option long_options[] = {
    {"input", required_argument, NULL, 'i'},
    {"output", required_argument, NULL, 'o'},
    {"cue", required_argument, NULL, 2},
    {"acompression", required_argument, NULL, 'a'},
    {"dcompression", required_argument, NULL, 'd'},
    {"clevel", required_argument, NULL, 'c'},
    {"extreme-compression", no_argument, NULL, 'e'},
    {"seekable", no_argument, NULL, 's'},
    {"sectors-per-block", required_argument, NULL, 'p'},
    {"force", no_argument, NULL, 'f'},
    {"keep-output", no_argument, NULL, 'k'},
    {"verify", no_argument, NULL, 'V'},
    {"split", no_argument, NULL, 'S'},
    {"jobs", required_argument, NULL, 'j'},
    {"help", no_argument, NULL, 'h'},
    {"batch-cue", required_argument, NULL, 3},
    {"batch-decode", required_argument, NULL, 4},
    {"delete-source", no_argument, NULL, 5},
    {"split-cue", required_argument, NULL, 6},
    {"combine-cue", required_argument, NULL, 7},
    {NULL, 0, NULL, 0}
};


#ifndef ECM3_GUI
int main(int argc, char **argv) {
    // DEBUG startup marker

    std::ios_base::sync_with_stdio(false);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifdef SIGBREAK
    std::signal(SIGBREAK, signal_handler);
#endif
    // ECM processor options
    ecm_options options;

    // Input file will be decoded
    bool decode = false;

    // Track whether we already printed a specific error (skip generic fallback)
    bool specific_error_printed = false;

    // Metadata for decode path
    track_metadata_header meta_header;
    memset(&meta_header, 0, sizeof(meta_header));
    std::vector<track_metadata_entry> meta_entries;
    bool has_metadata = false;

    // Temp file for split-BIN concatenation
    temp_file temp_concat; 

    setbuf(stderr, NULL);
    // Start the timer to measure execution time
    auto start = std::chrono::high_resolution_clock::now();

    // Return code.
    int return_code = 0;

    std::string output_name_base;

    return_code = get_options(argc, argv, &options);

    if (return_code == 2) {
        return 0;
    }
    if (return_code == 3) {
        return 1;
    }
    if (return_code) {
        return 1;
    }

    // CUE split/combine are standalone operations
    // Require -o to protect original CUE from accidental overwrite
    if (options.cue_split) {
        if (options.out_filename.empty()) {
            fprintf(stderr, "ERROR: --split-cue requires -o <output directory> (to protect original CUE from overwrite). Use -f to overwrite existing files.\n\n");
            print_help();
            return 1;
        }
        return cue_cmd_split(options.cue_split_combine_file, options.out_filename, options.force_rewrite);
    }
    if (options.cue_combine) {
        if (options.out_filename.empty()) {
            fprintf(stderr, "ERROR: --combine-cue requires -o <output directory> (to protect original CUE from overwrite). Use -f to overwrite existing files.\n\n");
            print_help();
            return 1;
        }
        return cue_cmd_combine(options.cue_split_combine_file, options.out_filename, options.force_rewrite);
    }

    // No arguments: show help and exit cleanly
    if (argc == 1) {
        print_help();
        return 0;
    }

    // Collect files for batch processing
    std::vector<std::string> batch_files;
    if (options.batch_cue_mode) {
        try {
            for (auto& p : std::filesystem::recursive_directory_iterator(options.batch_directory)) {
                if (p.is_regular_file()) {
                    auto ext = p.path().extension().string();
                    if (ext == ".cue" || ext == ".CUE")
                        batch_files.push_back(p.path().string());
                }
            }
        } catch (const std::filesystem::filesystem_error&) {
            fprintf(stderr, "ERROR: cannot access directory: %s\n", options.batch_directory.c_str());
            return 1;
        }
        if (batch_files.empty()) {
            fprintf(stderr, "ERROR: no .cue files found in %s\n", options.batch_directory.c_str());
            return 1;
        }
        fprintf(stdout, "Found %zu .cue file(s) in %s\n", batch_files.size(), options.batch_directory.c_str());
    } else if (options.batch_decode_mode) {
        try {
            for (auto& p : std::filesystem::recursive_directory_iterator(options.batch_directory)) {
                if (p.is_regular_file()) {
                    auto ext = p.path().extension().string();
                    if (ext == ".ecm3" || ext == ".ECM3")
                        batch_files.push_back(p.path().string());
                }
            }
        } catch (const std::filesystem::filesystem_error&) {
            fprintf(stderr, "ERROR: cannot access directory: %s\n", options.batch_directory.c_str());
            return 1;
        }
        if (batch_files.empty()) {
            fprintf(stderr, "ERROR: no .ecm3 files found in %s\n", options.batch_directory.c_str());
            return 1;
        }
        fprintf(stdout, "Found %zu .ecm3 file(s) in %s\n", batch_files.size(), options.batch_directory.c_str());
    } else {
        batch_files.push_back(options.in_filename);
    }

    int overall_return_code = 0;

    for (size_t file_idx = 0; file_idx < batch_files.size(); file_idx++) {
        options.in_filename = batch_files[file_idx];
        if (batch_files.size() > 1) {
            options.cue_filename = "";
            options.out_filename = "";
        }
        decode = false;
        specific_error_printed = false;
        return_code = 0;

        meta_header = track_metadata_header();
        memset(&meta_header, 0, sizeof(meta_header));
        meta_entries.clear();
        has_metadata = false;
        options.delete_paths.clear();

        start = std::chrono::high_resolution_clock::now();

        if (batch_files.size() > 1) {
            fprintf(stdout, "\n[%zu/%zu] %s\n", file_idx + 1, batch_files.size(),
                    std::filesystem::path(batch_files[file_idx]).filename().string().c_str());
        }

    // When --cue is provided, it is the sole input reference — ignore any -i argument
    if (!options.cue_filename.empty() && !options.in_filename.empty()) {
        fprintf(stderr, "WARNING: --cue was specified; ignoring --input/-i (the CUE sheet itself references the BIN files).\n");
        options.in_filename.clear();
    }

    // Auto-detect if --input has .cue extension
    if (options.cue_filename.empty() && !options.in_filename.empty()) {
        std::string ext = std::filesystem::path(options.in_filename).extension().string();
        if (ext == ".cue" || ext == ".CUE") {
            cue_sheet auto_cue;
            if (cue_parse(options.in_filename, auto_cue) == 0 && !auto_cue.file_order.empty()) {
                std::string cue_dir = get_cue_dir(options.in_filename);
                std::filesystem::path bin_path = std::filesystem::path(cue_dir) / auto_cue.file_order[0];
                fprintf(stdout, "CUE file detected, using BIN: %s\n", bin_path.filename().string().c_str());
                options.cue_filename = options.in_filename;
                options.in_filename = bin_path.string();
            }
        }
    }

    // Detect if the file is a ECM3 file
    if (!options.in_filename.empty()) {
        std::ifstream tmp_f(options.in_filename.c_str(), std::ios::binary);
        if (tmp_f.good()) {
            std::string sig(3, '\0');
            tmp_f.read(&sig[0], 3);
            if (sig == "ECM") {
                decode = true;
            }
        }
    }

// Encoding process
    if (!decode) {
        // Auto-detect CUE when input is a .bin (must run before the CUE handling below)
        if (options.cue_filename.empty()) {
            namespace fs = std::filesystem;
            fs::path in_path(options.in_filename);
            std::string ext = in_path.extension().string();
            if (ext != ".cue" && ext != ".CUE" && ext != ".ecm3") {
                // Strategy 1: same basename, different extension
                fs::path cue_path = in_path;
                cue_path.replace_extension(".cue");
                if (fs::exists(cue_path)) {
                    fprintf(stdout, "Auto-detected CUE: %s\n", cue_path.filename().string().c_str());
                    options.cue_filename = cue_path.string();
                }
                // Strategy 2: search directory for .cue files that reference this BIN
                if (options.cue_filename.empty()) {
                    fs::path in_dir = in_path.parent_path();
                    if (in_dir.empty()) in_dir = ".";
                    std::string in_filename = in_path.filename().string();
                    std::string in_filename_lower = in_filename;
                    std::transform(in_filename_lower.begin(), in_filename_lower.end(), in_filename_lower.begin(), ::tolower);
                    for (auto& entry : fs::directory_iterator(in_dir)) {
                        if (entry.path().extension() != ".cue") continue;
                        cue_sheet auto_cue;
                        if (cue_parse(entry.path().string(), auto_cue) != 0 || auto_cue.file_order.empty())
                            continue;
                        for (auto& fname : auto_cue.file_order) {
                            if (fname == in_filename_lower) {
                                fprintf(stdout, "Auto-detected CUE: %s\n", entry.path().filename().string().c_str());
                                options.cue_filename = entry.path().string();
                                break;
                            }
                        }
                        if (!options.cue_filename.empty()) break;
                    }
                }
            }
        }

        // --cue handling: parse cue sheet and optionally concatenate split BINs
        cue_sheet parsed_cue;
        bool has_cue = false;
        if (!options.cue_filename.empty()) {
            if (cue_parse(options.cue_filename, parsed_cue) != 0) {
                fprintf(stderr, "ERROR: Failed to parse CUE file: %s\n", options.cue_filename.c_str());
                return_code = 1;
                goto exit;
            }
            has_cue = true;

            std::string cue_dir = get_cue_dir(options.cue_filename);

            if (parsed_cue.file_order.size() > 1) {
                if (concat_split_bins(parsed_cue, cue_dir, temp_concat) != 0) {
                    return_code = 1;
                    goto exit;
                }
                options.in_filename = temp_concat.path();
            } else {
                options.in_filename = (std::filesystem::path(cue_dir) / parsed_cue.file_order[0]).string();
            }
        }

        // Derive output filename from input if not provided (use stem() to strip any extension)
        if (options.out_filename.empty()) {
            std::filesystem::path base = has_cue
                ? std::filesystem::path(options.cue_filename)
                : std::filesystem::path(options.in_filename);
            options.out_filename = (base.parent_path() / (base.stem().string() + ".ecm3")).string();
        }

        // Check if output file already exists
        if (!options.force_rewrite && std::filesystem::exists(options.out_filename)) {
            fprintf(stderr, "ERROR: output file already exists. Use -f to overwrite.\n");
            specific_error_printed = true;
            return_code = 1;
            goto exit;
        }

        // Call core encode function
        ecm3_result encode_result;
        return_code = ecm3_encode(options.in_filename, options.out_filename, options,
                                   has_cue ? &parsed_cue : nullptr, has_cue, encode_result);
        if (return_code == 0) {
            meta_header = encode_result.meta_header;
            meta_entries = encode_result.meta_entries;
            has_metadata = encode_result.has_metadata;
            options.delete_paths = encode_result.source_paths;

            summary(
                &encode_result.sectors_type_summary,
                &options,
                0,
                &encode_result.encode_streams_script
            );
        }
        if (return_code != 0) {
            goto exit;
        }
    }
    // Decoding process
    else {
        // Derive output filename from input: strip .ecm3, use .bin
        if (options.out_filename.empty()) {
            std::string in = options.in_filename;
            if (in.size() >= 5 && in.substr(in.size() - 5) == ".ecm3") {
                in.resize(in.size() - 5);
            }
            options.out_filename = in + ".bin";
        }

        mycounter_is_verify = options.verify;

        // Check if output file already exists
        if (!options.verify && !options.force_rewrite && std::filesystem::exists(options.out_filename)) {
            fprintf(stderr, "ERROR: output file already exists. Use -f to overwrite.\n");
            specific_error_printed = true;
            return_code = 1;
            goto exit;
        }

        // Call core decode function
        ecm3_result decode_result;
        return_code = ecm3_decode(options.in_filename, options.out_filename, options, decode_result);
        if (return_code == 0) {
            meta_header = decode_result.meta_header;
            meta_entries = decode_result.meta_entries;
            has_metadata = decode_result.has_metadata;
            options.delete_paths = decode_result.source_paths;

            // Write .cue file if metadata was found
            if (has_metadata && !options.verify) {
                write_cue_from_metadata(meta_header, meta_entries, options.out_filename, options.split_output);
            }
        }
        if (return_code != 0) {
            goto exit;
        }
    }

    exit:
    // temp_concat RAII destructor will clean up the temp file automatically

    if (g_interrupted.load(std::memory_order_relaxed)) {
        fprintf(stderr, "\nInterrupted. Cleaning up...\n");
        if (!options.out_filename.empty() && !options.verify) {
            if (std::filesystem::exists(options.out_filename)) {
                std::filesystem::remove(options.out_filename);
            }
        }
        return_code = 1;
    }
    else if (return_code != 0) {
        if (!specific_error_printed && !options.keep_output && !options.verify) {
            fprintf(stderr, "\n\nERROR: there was an error processing the input file.\n\n");
            if (std::filesystem::exists(options.out_filename)) {
                std::filesystem::remove(options.out_filename);
            }
        }
        if (decode && options.verify) {
            fprintf(stdout, "\n\nVerification: FAILED (error %d)\n", return_code);
        }
    }
    else {
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
        if (decode && options.verify) {
            fprintf(stdout, "\n\nVerification: PASSED (EDC check OK)\n");
        } else {
            fprintf(stdout, "\n\nThe file was processed without any problem\n");
        }
        fprintf(stdout, "Total execution time: %0.3fs\n\n", duration.count() / 1000.0F);

        if (decode && !options.verify && options.split_output && has_metadata && !meta_entries.empty()) {
            if (meta_entries.size() > 1) {
                if (split_output_bin(options.out_filename, meta_header, meta_entries, meta_header.total_sectors, options.force_rewrite) == 0) {
                    std::error_code ec;
                    if (!std::filesystem::remove(options.out_filename, ec) && ec) {
                        fprintf(stderr, "WARNING: Could not remove temporary combined BIN: %s\n", ec.message().c_str());
                    }
                }
            }
        }

        // Delete source files if --delete-source was specified
        if (options.delete_source && !options.delete_paths.empty()) {
            for (const auto& path : options.delete_paths) {
                std::error_code ec;
                if (std::filesystem::remove(path, ec)) {
                    fprintf(stdout, "Deleted source: %s\n", path.c_str());
                } else if (ec) {
                    fprintf(stderr, "WARNING: Could not delete %s: %s\n", path.c_str(), ec.message().c_str());
                }
            }
        }
    }
    if (return_code != 0) {
        overall_return_code = return_code;
    }
    if (g_interrupted.load(std::memory_order_relaxed)) {
        break;
    }
}
return overall_return_code;
}
#endif // ECM3_GUI


int image_to_ecm_block(
    std::ifstream &in_file,
    std::fstream &out_file,
    ecm_options *options,
    std::vector<uint32_t> *sectors_type_summary,
    std::vector<stream_script> *streams_script_out
) {
    // Input size
    in_file.seekg(0, std::ios_base::end);
    size_t in_total_size = in_file.tellg();
    in_file.seekg(0, std::ios_base::beg);

    // Stream script to the encode process
    std::vector<stream_script> streams_script;

    // Sectors TOC
    sector *sectors_toc = NULL;
    sec_str_size sectors_toc_header = {C_NONE, 0, 0, 0};
    uint8_t *sectors_toc_c_buffer = NULL;

    // Streams TOC
    stream *streams_toc = NULL;
    sec_str_size streams_toc_header = {C_NONE, 0, 0, 0};
    uint8_t *streams_toc_c_buffer = NULL;

    // Sector Tools object
    sector_tools *sTools;

    // Return code
    int return_code = 0;

    if (in_total_size % 2352) {
        fprintf(stderr, "ERROR: The input file doesn't appear to be a CD-ROM image\n");
        fprintf(stderr, "       This program only allows to process CD-ROM images\n");

        return ECMTOOL_FILE_READ_ERROR;
    }


    // Reset the counters
    resetcounter(in_total_size, options->audio_compression, options->data_compression);
    setcounter_decode_mode(false);


    // Sector Tools object
    sTools = new sector_tools();
    // Block header with type 2 (ECM data), no compression, and no size for now
    // Later will be completed when data is known
    block_header ecm_block_header = {2, 0, 0, 0};
    // ECM Header
    ecm_header ecm_data_header = {
        options->optimizations,
        options->seekable ? options->sectors_per_block : (uint8_t)0,
        0,
        0,
        0,
        0,
        0,
        0,
        "",
        ""
    };
    // Struct size without the strings
    uint32_t ecm_data_header_size = ECM_HEADER_FIXED_SIZE;

    // First blocks byte
    uint64_t block_start_position = out_file.tellp();
    // Will be setted later
    uint64_t ecm_block_start_position = 0;

    // Write the "dummy" block header
    return_code = write_block_header(out_file, &ecm_block_header);
    if (return_code) {
        goto exit;
    }

    // First ECM block byte
    ecm_block_start_position = out_file.tellp();

    // Analyze the disk to detect the sectors types
    return_code = disk_analyzer (
        sTools,
        in_file,
        in_total_size,
        streams_script,
        &ecm_data_header,
        options
    );

    // Compute per-type encode totals from the analyzed streams
    {
        uint64_t audio_total = 0, data_total = 0;
        for (const auto& s : streams_script) {
            uint64_t stream_bytes = 0;
            for (const auto& sd : s.sectors_data) {
                stream_bytes += (uint64_t)sd.sector_count * 2352;
            }
            if (s.stream_data.type == 0) {
                audio_total += stream_bytes;
            } else {
                data_total += stream_bytes;
            }
        }
        mycounter_audio_comp_total = audio_total;
        mycounter_data_comp_total = data_total;
    }

    // Write the ECM dummy header
    {
        uint8_t ecm_hdr_buf[ECM_HEADER_FIXED_SIZE];
        serialize_ecm_header(ecm_hdr_buf, ecm_data_header);
        out_file.write(reinterpret_cast<char*>(ecm_hdr_buf), sizeof(ecm_hdr_buf));
    }
    if (!out_file.good()) {
        return_code = 1;
        goto exit;
    }
    out_file << ecm_data_header.title;
    out_file << ecm_data_header.id;

    //
    // Now we will write the ECM data header
    //
    ecm_data_header.ecm_data_pos = (uint64_t)out_file.tellp() - ecm_block_start_position;

    //
    // Convert the image to ECM data
    //
    return_code = disk_encode (
        sTools,
        in_file,
        out_file,
        streams_script,
        options,
        sectors_type_summary,
        ecm_data_header.ecm_data_pos
    );
    if (return_code) {
        goto exit;
    }


    //
    // Streams and Sectors TOC will be wrritten at the end because streams is modified
    // during the encoding process with required data if compression was used.
    //
    //
    // Time to write the streams header
    //
    ecm_data_header.streams_toc_pos = (uint64_t)out_file.tellp() - ecm_block_start_position;
    return_code = task_to_streams_header (
        streams_toc,
        streams_toc_header,
        streams_script
    );
    if (return_code) {
        goto exit;
    }
    // Compress the streams header. Sectors count is base 0, so one will be added to the size calculation
    {
        std::vector<uint8_t> streams_toc_buf(streams_toc_header.uncompressed_size);
        for (uint32_t i = 0; i < streams_toc_header.count; i++) {
            serialize_stream(streams_toc_buf.data() + i * ECM_STREAM_SIZE, streams_toc[i]);
        }
        // Compressed size will be the uncompressed size + 6 zlib header bytes + 5 zlib block headers for every 16k (plus two extra for security)
        uint32_t compressed_size = streams_toc_header.uncompressed_size + 6 + (((streams_toc_header.uncompressed_size / 16.384) + 3) * 5);
        streams_toc_c_buffer = (uint8_t *)malloc(compressed_size);
        if(!streams_toc_c_buffer) {
            fprintf(stderr, "Out of memory\n");
            return_code = ECMTOOL_BUFFER_MEMORY_ERROR;
            goto exit;
        }
        if (compress_header(streams_toc_c_buffer, compressed_size, streams_toc_buf.data(), streams_toc_header.uncompressed_size, 9)) {
            fprintf(stderr, "There was an error compressing the streams header.\n");
            return_code = ECMTOOL_HEADER_COMPRESSION_ERROR;
            goto exit;
        }
        streams_toc_header.compressed_size = compressed_size;
        streams_toc_header.compression = C_ZLIB;
    }
    // Write the compressed header
    {
        uint8_t ssh_buf[ECM_SEC_STR_SIZE];
        serialize_sec_str_size(ssh_buf, streams_toc_header);
        out_file.write(reinterpret_cast<char*>(ssh_buf), sizeof(ssh_buf));
    }
    out_file.write(reinterpret_cast<char*>(streams_toc_c_buffer), streams_toc_header.compressed_size);
    // Free the header memory
    free(streams_toc);
    streams_toc = NULL;
    free(streams_toc_c_buffer);
    streams_toc_c_buffer = NULL;

    //
    // Time to write the sectors header
    //
    ecm_data_header.sectors_toc_pos = (uint64_t)out_file.tellp() - ecm_block_start_position;
    return_code = task_to_sectors_header (
        sectors_toc,
        sectors_toc_header,
        streams_script
    );
    if (return_code) {
        goto exit;
    }
    // Compress the sectors header.  Sectors count is base 0, so one will be added to the size calculation
    {
        std::vector<uint8_t> sectors_toc_buf(sectors_toc_header.uncompressed_size);
        for (uint32_t i = 0; i < sectors_toc_header.count; i++) {
            serialize_sector(sectors_toc_buf.data() + i * ECM_SECTOR_SIZE, sectors_toc[i]);
        }
        // Compressed size will be the uncompressed size + 6 zlib header bytes + 5 zlib block headers for every 16k (plus two extra for security)
        uint32_t compressed_size = sectors_toc_header.uncompressed_size + 6 + (((sectors_toc_header.uncompressed_size / 16.384) + 3) * 5);
        sectors_toc_c_buffer = (uint8_t*) malloc(compressed_size);
        if(!sectors_toc_c_buffer) {
            fprintf(stderr, "Out of memory\n");
            return_code = ECMTOOL_BUFFER_MEMORY_ERROR;
            goto exit;
        }
        if (compress_header(sectors_toc_c_buffer, compressed_size, sectors_toc_buf.data(), sectors_toc_header.uncompressed_size, 9)) {
            fprintf(stderr, "There was an error compressing the output sectors header.\n");
            return_code = ECMTOOL_HEADER_COMPRESSION_ERROR;
            goto exit;
        }
        sectors_toc_header.compressed_size = compressed_size;
        sectors_toc_header.compression = C_ZLIB;
    }
    // Write the compressed header
    {
        uint8_t ssh_buf[ECM_SEC_STR_SIZE];
        serialize_sec_str_size(ssh_buf, sectors_toc_header);
        out_file.write(reinterpret_cast<char*>(ssh_buf), sizeof(ssh_buf));
    }
    if (!out_file.good()) {
        return_code = 1;
        goto exit;
    }
    out_file.write(reinterpret_cast<char*>(sectors_toc_c_buffer), sectors_toc_header.compressed_size);
    if (!out_file.good()) {
        return_code = 1;
        goto exit;
    }
    // Free the header memory
    free(sectors_toc);
    sectors_toc = NULL;
    free(sectors_toc_c_buffer);
    sectors_toc_c_buffer = NULL;


    // Set the block sizes. Both are equal because this block will not use compression
    ecm_block_header.real_block_size = (uint64_t)out_file.tellp() - ecm_block_start_position;
    ecm_block_header.block_size = (uint64_t)out_file.tellp() - ecm_block_start_position;

    // Write the new block heeader
    out_file.seekp(block_start_position);
    {
        uint8_t bh_buf[ECM_BLOCK_HEADER_SIZE];
        serialize_block_header(bh_buf, ecm_block_header);
        out_file.write(reinterpret_cast<char*>(bh_buf), sizeof(bh_buf));
    }
    if (!out_file.good()) {
        return_code = 1;
        goto exit;
    }

    // Finally, write the ecm data block header
    {
        uint8_t ecm_hdr_buf[ECM_HEADER_FIXED_SIZE];
        serialize_ecm_header(ecm_hdr_buf, ecm_data_header);
        out_file.write(reinterpret_cast<char*>(ecm_hdr_buf), sizeof(ecm_hdr_buf));
    }
    if (!out_file.good()) {
        return_code = 1;
        goto exit;
    }
    out_file.seekp(0, std::ios_base::end);

    exit:
    // Copy streams_script to output parameter before cleanup
    if (streams_script_out && return_code == 0) {
        *streams_script_out = std::move(streams_script);
    }

    // Free the reserved memory for objects
    if (sTools) {
        delete sTools;
    }
    if (streams_toc) {
        free(streams_toc);
    }
    if (streams_toc_c_buffer) {
        free(streams_toc_c_buffer);
    }
    if (sectors_toc) {
        free(sectors_toc);
    }
    if (sectors_toc_c_buffer) {
        free(sectors_toc_c_buffer);
    }

    return return_code;
}

int ecm_block_to_image(
    std::ifstream &in_file,
    std::fstream &out_file,
    ecm_options *options
) {
    // CRC calculation to check the decoded stream
    uint32_t output_edc = 0;

    // Stream script to the encode process
    std::vector<stream_script> streams_script;

    // ECM headers
    block_header ecm_block_header;
    ecm_header ecm_data_header;
    // Struct size without the strings
    uint32_t ecm_data_header_size = ECM_HEADER_FIXED_SIZE;

    // Will be setted later
    uint64_t ecm_block_start_position;

    // Sectors TOC
    sector *sectors_toc = NULL;
    sec_str_size sectors_toc_header = {C_NONE, 0, 0, 0};
    uint8_t *sectors_toc_c_buffer;

    // Streams TOC
    stream *streams_toc = NULL;
    sec_str_size streams_toc_header = {C_NONE, 0, 0, 0};
    uint8_t *streams_toc_c_buffer;

    // Sector Tools object
    sector_tools *sTools = new sector_tools();

    // Return code
    int return_code = 0;

    // Read the block header
    return_code = read_block_header(in_file, &ecm_block_header);
    if (return_code) {
        goto exit;
    }

    // First ECM block byte
    ecm_block_start_position = in_file.tellg();

    // Read the ECM data header
    {
        uint8_t ecm_hdr_buf[ECM_HEADER_FIXED_SIZE];
        in_file.read(reinterpret_cast<char*>(ecm_hdr_buf), sizeof(ecm_hdr_buf));
        if (!in_file.good()) {
            return_code = 1;
            goto exit;
        }
        deserialize_ecm_header(ecm_hdr_buf, ecm_data_header);
    }

    // Read the title stored in file if exists
    if (ecm_data_header.title_length) {
        if (ecm_data_header.title_length > 256) {
            fprintf(stderr, "ERROR: Invalid title length %u in ECM header.\n", ecm_data_header.title_length);
            return_code = ECMTOOL_CORRUPTED_STREAM;
            goto exit;
        }
        ecm_data_header.title.resize(ecm_data_header.title_length);
        in_file.read((char *)ecm_data_header.title.data(), ecm_data_header.title_length);
        if (!in_file.good()) {
            return_code = 1;
            goto exit;
        }
    }

    // Read the ID stored in file if exists
    if (ecm_data_header.id_length) {
        if (ecm_data_header.id_length > 256) {
            fprintf(stderr, "ERROR: Invalid ID length %u in ECM header.\n", ecm_data_header.id_length);
            return_code = ECMTOOL_CORRUPTED_STREAM;
            goto exit;
        }
        ecm_data_header.id.resize(ecm_data_header.id_length);
        in_file.read((char *)ecm_data_header.id.data(), ecm_data_header.id_length);
        if (!in_file.good()) {
            return_code = 1;
            goto exit;
        }
    }

    // Set the optimization options used in file
    options->optimizations = (optimization_options)ecm_data_header.optimizations;

    // Reset the counters
    resetcounter(ecm_block_header.block_size);
    setcounter_decode_mode(true);

    //
    // Read the streams toc header
    in_file.seekg(ecm_data_header.streams_toc_pos + ecm_block_start_position, std::ios_base::beg);
    {
        uint8_t ssh_buf[ECM_SEC_STR_SIZE];
        in_file.read(reinterpret_cast<char*>(ssh_buf), sizeof(ssh_buf));
        if (!in_file.good()) {
            return_code = ECMTOOL_CORRUPTED_STREAM;
            goto exit;
        }
        deserialize_sec_str_size(ssh_buf, streams_toc_header);
    }
    if (streams_toc_header.count > 10000 || streams_toc_header.compressed_size > 100 * 1024 * 1024 || streams_toc_header.uncompressed_size > 100 * 1024 * 1024) {
        fprintf(stderr, "ERROR: Invalid streams TOC header (count=%u, compressed=%u, uncompressed=%u).\n",
            streams_toc_header.count, streams_toc_header.compressed_size, streams_toc_header.uncompressed_size);
        return_code = ECMTOOL_CORRUPTED_STREAM;
        goto exit;
    }
    // Read the compressed stream toc data
    streams_toc_c_buffer = (uint8_t *)malloc(streams_toc_header.compressed_size);
    if (!streams_toc_c_buffer) {
        return_code = 1;
        goto exit;
    }
    in_file.read(reinterpret_cast<char*>(streams_toc_c_buffer), streams_toc_header.compressed_size);
    if (!in_file.good()) {
        printf("Error reading the in file: %s\n", strerror(errno));
        return_code = 1;
        goto exit;
    }
    // Decompress the streams toc data into a byte buffer, then deserialize
    {
        uint8_t *streams_toc_buf = (uint8_t *)malloc(streams_toc_header.uncompressed_size);
        if (!streams_toc_buf) {
            free(streams_toc_c_buffer);
            streams_toc_c_buffer = NULL;
            return_code = ECMTOOL_BUFFER_MEMORY_ERROR;
            goto exit;
        }
        if (decompress_header(streams_toc_buf, streams_toc_header.uncompressed_size, streams_toc_c_buffer, streams_toc_header.compressed_size)) {
            fprintf(stderr, "There was an error decompressing the streams header.\n");
            free(streams_toc_buf);
            free(streams_toc_c_buffer);
            streams_toc_c_buffer = NULL;
            return_code = ECMTOOL_HEADER_COMPRESSION_ERROR;
            goto exit;
        }
        streams_toc = (stream *)calloc(streams_toc_header.count, sizeof(stream));
        for (uint32_t i = 0; i < streams_toc_header.count; i++) {
            deserialize_stream(streams_toc_buf + i * ECM_STREAM_SIZE, streams_toc[i]);
        }
        free(streams_toc_buf);
    }
    free(streams_toc_c_buffer);
    streams_toc_c_buffer = NULL;

    //
    // Read the sectors toc header
    in_file.seekg(ecm_data_header.sectors_toc_pos + ecm_block_start_position, std::ios_base::beg);
    {
        uint8_t ssh_buf[ECM_SEC_STR_SIZE];
        in_file.read(reinterpret_cast<char*>(ssh_buf), sizeof(ssh_buf));
        if (!in_file.good()) {
            return_code = ECMTOOL_CORRUPTED_STREAM;
            goto exit;
        }
        deserialize_sec_str_size(ssh_buf, sectors_toc_header);
    }
    if (sectors_toc_header.count > 1000000 || sectors_toc_header.compressed_size > 100 * 1024 * 1024 || sectors_toc_header.uncompressed_size > 100 * 1024 * 1024) {
        fprintf(stderr, "ERROR: Invalid sectors TOC header (count=%u, compressed=%u, uncompressed=%u).\n",
            sectors_toc_header.count, sectors_toc_header.compressed_size, sectors_toc_header.uncompressed_size);
        return_code = ECMTOOL_CORRUPTED_STREAM;
        goto exit;
    }
    // Read the compressed stream toc data
    sectors_toc_c_buffer = (uint8_t *)malloc(sectors_toc_header.compressed_size);
    if (!sectors_toc_c_buffer) {
        return_code = 1;
        goto exit;
    }
    in_file.read(reinterpret_cast<char*>(sectors_toc_c_buffer), sectors_toc_header.compressed_size);
    if (!in_file.good()) {
        return_code = 1;
        goto exit;
    }
    // Decompress the sectors toc data into a byte buffer, then deserialize
    {
        uint8_t *sectors_toc_buf = (uint8_t *)malloc(sectors_toc_header.uncompressed_size);
        if (!sectors_toc_buf) {
            free(sectors_toc_c_buffer);
            sectors_toc_c_buffer = NULL;
            return_code = ECMTOOL_BUFFER_MEMORY_ERROR;
            goto exit;
        }
        if (decompress_header(sectors_toc_buf, sectors_toc_header.uncompressed_size, sectors_toc_c_buffer, sectors_toc_header.compressed_size)) {
            fprintf(stderr, "There was an error decompressing the sectors header.\n");
            free(sectors_toc_buf);
            free(sectors_toc_c_buffer);
            sectors_toc_c_buffer = NULL;
            return_code = ECMTOOL_HEADER_COMPRESSION_ERROR;
            goto exit;
        }
        sectors_toc = (sector *)calloc(sectors_toc_header.count, sizeof(sector));
        for (uint32_t i = 0; i < sectors_toc_header.count; i++) {
            deserialize_sector(sectors_toc_buf + i * ECM_SECTOR_SIZE, sectors_toc[i]);
        }
        free(sectors_toc_buf);
    }
    free(sectors_toc_c_buffer);
    sectors_toc_c_buffer = NULL;

    // Convert the headers to a script to be followed
    return_code = task_maker (
        streams_toc,
        streams_toc_header,
        sectors_toc,
        sectors_toc_header,
        streams_script
    );
    if (return_code) {
        return_code = ECMTOOL_CORRUPTED_STREAM;
        goto exit;
    }

    in_file.seekg(ecm_data_header.ecm_data_pos + ecm_block_start_position, std::ios_base::beg);
    return_code = disk_decode (
        sTools,
        in_file,
        out_file,
        streams_script,
        options,
        ecm_data_header.ecm_data_pos
    );

    exit:
    if (sTools) {
        delete sTools;
    }
    if (streams_toc) {
        free(streams_toc);
    }
    if (streams_toc_c_buffer) {
        free(streams_toc_c_buffer);
    }
    if (sectors_toc) {
        free(sectors_toc);
    }
    if (sectors_toc_c_buffer) {
        free(sectors_toc_c_buffer);
    }

    return return_code;
}


int write_block_header(
    std::fstream &out_file,
    block_header *block_header_data
) {
    if (out_file.good()) {
        uint8_t bh_buf[ECM_BLOCK_HEADER_SIZE];
        serialize_block_header(bh_buf, *block_header_data);
        out_file.write(reinterpret_cast<char*>(bh_buf), sizeof(bh_buf));

        if (out_file.good()) {
            return 0;
        }
        else {
            return 1;
        }
    }
    else {
        fprintf(stderr, "Error writing the header because the output file is not correct\n.");
        return 1;
    }

    return 0;
}


int read_block_header(
    std::ifstream &in_file,
    block_header *block_header_data
) {
    if (in_file.good()) {
        uint8_t bh_buf[ECM_BLOCK_HEADER_SIZE];
        in_file.read(reinterpret_cast<char*>(bh_buf), sizeof(bh_buf));
        if (in_file.good()) {
            deserialize_block_header(bh_buf, *block_header_data);
            return 0;
        }
        else {
            return 1;
        }
    }
    else {
        fprintf(stderr, "Error reading the header because the input file is not correct\n.");
        return 1;
    }

    return 0;
}


struct sector_analysis_result {
    sector_tools_types type;
};

static void analyze_sector_chunk(
    const uint8_t* mmap_data,
    uint64_t offset,
    size_t sector_count,
    sector_analysis_result* results
) {
    for (size_t i = 0; i < sector_count; i++) {
        sector_tools sTools;
        results[i].type = sTools.detect(const_cast<uint8_t*>(mmap_data + offset + i * 2352));
    }
}

static ecm3_return_code disk_analyzer (
    sector_tools *sTools,
    std::ifstream &in_file,
    size_t image_file_size,
    std::vector<stream_script> &streams_script,
    ecm_header *ecm_data_header,
    ecm_options *options
) {
    size_t sectors_count = image_file_size / 2352;
    uint32_t current_sector = 0;

    std::string filename;
    {
        std::filesystem::path p(options->in_filename);
        filename = p.string();
    }

    mmap_file mfile;
    bool use_mmap = mfile.open(filename);
    const uint8_t *mmap_data = use_mmap ? mfile.data() : nullptr;

    uint8_t in_sector[2352];
    std::string id;
    int id_detection_return = -1;

    if (use_mmap && options->jobs != 1) {
        uint32_t num_jobs = options->jobs;
        if (num_jobs == 0) {
            num_jobs = std::thread::hardware_concurrency();
            if (num_jobs == 0) num_jobs = 1;
        }
        if (num_jobs > sectors_count) num_jobs = (uint32_t)sectors_count;

        std::vector<sector_analysis_result> all_results(sectors_count);
        std::vector<std::thread> threads;
        size_t chunk_size = (sectors_count + num_jobs - 1) / num_jobs;

        for (uint32_t t = 0; t < num_jobs; t++) {
            size_t start = t * chunk_size;
            size_t count = std::min(chunk_size, sectors_count - start);
            if (count == 0) break;

            threads.emplace_back(analyze_sector_chunk,
                mmap_data, start * 2352, count,
                all_results.data() + start);
        }

        for (auto& th : threads) {
            th.join();
        }

        setcounter_analyze(image_file_size);

        for (size_t i = 0; i < sectors_count; i++) {
            sector_tools_types detected_type = all_results[i].type;
            memcpy(in_sector, mmap_data + i * 2352, 2352);

            if (id_detection_return != 0) {
                id_detection_return = detect_id_psx(id, in_sector, 2352);
                if (id_detection_return == 0) {
                    ecm_data_header->id = id;
                    ecm_data_header->id_length = id.length();
                }
            }

            if (detected_type != STT_CDDA && detected_type != STT_CDDA_GAP) {
                uint8_t time_data[3];
                sTools->sector_to_time(time_data, current_sector + 0x96);
                if (
                    ecm_data_header->optimizations & OO_REMOVE_MSF &&
                    (time_data[0] != in_sector[0x0C] ||
                    time_data[1] != in_sector[0x0D] ||
                    time_data[2] != in_sector[0x0E])
                ) {
                    ecm_data_header->optimizations = (optimization_options)(ecm_data_header->optimizations & ~OO_REMOVE_MSF);
                }

                if (
                    (
                        detected_type == STT_MODE2_1 ||
                        detected_type == STT_MODE2_1_GAP ||
                        detected_type == STT_MODE2_2 ||
                        detected_type == STT_MODE2_2_GAP
                    ) &&
                    ecm_data_header->optimizations & OO_REMOVE_REDUNDANT_FLAG &&
                    (
                        in_sector[0x10] != in_sector[0x14] ||
                        in_sector[0x11] != in_sector[0x15] ||
                        in_sector[0x12] != in_sector[0x16] ||
                        in_sector[0x13] != in_sector[0x17]
                    )
                ) {
                    ecm_data_header->optimizations = (optimization_options)(ecm_data_header->optimizations & ~OO_REMOVE_REDUNDANT_FLAG);
                }
            }

            options->optimizations = (optimization_options)ecm_data_header->optimizations;

            sector_tools_stream_types stream_type = sTools->detect_stream(detected_type);
            if (
                streams_script.size() == 0 ||
                streams_script.back().stream_data.type != (stream_type - 1)
            ) {
                streams_script.push_back(stream_script());
                streams_script.back().stream_data.type = stream_type - 1;
                if (stream_type == STST_AUDIO) {
                    streams_script.back().stream_data.compression = options->audio_compression;
                }
                else {
                    streams_script.back().stream_data.compression = options->data_compression;
                }

                if (streams_script.size() > 1) {
                    streams_script.back().stream_data.end_sector = streams_script[streams_script.size() - 2].stream_data.end_sector;
                }
                else {
                    streams_script.back().stream_data.end_sector = 0;
                }
            }

            if (
                streams_script.back().sectors_data.size() == 0 ||
                streams_script.back().sectors_data.back().mode != detected_type
            ) {
                streams_script.back().sectors_data.push_back(sector());
                streams_script.back().sectors_data.back().mode = detected_type;
                streams_script.back().sectors_data.back().sector_count = 0;
            }

            streams_script.back().stream_data.end_sector++;
            streams_script.back().sectors_data.back().sector_count++;
            current_sector++;
        }
    }
    else {
        in_file.seekg(0, std::ios_base::beg);

        for (size_t i = 0; i < sectors_count; i++) {
            if (use_mmap) {
                memcpy(in_sector, mmap_data + i * 2352, 2352);
            } else {
                in_file.read(reinterpret_cast<char*>(in_sector), 2352);
                if (!in_file.good()) {
                    fprintf(stderr, "There was an error reading the input file.\n");
                    return ECMTOOL_FILE_READ_ERROR;
                }
            }

            setcounter_analyze(use_mmap ? ((i + 1) * 2352) : (in_file.good() ? (std::streamoff)in_file.tellg() : (std::streamoff)(i * 2352)));

            CHECK_INTERRUPT();

            sector_tools_types detected_type = sTools->detect(in_sector);

            if (id_detection_return != 0) {
                id_detection_return = detect_id_psx(id, in_sector, 2352);
                if (id_detection_return == 0) {
                    ecm_data_header->id = id;
                    ecm_data_header->id_length = id.length();
                }
            }

            if (detected_type != STT_CDDA && detected_type != STT_CDDA_GAP) {
                uint8_t time_data[3];
                sTools->sector_to_time(time_data, current_sector + 0x96);
                if (
                    ecm_data_header->optimizations & OO_REMOVE_MSF &&
                    (time_data[0] != in_sector[0x0C] ||
                    time_data[1] != in_sector[0x0D] ||
                    time_data[2] != in_sector[0x0E])
                ) {
                    ecm_data_header->optimizations = (optimization_options)(ecm_data_header->optimizations & ~OO_REMOVE_MSF);
                }

                if (
                    (
                        detected_type == STT_MODE2_1 ||
                        detected_type == STT_MODE2_1_GAP ||
                        detected_type == STT_MODE2_2 ||
                        detected_type == STT_MODE2_2_GAP
                    ) &&
                    ecm_data_header->optimizations & OO_REMOVE_REDUNDANT_FLAG &&
                    (
                        in_sector[0x10] != in_sector[0x14] ||
                        in_sector[0x11] != in_sector[0x15] ||
                        in_sector[0x12] != in_sector[0x16] ||
                        in_sector[0x13] != in_sector[0x17]
                    )
                ) {
                    ecm_data_header->optimizations = (optimization_options)(ecm_data_header->optimizations & ~OO_REMOVE_REDUNDANT_FLAG);
                }
            }

            options->optimizations = (optimization_options)ecm_data_header->optimizations;

            sector_tools_stream_types stream_type = sTools->detect_stream(detected_type);
            if (
                streams_script.size() == 0 ||
                streams_script.back().stream_data.type != (stream_type - 1)
            ) {
                streams_script.push_back(stream_script());
                streams_script.back().stream_data.type = stream_type - 1;
                if (stream_type == STST_AUDIO) {
                    streams_script.back().stream_data.compression = options->audio_compression;
                }
                else {
                    streams_script.back().stream_data.compression = options->data_compression;
                }

                if (streams_script.size() > 1) {
                    streams_script.back().stream_data.end_sector = streams_script[streams_script.size() - 2].stream_data.end_sector;
                }
                else {
                    streams_script.back().stream_data.end_sector = 0;
                }
            }

            if (
                streams_script.back().sectors_data.size() == 0 ||
                streams_script.back().sectors_data.back().mode != detected_type
            ) {
                streams_script.back().sectors_data.push_back(sector());
                streams_script.back().sectors_data.back().mode = detected_type;
                streams_script.back().sectors_data.back().sector_count = 0;
            }

            streams_script.back().stream_data.end_sector++;
            streams_script.back().sectors_data.back().sector_count++;
            current_sector++;
        }
    }

    return ECMTOOL_OK;
}


struct stream_encode_result {
    std::vector<uint8_t> data;
    uint32_t partial_edc;
    uint64_t byte_count;
    ecm3_return_code error;
};

static size_t get_buffer_size_for_compression(sector_tools_compression comp) {
    switch (comp) {
        case C_LZMA:
        case C_LZMA2:
            return 0x800000;
        case C_FLAC:
        case C_ZLIB:
            return 0x400000;
        case C_LZ4:
            return 0x600000;
        case C_ZSTD:
        case C_WAVPACK:
            return 0x400000;
        default:
            return BUFFER_SIZE;
    }
}

static ecm3_return_code encode_single_stream(
    const std::string &in_filename,
    uint64_t start_byte_offset,
    const stream_script &script,
    ecm_options *options,
    stream_encode_result &result,
    std::atomic<uint64_t> *progress,
    const std::vector<uint8_t>* preset_dict = nullptr
) {
    result.data.clear();
    result.partial_edc = 0;
    result.byte_count = 0;
    result.error = ECMTOOL_OK;

    mmap_file mfile;
    bool use_mmap = mfile.open(in_filename);
    const uint8_t *mmap_data = use_mmap ? mfile.data() : nullptr;
    uint64_t mmap_offset = start_byte_offset;

    std::ifstream in_file;
    if (!use_mmap) {
        in_file.open(in_filename, std::ios::binary);
        if (!in_file.good()) {
            result.error = ECMTOOL_FILE_READ_ERROR;
            return result.error;
        }
        in_file.seekg(start_byte_offset, std::ios_base::beg);
    }

    sector_tools local_sTools;
    uint8_t in_sector[2352];
    uint8_t out_sector[2352];
    compressor *compobj = NULL;

    size_t comp_buffer_size = get_buffer_size_for_compression(
        (sector_tools_compression)script.stream_data.compression);

    if (script.stream_data.compression) {
        int32_t compression_option = options->compression_level;
        if (options->extreme_compression) {
            if ((sector_tools_compression)script.stream_data.compression == C_LZMA ||
                (sector_tools_compression)script.stream_data.compression == C_LZMA2) {
                compression_option |= LZMA_PRESET_EXTREME;
            }
            else if ((sector_tools_compression)script.stream_data.compression == C_FLAC) {
                compression_option |= FLACZLIB_EXTREME_COMPRESSION;
            }
            else if ((sector_tools_compression)script.stream_data.compression == C_ZSTD) {
                compression_option = ZSTD_maxCLevel();
            }
            else if ((sector_tools_compression)script.stream_data.compression == C_WAVPACK) {
                compression_option |= WAVPACKZLIB_EXTREME_COMPRESSION;
            }
        }
        compobj = new compressor(
            (sector_tools_compression)script.stream_data.compression,
            true,
            compression_option
        );
        if (preset_dict && !preset_dict->empty()) {
            compobj->set_dictionary(preset_dict->data(), preset_dict->size());
        }
        result.data.resize(comp_buffer_size);
        compobj->set_output(result.data.data(), comp_buffer_size);
    }

    uint32_t current_sector_abs = 0;
    uint32_t total_sectors_in_stream = 0;
    for (uint32_t j = 0; j < script.sectors_data.size(); j++) {
        total_sectors_in_stream += script.sectors_data[j].sector_count;
    }

    for (uint32_t j = 0; j < script.sectors_data.size(); j++) {
        for (uint32_t k = 0; k < script.sectors_data[j].sector_count; k++) {
            CHECK_INTERRUPT();
            if (use_mmap) {
                memcpy(in_sector, mmap_data + mmap_offset, 2352);
                mmap_offset += 2352;
            } else {
                in_file.read(reinterpret_cast<char*>(in_sector), 2352);
                if (!in_file.good()) {
                    delete compobj;
                    result.error = ECMTOOL_FILE_READ_ERROR;
                    return result.error;
                }
            }

            {
                auto st = (sector_tools_types)script.sectors_data[j].mode;
                bool has_ecc = (st == STT_MODE1 || st == STT_MODE1_GAP || st == STT_MODE1_RAW ||
                                st == STT_MODE2_1 || st == STT_MODE2_1_GAP);
                uint32_t edc_size = (has_ecc && (options->optimizations & OO_REMOVE_ECC)) ? 0x81C : 2352;
                result.partial_edc = local_sTools.edc_compute(result.partial_edc, in_sector, edc_size);
            }
            result.byte_count += 2352;

            if (progress) progress->store(result.byte_count, std::memory_order_relaxed);

            uint16_t out_size = 0;
            int8_t res = sector_tools::clean_sector(
                out_sector,
                in_sector,
                (sector_tools_types)script.sectors_data[j].mode,
                out_size,
                options->optimizations
            );

            if (res) {
                delete compobj;
                result.error = ECMTOOL_PROCESSING_ERROR;
                return result.error;
            }

            if (!script.stream_data.compression) {
                size_t old_size = result.data.size();
                result.data.resize(old_size + out_size);
                memcpy(result.data.data() + old_size, out_sector, out_size);
            } else {
                current_sector_abs++;
                size_t compress_buffer_left = 0;

                if (current_sector_abs >= total_sectors_in_stream) {
                    res = compobj->compress(compress_buffer_left, out_sector, out_size, Z_FINISH);
                } else if (options->seekable && (options->sectors_per_block == 1 || !(current_sector_abs % options->sectors_per_block))) {
                    res = compobj->compress(compress_buffer_left, out_sector, out_size, Z_FULL_FLUSH);
                } else {
                    res = compobj->compress(compress_buffer_left, out_sector, out_size, Z_NO_FLUSH);
                }

                if (res != 0) {
                    delete compobj;
                    result.error = ECMTOOL_PROCESSING_ERROR;
                    return result.error;
                }

                if (compress_buffer_left < (comp_buffer_size * 0.25) || current_sector_abs >= total_sectors_in_stream) {
                    // Trim unused space from the current buffer window
                    result.data.resize(result.data.size() - compress_buffer_left);
                    if (current_sector_abs < total_sectors_in_stream) {
                        // Allocate fresh space for the next chunk
                        result.data.resize(result.data.size() + comp_buffer_size);
                        compobj->set_output(result.data.data() + result.data.size() - comp_buffer_size, comp_buffer_size);
                    }
                }
            }
        }
    }

    delete compobj;

    if (!use_mmap) in_file.close();
    result.error = ECMTOOL_OK;
    return result.error;
}

static ecm3_return_code disk_encode_sequential(
    sector_tools *sTools,
    std::ifstream &in_file,
    std::fstream &out_file,
    std::vector<stream_script> &streams_script,
    ecm_options *options,
    std::vector<uint32_t> *sectors_type,
    uint64_t ecm_block_start_position
) {
    uint8_t in_sector[2352];
    uint8_t out_sector[2352];
    uint32_t input_edc = 0;
    uint8_t buffer_edc[4];
    std::vector<uint32_t>& sectors_type_ref = *sectors_type;

    std::string in_filename = options->in_filename;
    mmap_file mfile;
    bool use_mmap = mfile.open(in_filename);
    const uint8_t *mmap_data = use_mmap ? mfile.data() : nullptr;
    uint64_t mmap_offset = 0;

    if (!use_mmap) {
        in_file.seekg(0, std::ios_base::beg);
    }

    // Temp file for cleaned sectors (Pass 1 writes, Pass 2 reads)
    namespace fs = std::filesystem;
    static std::atomic<uint32_t> tmp_counter{0};
#ifdef _WIN32
    uint32_t pid = GetCurrentProcessId();
#else
    uint32_t pid = (uint32_t)getpid();
#endif
    std::string tmp_name = "ecm3_encode_clean." + std::to_string(pid) + "." + std::to_string(tmp_counter++) + ".tmp";
    temp_file tmp_clean((fs::temp_directory_path() / tmp_name).string());
    std::fstream tmp_file(tmp_clean.path(), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!tmp_file.good()) {
        fprintf(stderr, "Failed to create temp file for encoding\n");
        return ECMTOOL_FILE_WRITE_ERROR;
    }

    // === PASS 1: Clean all sectors, write to temp file, update ECM progress ===
    for (uint32_t i = 0; i < streams_script.size(); i++) {
        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            for (uint32_t k = 0; k < streams_script[i].sectors_data[j].sector_count; k++) {
                if (use_mmap) {
                    memcpy(in_sector, mmap_data + mmap_offset, 2352);
                    mmap_offset += 2352;
                } else {
                    if (in_file.eof()) {
                        fprintf(stderr, "Unexpected EOF detected.\n");
                        return ECMTOOL_FILE_READ_ERROR;
                    }
                    in_file.read(reinterpret_cast<char*>(in_sector), 2352);
                }

                {
                    auto st = (sector_tools_types)streams_script[i].sectors_data[j].mode;
                    bool has_ecc = (st == STT_MODE1 || st == STT_MODE1_GAP || st == STT_MODE1_RAW ||
                                    st == STT_MODE2_1 || st == STT_MODE2_1_GAP);
                    uint32_t edc_size = (has_ecc && (options->optimizations & OO_REMOVE_ECC)) ? 0x81C : 2352;
                    input_edc = sTools->edc_compute(input_edc, in_sector, edc_size);
                }

                uint16_t output_size = 0;
                int8_t res = sector_tools::clean_sector(
                    out_sector,
                    in_sector,
                    (sector_tools_types)streams_script[i].sectors_data[j].mode,
                    output_size,
                    options->optimizations
                );

                if (res) {
                    fprintf(stderr, "There was an error cleaning the sector\n");
                    return ECMTOOL_PROCESSING_ERROR;
                }

                sectors_type_ref[streams_script[i].sectors_data[j].mode]++;

                tmp_file.write(reinterpret_cast<char*>(out_sector), output_size);
                if (!tmp_file.good()) {
                    fprintf(stderr, "\nThere was an error writing to temp file");
                    return ECMTOOL_FILE_WRITE_ERROR;
                }

                uint64_t cur_pos = use_mmap ? mmap_offset : (uint64_t)in_file.tellg();
                setcounter_ecm_encode(cur_pos);
                setcounter_encode(cur_pos);
                CHECK_INTERRUPT();
            }
        }
    }

    tmp_file.flush();
    tmp_file.seekg(0, std::ios_base::beg);
    tmp_file.seekp(0, std::ios_base::beg);

    // === PASS 2: Read cleaned sectors from temp file, compress/write to output ===
    uint32_t current_sector_abs = 0;

    // Per-type rolling dictionaries for cross-stream compression.
    std::vector<uint8_t> audio_dict, data_dict;

    for (uint32_t i = 0; i < streams_script.size(); i++) {
        compressor *compobj = NULL;
        uint8_t *comp_buffer = NULL;
        size_t comp_buffer_size = BUFFER_SIZE;

        if (streams_script[i].stream_data.compression) {
            int32_t compression_option = options->compression_level;
            if (options->extreme_compression) {
                if ((sector_tools_compression)streams_script[i].stream_data.compression == C_LZMA ||
                    (sector_tools_compression)streams_script[i].stream_data.compression == C_LZMA2) {
                    compression_option |= LZMA_PRESET_EXTREME;
                }
                else if ((sector_tools_compression)streams_script[i].stream_data.compression == C_FLAC) {
                    compression_option |= FLACZLIB_EXTREME_COMPRESSION;
                }
                else if ((sector_tools_compression)streams_script[i].stream_data.compression == C_ZSTD) {
                    compression_option = ZSTD_maxCLevel();
                }
                else if ((sector_tools_compression)streams_script[i].stream_data.compression == C_WAVPACK) {
                    compression_option |= WAVPACKZLIB_EXTREME_COMPRESSION;
                }
            }
            compobj = new compressor(
                (sector_tools_compression)streams_script[i].stream_data.compression,
                true,
                compression_option
            );
            // Cross-stream preset dictionary for zlib streams
            if ((sector_tools_compression)streams_script[i].stream_data.compression == C_ZLIB) {
                auto& dict = streams_script[i].stream_data.type ? data_dict : audio_dict;
                if (!dict.empty()) {
                    compobj->set_dictionary(dict.data(), dict.size());
                    streams_script[i].stream_data.cross_stream_dict = true;
                }
            }
            comp_buffer_size = get_buffer_size_for_compression(
                (sector_tools_compression)streams_script[i].stream_data.compression);
            comp_buffer = (uint8_t*)malloc(comp_buffer_size);
            if (!comp_buffer) {
                fprintf(stderr, "Out of memory\n");
                return ECMTOOL_BUFFER_MEMORY_ERROR;
            }
            size_t output_size = comp_buffer_size;
            compobj->set_output(comp_buffer, output_size);
        }

        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            for (uint32_t k = 0; k < streams_script[i].sectors_data[j].sector_count; k++) {
                current_sector_abs++;
                size_t bytes_to_read = 0;
                sector_tools::encoded_sector_size(
                    (sector_tools_types)streams_script[i].sectors_data[j].mode,
                    bytes_to_read,
                    options->optimizations
                );

                tmp_file.read(reinterpret_cast<char*>(out_sector), bytes_to_read);
                if (!tmp_file.good()) {
                    fprintf(stderr, "\nThere was an error reading from temp file");
                    return ECMTOOL_FILE_READ_ERROR;
                }

                // Capture cleaned data for cross-stream dictionary
                if (streams_script[i].stream_data.compression != C_NONE) {
                    auto& dict = streams_script[i].stream_data.type ? data_dict : audio_dict;
                    update_dict(dict, out_sector, bytes_to_read);
                }

                switch (streams_script[i].stream_data.compression) {
                case C_NONE:
                    out_file.write(reinterpret_cast<char*>(out_sector), bytes_to_read);
                    if (!out_file.good()) {
                        fprintf(stderr, "\nThere was an error writing the output file");
                        return ECMTOOL_FILE_WRITE_ERROR;
                    }
                    break;

                case C_ZLIB:
                case C_LZMA:
                case C_LZMA2:
                case C_LZ4:
                case C_FLAC:
                case C_ZSTD:
                case C_WAVPACK: {
                    size_t compress_buffer_left = 0;
                    int8_t res;
                    if (current_sector_abs >= streams_script[i].stream_data.end_sector) {
                        res = compobj->compress(compress_buffer_left, out_sector, bytes_to_read, Z_FINISH);
                    }
                    else if (options->seekable && (options->sectors_per_block == 1 || !(current_sector_abs % options->sectors_per_block))) {
                        res = compobj->compress(compress_buffer_left, out_sector, bytes_to_read, Z_FULL_FLUSH);
                    }
                    else {
                        res = compobj->compress(compress_buffer_left, out_sector, bytes_to_read, Z_NO_FLUSH);
                    }

                    if (res != 0) {
                        fprintf(stderr, "There was an error compressing the stream: %d.\n", res);
                        return ECMTOOL_PROCESSING_ERROR;
                    }

                    if (compress_buffer_left < (comp_buffer_size * 0.25) || current_sector_abs >= streams_script[i].stream_data.end_sector) {
                        out_file.write(reinterpret_cast<char*>(comp_buffer), comp_buffer_size - compress_buffer_left);
                        if (!out_file.good()) {
                            fprintf(stderr, "\nThere was an error writing the output file");
                            return ECMTOOL_FILE_WRITE_ERROR;
                        }
                        size_t new_output_size = comp_buffer_size;
                        compobj->set_output(comp_buffer, new_output_size);
                    }
                    break;
                }
                }

                if (streams_script[i].stream_data.compression != C_NONE) {
                    if (streams_script[i].stream_data.type == 0) {
                        setcounter_audio_comp_encode(current_sector_abs * 2352ULL);
                    } else {
                        setcounter_data_comp_encode(current_sector_abs * 2352ULL);
                    }
                }
                CHECK_INTERRUPT();
            }
        }

        if (compobj) { delete compobj; compobj = NULL; }
        if (comp_buffer) { free(comp_buffer); }

        streams_script[i].stream_data.out_end_position = (uint64_t)out_file.tellp() - ecm_block_start_position;
    }

    tmp_file.close();

    sTools->put32lsb(buffer_edc, input_edc);
    out_file.write(reinterpret_cast<char*>(buffer_edc), 4);

    return ECMTOOL_OK;
}


static ecm3_return_code disk_encode (
    sector_tools *sTools,
    std::ifstream &in_file,
    std::fstream &out_file,
    std::vector<stream_script> &streams_script,
    ecm_options *options,
    std::vector<uint32_t> *sectors_type,
    uint64_t ecm_block_start_position
) {
    uint32_t num_jobs = options->jobs;
    if (num_jobs == 0) {
        num_jobs = std::thread::hardware_concurrency();
        if (num_jobs == 0) num_jobs = 1;
    }

    if (num_jobs <= 1 || streams_script.size() <= 1) {
        return disk_encode_sequential(sTools, in_file, out_file, streams_script, options, sectors_type, ecm_block_start_position);
    }

    uint32_t num_streams = (uint32_t)streams_script.size();
    std::vector<stream_encode_result> results(num_streams);
    std::string in_filename = options->in_filename;

    std::vector<uint64_t> start_offsets(num_streams, 0);
    uint64_t offset = 0;
    for (uint32_t i = 0; i < num_streams; i++) {
        start_offsets[i] = offset;
        uint64_t sector_count = 0;
        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            sector_count += streams_script[i].sectors_data[j].sector_count;
        }
        offset += sector_count * 2352;
    }

    uint32_t actual_jobs = std::min(num_jobs, num_streams);
    std::vector<std::thread> threads;
    std::atomic<uint32_t> next_stream{0};
    std::vector<std::atomic<uint64_t>> stream_progress(num_streams);
    for (uint32_t i = 0; i < num_streams; i++) stream_progress[i].store(0, std::memory_order_relaxed);

    auto worker = [&]() {
        while (true) {
            uint32_t idx = next_stream.fetch_add(1);
            if (idx >= num_streams) break;
            encode_single_stream(
                in_filename,
                start_offsets[idx],
                streams_script[idx],
                options,
                results[idx],
                &stream_progress[idx]
            );
        }
    };

    std::atomic<bool> done{false};
    std::thread display_thread([&]() {
        while (!done.load(std::memory_order_relaxed) && !g_interrupted.load(std::memory_order_relaxed)) {
            uint64_t aggregate = 0;
            uint64_t aggregate_audio = 0;
            uint64_t aggregate_data = 0;
            for (uint32_t i = 0; i < num_streams; i++) {
                uint64_t p = stream_progress[i].load(std::memory_order_relaxed);
                aggregate += p;
                if (streams_script[i].stream_data.compression != C_NONE) {
                    if (streams_script[i].stream_data.type == 0) {
                        aggregate_audio += p;
                    } else {
                        aggregate_data += p;
                    }
                }
            }
            if (mycounter_total > 0) {
                uint8_t pct = (uint8_t)(100 * aggregate / mycounter_total);
                if (pct != mycounter_encode_display) {
                    mycounter_encode_display = pct;
                }
                if (pct != mycounter_ecm_encode_display) {
                    mycounter_ecm_encode_display = pct;
                }
            }
            if (mycounter_audio_comp_total > 0) {
                uint8_t p = (uint8_t)(100 * aggregate_audio / mycounter_audio_comp_total);
                if (p != mycounter_audio_comp_encode_display) {
                    mycounter_audio_comp_encode_display = p;
                }
            }
            if (mycounter_data_comp_total > 0) {
                uint8_t p = (uint8_t)(100 * aggregate_data / mycounter_data_comp_total);
                if (p != mycounter_data_comp_encode_display) {
                    mycounter_data_comp_encode_display = p;
                }
            }
            mycounter_audio_comp_progress.store(aggregate_audio, std::memory_order_relaxed);
            mycounter_data_comp_progress.store(aggregate_data, std::memory_order_relaxed);
            mycounter_ecm_progress.store(aggregate, std::memory_order_relaxed);
            mycounter_progress.store(aggregate, std::memory_order_relaxed);
            encode_progress();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        uint64_t aggregate = 0;
        uint64_t aggregate_audio = 0;
        uint64_t aggregate_data = 0;
        for (uint32_t i = 0; i < num_streams; i++) {
            uint64_t p = stream_progress[i].load(std::memory_order_relaxed);
            aggregate += p;
            if (streams_script[i].stream_data.compression != C_NONE) {
                if (streams_script[i].stream_data.type == 0) {
                    aggregate_audio += p;
                } else {
                    aggregate_data += p;
                }
            }
        }
        if (mycounter_total > 0) {
            mycounter_encode_display = (uint8_t)(100 * aggregate / mycounter_total);
            mycounter_ecm_encode_display = (uint8_t)(100 * aggregate / mycounter_total);
        }
        if (mycounter_audio_comp_total > 0)
            mycounter_audio_comp_encode_display = (uint8_t)(100 * aggregate_audio / mycounter_audio_comp_total);
        if (mycounter_data_comp_total > 0)
            mycounter_data_comp_encode_display = (uint8_t)(100 * aggregate_data / mycounter_data_comp_total);
        mycounter_audio_comp_progress.store(aggregate_audio, std::memory_order_relaxed);
        mycounter_data_comp_progress.store(aggregate_data, std::memory_order_relaxed);
        mycounter_ecm_progress.store(aggregate, std::memory_order_relaxed);
        mycounter_progress.store(aggregate, std::memory_order_relaxed);
        encode_progress();
    });

    for (uint32_t t = 0; t < actual_jobs; t++) {
        threads.emplace_back(worker);
    }
    for (auto &th : threads) {
        th.join();
    }
done.store(true, std::memory_order_relaxed);
    display_thread.join();

    if (g_interrupted.load(std::memory_order_relaxed)) {
        fprintf(stderr, "\nInterrupted during parallel encode.\n");
        return ECMTOOL_PROCESSING_ERROR;
    }

    for (uint32_t i = 0; i < num_streams; i++) {
        if (results[i].error != ECMTOOL_OK) {
            fprintf(stderr, "\nThere was an error encoding stream %u\n", i);
            return results[i].error;
        }
    }

    // Combine per-stream partial EDCs using edc_combine
    uint32_t total_edc = 0;
    for (uint32_t i = 0; i < num_streams; i++) {
        total_edc = sTools->edc_combine(total_edc, results[i].partial_edc, results[i].byte_count);
    }

    for (uint32_t i = 0; i < num_streams; i++) {
        out_file.write(reinterpret_cast<char*>(results[i].data.data()), results[i].data.size());
        if (!out_file.good()) {
            return ECMTOOL_FILE_WRITE_ERROR;
        }
        streams_script[i].stream_data.out_end_position = (uint64_t)out_file.tellp() - ecm_block_start_position;

        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            (*sectors_type)[streams_script[i].sectors_data[j].mode] += streams_script[i].sectors_data[j].sector_count;
        }
    }

    uint8_t buffer_edc[4];
    sTools->put32lsb(buffer_edc, total_edc);
    out_file.write(reinterpret_cast<char*>(buffer_edc), 4);

    return ECMTOOL_OK;
}


struct stream_decode_result {
    temp_file temp;
    uint32_t sector_count;
    ecm3_return_code error;
};

static ecm3_return_code decode_single_stream(
    const std::string &in_filename,
    const std::string &temp_dir,
    uint32_t stream_index,
    const stream_script &script,
    ecm_options *options,
    uint64_t stream_start_pos,
    uint64_t ecm_block_start_position,
    uint32_t start_sector_index,
    stream_decode_result &result,
    std::atomic<uint64_t> *progress = nullptr,
    const std::vector<uint8_t>* preset_dict = nullptr
) {
    result.sector_count = 0;
    result.error = ECMTOOL_OK;

    namespace fs = std::filesystem;
    result.temp = temp_file((fs::path(temp_dir) / ("ecm3_decode_stream_" + std::to_string(stream_index) + ".tmp")).string());

    mmap_file mfile;
    bool use_mmap = mfile.open(in_filename);
    std::ifstream in_file_fallback;
    const uint8_t *mmap_data = nullptr;
    uint64_t mmap_cursor = 0;

    if (use_mmap) {
        mmap_data = mfile.data();
        mmap_cursor = stream_start_pos + ecm_block_start_position;
    } else {
        in_file_fallback.open(in_filename, std::ios::binary);
        if (!in_file_fallback.good()) {
            result.error = ECMTOOL_FILE_READ_ERROR;
            return result.error;
        }
        in_file_fallback.seekg(stream_start_pos + ecm_block_start_position, std::ios_base::beg);
    }

    std::ofstream out_tmp(result.temp.path(), std::ios::binary);
    if (!out_tmp.good()) {
        result.error = ECMTOOL_FILE_WRITE_ERROR;
        return result.error;
    }

    uint8_t in_sector[2352];
    uint8_t out_sector[2352];
    compressor *decompobj = NULL;
    uint8_t *decomp_buffer = NULL;
    sector_tools local_sTools;
    uint32_t current_sector = start_sector_index;

    uint16_t effective_compression_val = script.stream_data.compression;
    if (effective_compression_val) {
        size_t stream_size = script.stream_data.out_end_position - stream_start_pos;
        if (stream_size == 0) {
            effective_compression_val = C_NONE;
        }
        if (effective_compression_val) {
            decomp_buffer = (uint8_t*)malloc(BUFFER_SIZE);
            if (!decomp_buffer) {
                out_tmp.close();
                result.error = ECMTOOL_BUFFER_MEMORY_ERROR;
                return result.error;
            }
            size_t to_read = BUFFER_SIZE;
            if (to_read > stream_size) {
                to_read = stream_size;
            }
            if (use_mmap) {
                if (to_read > 0) {
                    memcpy(decomp_buffer, mmap_data + mmap_cursor, to_read);
                    mmap_cursor += to_read;
                }
            } else {
                if (to_read > 0) {
                    in_file_fallback.read(reinterpret_cast<char*>(decomp_buffer), to_read);
                    if (!in_file_fallback.good()) {
                        free(decomp_buffer);
                        out_tmp.close();
                        result.error = ECMTOOL_FILE_READ_ERROR;
                        return result.error;
                    }
                }
            }
            decompobj = new compressor((sector_tools_compression)effective_compression_val, false);
            if (preset_dict && !preset_dict->empty() &&
                (sector_tools_compression)effective_compression_val == C_ZLIB &&
                script.stream_data.cross_stream_dict) {
                decompobj->set_dictionary(preset_dict->data(), preset_dict->size());
            }
            decompobj->set_input(decomp_buffer, to_read);
        }
    }

    for (uint32_t j = 0; j < script.sectors_data.size(); j++) {
        for (uint32_t k = 0; k < script.sectors_data[j].sector_count; k++) {
            CHECK_INTERRUPT();
            size_t bytes_to_read = 0;
            local_sTools.encoded_sector_size(
                (sector_tools_types)script.sectors_data[j].mode,
                bytes_to_read,
                options->optimizations
            );

            size_t decompress_buffer_left = 0;
            switch (effective_compression_val) {
            case C_NONE:
                if (use_mmap) {
                    memcpy(in_sector, mmap_data + mmap_cursor, bytes_to_read);
                    mmap_cursor += bytes_to_read;
                } else {
                    in_file_fallback.read(reinterpret_cast<char*>(in_sector), bytes_to_read);
                    if (!in_file_fallback.good()) {
                        if (decompobj) delete decompobj;
                        if (decomp_buffer) free(decomp_buffer);
                        out_tmp.close();
                        result.error = ECMTOOL_FILE_READ_ERROR;
                        return result.error;
                    }
                }
                break;

            case C_ZLIB:
            case C_LZMA:
            case C_LZMA2:
            case C_LZ4:
            case C_FLAC:
            case C_ZSTD:
            case C_WAVPACK:
                {
                    int8_t decomp_rc = decompobj->decompress(in_sector, bytes_to_read, decompress_buffer_left, Z_SYNC_FLUSH);
                    if (is_decompress_error(decomp_rc)) {
                        if (decompobj) delete decompobj;
                        if (decomp_buffer) free(decomp_buffer);
                        result.error = ECMTOOL_CORRUPTED_STREAM;
                        return result.error;
                    }
                }

                {
                    uint64_t consumed_offset = use_mmap ? mmap_cursor : (uint64_t)in_file_fallback.tellg();
                    if (script.stream_data.out_end_position > (consumed_offset - ecm_block_start_position) && decompress_buffer_left < (BUFFER_SIZE * 0.25)) {
                        size_t position = BUFFER_SIZE - decompress_buffer_left;
                        memmove(decomp_buffer, decomp_buffer + position, decompress_buffer_left);
                        size_t to_read = BUFFER_SIZE - decompress_buffer_left;
                        size_t stream_size = script.stream_data.out_end_position - (consumed_offset - ecm_block_start_position);
                        if (to_read > stream_size) {
                            to_read = stream_size;
                        }
                        if (use_mmap) {
                            memcpy(decomp_buffer + decompress_buffer_left, mmap_data + mmap_cursor, to_read);
                            mmap_cursor += to_read;
                        } else {
                            in_file_fallback.read(reinterpret_cast<char*>(decomp_buffer + decompress_buffer_left), to_read);
                        }
                        size_t input_size = decompress_buffer_left + to_read;
                        decompobj->set_input(decomp_buffer, input_size);
                    }
                }
                break;
            }

            uint16_t bytes_read = 0;
            local_sTools.regenerate_sector(
                out_sector,
                in_sector,
                (sector_tools_types)script.sectors_data[j].mode,
                current_sector + 0x96,
                bytes_read,
                options->optimizations
            );

            out_tmp.write(reinterpret_cast<char*>(out_sector), 2352);
            if (!out_tmp.good()) {
                if (decompobj) { delete decompobj; decompobj = NULL; }
            if (decomp_buffer) { free(decomp_buffer); }
            out_tmp.close();
            result.error = ECMTOOL_FILE_WRITE_ERROR;
            return result.error;
            }

            result.sector_count++;
            if (progress) progress->store(static_cast<uint64_t>(result.sector_count) * 2352, std::memory_order_relaxed);
            current_sector++;
        }
    }

    if (decompobj) { delete decompobj; decompobj = NULL; }
    if (decomp_buffer) { free(decomp_buffer); decomp_buffer = NULL; }

    if (!use_mmap) in_file_fallback.close();
    out_tmp.close();
    result.error = ECMTOOL_OK;
    return result.error;
}

static ecm3_return_code disk_decode_sequential(
    sector_tools *sTools,
    std::ifstream &in_file,
    std::fstream &out_file,
    std::vector<stream_script> &streams_script,
    ecm_options *options,
    uint64_t ecm_block_start_position
) {
    uint8_t in_sector[2352];
    uint8_t out_sector[2352];
    uint32_t current_sector = 0;
    uint32_t original_edc = 0;
    uint32_t output_edc = 0;

    // Per-type rolling dictionaries for cross-stream decompression.
    std::vector<uint8_t> audio_dict, data_dict;

    for (uint32_t i = 0; i < streams_script.size(); i++) {
        compressor *decompobj = NULL;
        uint8_t *decomp_buffer = NULL;

        sector_tools_compression effective_comp = (sector_tools_compression)streams_script[i].stream_data.compression;
        if (effective_comp) {
            size_t stream_size = streams_script[i].stream_data.out_end_position - ((uint64_t)in_file.tellg() - ecm_block_start_position);
            if (stream_size == 0) {
                effective_comp = C_NONE;
            }
        }
        if (effective_comp) {
            decomp_buffer = (uint8_t*) malloc(BUFFER_SIZE);
            if(!decomp_buffer) {
                fprintf(stderr, "Out of memory\n");
                return ECMTOOL_BUFFER_MEMORY_ERROR;
            }
            size_t stream_size = streams_script[i].stream_data.out_end_position - ((uint64_t)in_file.tellg() - ecm_block_start_position);
            size_t to_read = BUFFER_SIZE;
            if (to_read > stream_size) {
                to_read = stream_size;
            }
            in_file.read(reinterpret_cast<char*>(decomp_buffer), to_read);
            decompobj = new compressor(effective_comp, false);
            // Cross-stream preset dictionary for zlib streams
            if (effective_comp == C_ZLIB &&
                streams_script[i].stream_data.cross_stream_dict) {
                auto& dict = streams_script[i].stream_data.type ? data_dict : audio_dict;
                if (!dict.empty()) {
                    decompobj->set_dictionary(dict.data(), dict.size());
                }
            }
            decompobj -> set_input(decomp_buffer, to_read);
        }

        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            for (uint32_t k = 0; k < streams_script[i].sectors_data[j].sector_count; k++) {
                CHECK_INTERRUPT();
                if (in_file.eof()){
                    fprintf(stderr, "Unexpected EOF detected.\n");
                    return ECMTOOL_FILE_READ_ERROR;
                }

                size_t bytes_to_read = 0;
                sTools->encoded_sector_size(
                    (sector_tools_types)streams_script[i].sectors_data[j].mode,
                    bytes_to_read,
                    options->optimizations
                );

                size_t decompress_buffer_left = 0;
                switch (effective_comp) {
                case C_NONE:
                    in_file.read(reinterpret_cast<char*>(in_sector), bytes_to_read);
                    setcounter_decode((uint64_t)in_file.tellg() - ecm_block_start_position);
                    break;

                case C_ZLIB:
                case C_LZMA:
                case C_LZMA2:
                case C_LZ4:
                case C_FLAC:
                case C_ZSTD:
                case C_WAVPACK:
                    {
                        int8_t decomp_rc = decompobj->decompress(in_sector, bytes_to_read, decompress_buffer_left, Z_SYNC_FLUSH);
                        if (is_decompress_error(decomp_rc)) {
                            if (decompobj) { delete decompobj; decompobj = NULL; }
                            if (decomp_buffer) { free(decomp_buffer); decomp_buffer = NULL; }
                            fprintf(stderr, "\n\nERROR: Decompression failed (error code %d).\n", decomp_rc);
                            return ECMTOOL_CORRUPTED_STREAM;
                        }
                    }
                    setcounter_decode((uint64_t)in_file.tellg() - decompress_buffer_left - ecm_block_start_position);

                    if (streams_script[i].stream_data.out_end_position > (uint64_t)in_file.tellg() && decompress_buffer_left < (BUFFER_SIZE * 0.25)) {
                        size_t position = BUFFER_SIZE - decompress_buffer_left;
                        memmove(decomp_buffer, decomp_buffer + position, decompress_buffer_left);
                        size_t to_read = BUFFER_SIZE - decompress_buffer_left;
                        size_t stream_size = streams_script[i].stream_data.out_end_position - ((uint64_t)in_file.tellg() - ecm_block_start_position);
                        if (to_read > stream_size) {
                            to_read = stream_size;
                        }
                        in_file.read(reinterpret_cast<char*>(decomp_buffer + decompress_buffer_left), to_read);
                        size_t input_size = decompress_buffer_left + to_read;
                        decompobj -> set_input(decomp_buffer, input_size);
                    }
                }

                // Capture cleaned/decompressed data for cross-stream dictionary
                {
                    auto& dict = streams_script[i].stream_data.type ? data_dict : audio_dict;
                    update_dict(dict, in_sector, bytes_to_read);
                }

                uint16_t bytes_read = 0;
                sTools->regenerate_sector(
                    out_sector,
                    in_sector,
                    (sector_tools_types)streams_script[i].sectors_data[j].mode,
                    current_sector + 0x96,
                    bytes_read,
                    options->optimizations
                );

                out_file.write(reinterpret_cast<char*>(out_sector), 2352);
                {
                    auto st = (sector_tools_types)streams_script[i].sectors_data[j].mode;
                    bool has_ecc = (st == STT_MODE1 || st == STT_MODE1_GAP || st == STT_MODE1_RAW ||
                                    st == STT_MODE2_1 || st == STT_MODE2_1_GAP);
                    uint32_t edc_size = (has_ecc && (options->optimizations & OO_REMOVE_ECC)) ? 0x81C : 2352;
                    output_edc = sTools->edc_compute(output_edc, out_sector, edc_size);
                }
                current_sector++;
            }
        }

        if (decompobj) { delete decompobj; decompobj = NULL; }
        if (decomp_buffer) { free(decomp_buffer); }
    }

    setcounter_decode((uint64_t)in_file.tellg());

    uint8_t buffer_edc[4];
    in_file.read(reinterpret_cast<char*>(buffer_edc), 4);
    original_edc = sTools->get32lsb(buffer_edc);

    if (original_edc == output_edc) {
        return ECMTOOL_OK;
    }
    else {
        fprintf(stderr, "\n\nWrong CRC!... Maybe the input file is damaged.\n");
        return ECMTOOL_PROCESSING_ERROR;
    }
}


static ecm3_return_code disk_decode (
    sector_tools *sTools,
    std::ifstream &in_file,
    std::fstream &out_file,
    std::vector<stream_script> &streams_script,
    ecm_options *options,
    uint64_t ecm_block_start_position
) {
    uint32_t num_jobs = options->jobs;
    if (num_jobs == 0) {
        num_jobs = std::thread::hardware_concurrency();
        if (num_jobs == 0) num_jobs = 1;
    }

    if (num_jobs <= 1 || streams_script.size() <= 1) {
        return disk_decode_sequential(sTools, in_file, out_file, streams_script, options, ecm_block_start_position);
    }

    // Cross-stream dictionary requires sequential decode because the dict for
    // each stream depends on the already-decompressed data of earlier streams.
    for (auto& s : streams_script) {
        if (s.stream_data.cross_stream_dict) {
            return disk_decode_sequential(sTools, in_file, out_file, streams_script, options, ecm_block_start_position);
        }
    }

    uint32_t num_streams = (uint32_t)streams_script.size();
    std::vector<stream_decode_result> results(num_streams);
    std::string in_filename = options->in_filename;

    namespace fs = std::filesystem;
    std::string temp_dir = (fs::temp_directory_path() / "ecm3_decode").string();
    fs::create_directories(temp_dir);
    scope_guard clean_temp_dir([&temp_dir]() { std::error_code ec; fs::remove_all(temp_dir, ec); });

    // Compute start positions for each stream
    std::vector<uint64_t> stream_start_pos(num_streams, 0);
    stream_start_pos[0] = (uint64_t)in_file.tellg() - ecm_block_start_position;
    for (uint32_t i = 1; i < num_streams; i++) {
        stream_start_pos[i] = streams_script[i - 1].stream_data.out_end_position;
    }

    // Compute start sector index for each stream
    std::vector<uint32_t> start_sector_idx(num_streams, 0);
    for (uint32_t i = 1; i < num_streams; i++) {
        for (uint32_t j = 0; j < streams_script[i - 1].sectors_data.size(); j++) {
            start_sector_idx[i] += streams_script[i - 1].sectors_data[j].sector_count;
        }
        start_sector_idx[i] += start_sector_idx[i - 1];
    }

    uint32_t actual_jobs = std::min(num_jobs, num_streams);
    std::vector<std::thread> threads;
    std::atomic<uint32_t> next_stream{0};
    std::vector<std::atomic<uint64_t>> stream_progress(num_streams);
    for (uint32_t i = 0; i < num_streams; i++) stream_progress[i].store(0, std::memory_order_relaxed);

    uint64_t total_decode_bytes = 0;
    for (uint32_t i = 0; i < num_streams; i++) {
        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            total_decode_bytes += (uint64_t)streams_script[i].sectors_data[j].sector_count * 2352;
        }
    }

    auto worker = [&]() {
        while (true) {
            uint32_t idx = next_stream.fetch_add(1);
            if (idx >= num_streams) break;
            decode_single_stream(
                in_filename,
                temp_dir,
                idx,
                streams_script[idx],
                options,
                stream_start_pos[idx],
                ecm_block_start_position,
                start_sector_idx[idx],
                results[idx],
                &stream_progress[idx]
            );
        }
    };

    setcounter_decode_mode(true);
    std::atomic<bool> decode_done{false};
    std::thread display_thread([&]() {
        while (!decode_done.load(std::memory_order_relaxed) && !g_interrupted.load(std::memory_order_relaxed)) {
            uint64_t aggregate = 0;
            for (uint32_t i = 0; i < num_streams; i++)
                aggregate += stream_progress[i].load(std::memory_order_relaxed);
            if (total_decode_bytes > 0) {
                uint8_t pct = (uint8_t)(100 * aggregate / total_decode_bytes);
                if (pct != mycounter_decode_display) {
                    mycounter_decode_display = pct;
                    decode_progress();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        uint64_t aggregate = 0;
        for (uint32_t i = 0; i < num_streams; i++)
            aggregate += stream_progress[i].load(std::memory_order_relaxed);
        if (total_decode_bytes > 0) {
            uint8_t pct = (uint8_t)(100 * aggregate / total_decode_bytes);
            if (pct != mycounter_decode_display) {
                mycounter_decode_display = pct;
                decode_progress();
            }
        }
    });

    for (uint32_t t = 0; t < actual_jobs; t++) {
        threads.emplace_back(worker);
    }
    for (auto &th : threads) {
        th.join();
    }
    decode_done.store(true, std::memory_order_relaxed);
    display_thread.join();

    if (g_interrupted.load(std::memory_order_relaxed)) {
        fprintf(stderr, "\nInterrupted during parallel decode.\n");
        return ECMTOOL_PROCESSING_ERROR;
    }

    for (uint32_t i = 0; i < num_streams; i++) {
        if (results[i].error != ECMTOOL_OK) {
            fprintf(stderr, "\nThere was an error decoding stream %u\n", i);
            return results[i].error;
        }
    }

    uint32_t combined_edc = 0;
    for (uint32_t i = 0; i < num_streams; i++) {
        mmap_file tmp_mmap;
        if (!tmp_mmap.open(results[i].temp.path())) {
            return ECMTOOL_FILE_READ_ERROR;
        }

        const uint8_t *data = tmp_mmap.data();
        uint64_t remaining = (uint64_t)results[i].sector_count * 2352;
        out_file.write(reinterpret_cast<const char*>(data), remaining);
        if (!out_file.good()) {
            return ECMTOOL_FILE_WRITE_ERROR;
        }

        uint32_t sector_in_stream = 0;
        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            auto st = (sector_tools_types)streams_script[i].sectors_data[j].mode;
            bool has_ecc = (st == STT_MODE1 || st == STT_MODE1_GAP || st == STT_MODE1_RAW ||
                            st == STT_MODE2_1 || st == STT_MODE2_1_GAP);
            uint32_t edc_size = (has_ecc && (options->optimizations & OO_REMOVE_ECC)) ? 0x81C : 2352;
            for (uint32_t k = 0; k < streams_script[i].sectors_data[j].sector_count; k++) {
                combined_edc = sTools->edc_compute(combined_edc, data + sector_in_stream * 2352, edc_size);
                sector_in_stream++;
            }
        }

        tmp_mmap.unmap();
        results[i].temp.release();
    }

    // Read the stored CRC from after all stream data
    uint64_t last_end = streams_script.back().stream_data.out_end_position + ecm_block_start_position;
    in_file.seekg(last_end, std::ios_base::beg);
    uint8_t buffer_edc[4];
    in_file.read(reinterpret_cast<char*>(buffer_edc), 4);
    uint32_t original_edc = sTools->get32lsb(buffer_edc);

    if (original_edc == combined_edc) {
        return ECMTOOL_OK;
    }
    else {
        fprintf(stderr, "\n\nWrong CRC!... Maybe the input file is damaged.\n");
        return ECMTOOL_PROCESSING_ERROR;
    }
}


static sector_tools_compression parse_compression_name(const char* name);

/**
 * @brief Arguments parser for the program. It stores the options in the options struct
 * 
 * @param argc: Number of arguments passed to the program
 * @param argv: Array with the passed arguments
 * @param options: The output struct to place the parsed options
 * @return int: non zero on error
 */
int get_options(
    int argc,
    char **argv,
    ecm_options *options
) {
    char ch;
    // temporary variables for options parsing
    uint64_t temp_argument = 0;

    while ((ch = getopt_long(argc, argv, "i:o:a:d:c:esp:fkVSj:h", long_options, NULL)) != -1)
    {
        // check to see if a single character or long option came through
        switch (ch)
        {
            // short option '-i', long option '--input'
            case 'i':
                options->in_filename = optarg;
                break;

            // short option '-o', long option "--output"
            case 'o':
                options->out_filename = optarg;
                break;

            // short option '-a', long option "--acompression"
            // Audio compression option
            case 'a':
                options->audio_compression = parse_compression_name(optarg);
                if (options->audio_compression == C_NONE) {
                    fprintf(stderr, "ERROR: Unknown compression mode: %s\n\n", optarg);
                    print_help();
                    return 1;
                }
                break;

            // short option '-d', long option '--dcompression'
            // Data compression option
            case 'd':
                options->data_compression = parse_compression_name(optarg);
                if (options->data_compression == C_NONE) {
                    fprintf(stderr, "ERROR: Unknown compression mode: %s\n\n", optarg);
                    print_help();
                    return 1;
                }
                break;

            // short option '-c', long option "--clevel"
            case 'c':
                try {
                    std::string optarg_s(optarg);
                    temp_argument = std::stoi(optarg_s);

                    if (temp_argument > 9 || temp_argument < 0) {
                        fprintf(stderr, "ERROR: the provided compression level option is not correct.\n\n");
                        print_help();
                        return 1;
                    }
                    else {
                        options->compression_level = (uint8_t)temp_argument;
                    }
                } catch (std::exception const &e) {
                    fprintf(stderr, "ERROR: the provided compression level option is not correct.\n\n");
                    print_help();
                    return 1;
                }

                break;

            // short option '-e', long option "--extreme-compression" (only LZMA)
            case 'e':
                options->extreme_compression = true;
                break;

             // short option '-s', long option "--seekable"
            case 's':
                options->seekable = true;
                break;

            // short option '-p', long option "--sectors_per_block"
            case 'p':
                try {
                    std::string optarg_s(optarg);
                    temp_argument = std::stoi(optarg_s);

                    if (!temp_argument || temp_argument > 255 || temp_argument < 0) {
                        fprintf(stderr, "ERROR: the provided sectors per block number is not correct.\n\n");
                        print_help();
                        return 1;
                    }
                    else {
                        options->sectors_per_block = (uint8_t)temp_argument;
                    }
                } catch (std::exception const &e) {
                    fprintf(stderr, "ERROR: the provided sectors per block number is not correct.\n\n");
                    print_help();
                    return 1;
                }
                break;

            // short option '-f', long option "--force"
            case 'f':
                options->force_rewrite = true;
                break;

            // short option '-k', long option "--keep-output"
            case 'k':
                options->keep_output = true;
                break;

            // short option '-V', long option "--verify"
            case 'V':
                options->verify = true;
                break;

            // short option '-S', long option "--split"
            case 'S':
                options->split_output = true;
                break;

            // long option "--cue" (no short option)
            case 2:
                options->cue_filename = optarg;
                break;

            // short option '-j', long option "--jobs"
            case 'j':
                try {
                    std::string optarg_s(optarg);
                    temp_argument = std::stoi(optarg_s);
                    if (temp_argument < 0) {
                        fprintf(stderr, "ERROR: jobs must be 0 or a positive integer.\n\n");
                        print_help();
                        return 1;
                    }
                    options->jobs = (uint32_t)temp_argument;
                } catch (std::exception const &e) {
                    fprintf(stderr, "ERROR: the provided jobs number is not correct.\n\n");
                    print_help();
                    return 1;
                }
                break;

            // short option '-h', long option "--help"
            case 'h':
                print_help();
                return 2;

            // long option "--batch-cue"
            case 3:
                options->batch_cue_mode = true;
                options->batch_directory = optarg;
                break;

            // long option "--batch-decode"
            case 4:
                options->batch_decode_mode = true;
                options->batch_directory = optarg;
                break;

            // long option "--delete-source"
            case 5:
                options->delete_source = true;
                break;

            // long option "--split-cue"
            case 6:
                options->cue_split = true;
                options->cue_split_combine_file = optarg;
                break;

            // long option "--combine-cue"
            case 7:
                options->cue_combine = true;
                options->cue_split_combine_file = optarg;
                break;

            case '?':
                fprintf(stderr, "\n");
                print_help();
                return 3;
                break;
        }
    }

    return 0;
}


static void resetcounter(uint64_t total, sector_tools_compression audio_comp, sector_tools_compression data_comp, uint64_t audio_comp_total, uint64_t data_comp_total) {
    mycounter_progress.store(0, std::memory_order_relaxed);
    mycounter_analyze_display = 0;
    mycounter_encode_display = 0;
    mycounter_decode_display = 0;
    mycounter_audio_comp_encode_display = 0;
    mycounter_data_comp_encode_display = 0;
    mycounter_ecm_encode_display = 0;
    mycounter_total = total;
    mycounter_audio_comp_total = audio_comp_total;
    mycounter_data_comp_total = data_comp_total;
    mycounter_audio_comp_progress.store(0, std::memory_order_relaxed);
    mycounter_data_comp_progress.store(0, std::memory_order_relaxed);
    mycounter_ecm_progress.store(0, std::memory_order_relaxed);
    mycounter_is_decode.store(false, std::memory_order_relaxed);
    mycounter_start_time = std::chrono::steady_clock::now();
    mycounter_audio_compression = audio_comp;
    mycounter_data_compression = data_comp;
}


static void encode_progress(void) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - mycounter_start_time).count();
    double mb_done = MB(mycounter_progress.load(std::memory_order_relaxed));
    double throughput = (elapsed > 0.0) ? mb_done / elapsed : 0.0;

    bool show_audio = mycounter_audio_comp_total > 0 && mycounter_audio_compression != C_NONE;
    bool show_data = mycounter_data_comp_total > 0 && mycounter_data_compression != C_NONE;

    char buf[256];
    int pos = snprintf(buf, sizeof(buf),
        "Analyze(%02u%%) Encode(ECM)(%02u%%)",
        mycounter_analyze_display, mycounter_ecm_encode_display);

    if (show_data) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            " Data(%s)(%02u%%)",
            compression_name(mycounter_data_compression),
            mycounter_data_comp_encode_display);
    }
    if (show_audio) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            " Audio(%s)(%02u%%)",
            compression_name(mycounter_audio_compression),
            mycounter_audio_comp_encode_display);
    }

    int total_pct = mycounter_ecm_encode_display;
    int total_div = 1;
    if (show_data) { total_pct += mycounter_data_comp_encode_display; total_div++; }
    if (show_audio) { total_pct += mycounter_audio_comp_encode_display; total_div++; }
    total_pct = (total_pct + total_div / 2) / total_div;

    snprintf(buf + pos, sizeof(buf) - pos,
        " Total(%02u%%) %4.1fMB/s",
        total_pct, throughput);

#ifdef ECM3_GUI
    std::cerr << buf << "\n";
#else
    std::cerr << buf << "\r";
#endif

    if (g_progress_cb) g_progress_cb(total_pct);
}


static void decode_progress(void) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - mycounter_start_time).count();
    double mb_done = MB(mycounter_progress.load(std::memory_order_relaxed));
    double throughput = (elapsed > 0.0) ? mb_done / elapsed : 0.0;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s(%02u%%) %4.1fMB/s",
        mycounter_is_verify ? "Verify" : "Decode",
        mycounter_decode_display, throughput);

#ifdef ECM3_GUI
    std::cerr << buf << "\n";
#else
    std::cerr << buf << "\r";
#endif

    if (g_progress_cb) g_progress_cb(mycounter_decode_display);
}


static void setcounter_analyze(uint64_t n) {
    if (!mycounter_is_decode.load(std::memory_order_relaxed)) {
        uint64_t cur = mycounter_progress.load(std::memory_order_relaxed);
        uint64_t newval = n;
        if (newval > cur) mycounter_progress.store(newval, std::memory_order_relaxed);
        if (mycounter_total > 0) {
            uint8_t p = (uint8_t)(100 * n / mycounter_total);
            if (p != mycounter_analyze_display) {
                mycounter_analyze_display = p;
                encode_progress();
            }
        }
    }
}


static void setcounter_audio_comp_encode(uint64_t n) {
    if (!mycounter_is_decode.load(std::memory_order_relaxed)) {
        mycounter_audio_comp_progress.store(n, std::memory_order_relaxed);
        if (mycounter_audio_comp_total > 0) {
            uint8_t p = (uint8_t)(100 * n / mycounter_audio_comp_total);
            if (p != mycounter_audio_comp_encode_display) {
                mycounter_audio_comp_encode_display = p;
                encode_progress();
            }
        }
    }
}

static void setcounter_data_comp_encode(uint64_t n) {
    if (!mycounter_is_decode.load(std::memory_order_relaxed)) {
        mycounter_data_comp_progress.store(n, std::memory_order_relaxed);
        if (mycounter_data_comp_total > 0) {
            uint8_t p = (uint8_t)(100 * n / mycounter_data_comp_total);
            if (p != mycounter_data_comp_encode_display) {
                mycounter_data_comp_encode_display = p;
                encode_progress();
            }
        }
    }
}

static void setcounter_ecm_encode(uint64_t n) {
    if (!mycounter_is_decode.load(std::memory_order_relaxed)) {
        mycounter_ecm_progress.store(n, std::memory_order_relaxed);
        if (mycounter_total > 0) {
            uint8_t p = (uint8_t)(100 * n / mycounter_total);
            if (p != mycounter_ecm_encode_display) {
                mycounter_ecm_encode_display = p;
                encode_progress();
            }
        }
    }
}

static void setcounter_encode(uint64_t n) {
    if (!mycounter_is_decode.load(std::memory_order_relaxed)) {
        mycounter_progress.store(n, std::memory_order_relaxed);
        if (mycounter_total > 0) {
            uint8_t p = (uint8_t)(100 * n / mycounter_total);
            if (p != mycounter_encode_display) {
                mycounter_encode_display = p;
                encode_progress();
            }
        }
    }
}


static void setcounter_decode(uint64_t n) {
    if (mycounter_is_decode.load(std::memory_order_relaxed)) {
        mycounter_progress.store(n, std::memory_order_relaxed);
        if (mycounter_total > 0) {
            uint8_t p = (uint8_t)(100 * n / mycounter_total);
            if (p != mycounter_decode_display) {
                mycounter_decode_display = p;
                decode_progress();
            }
        }
    }
}


static void setcounter_decode_mode(bool is_decode) {
    mycounter_is_decode.store(is_decode, std::memory_order_relaxed);
}


static ecm3_return_code task_to_streams_header (
    stream *&streams_toc,
    sec_str_size &streams_toc_count,
    std::vector<stream_script> &streams_script
) {
    // Set the sizes
    streams_toc_count.count = streams_script.size();
    streams_toc_count.uncompressed_size = streams_toc_count.count * ECM_STREAM_SIZE;

    // Reserve the required memory. Must be freed later
    streams_toc = (stream *)calloc(streams_toc_count.count, sizeof(stream));
    if (!streams_toc) {
        fprintf(stderr, "There was an error reserving the memory for the stream toc.\n");
        return ECMTOOL_BUFFER_MEMORY_ERROR;
    }
    // Set the data
    for (uint32_t i = 0; i < streams_script.size(); i++) {
        streams_toc[i].compression = streams_script[i].stream_data.compression;
        streams_toc[i].end_sector = streams_script[i].stream_data.end_sector;
        streams_toc[i].out_end_position = streams_script[i].stream_data.out_end_position;
        streams_toc[i].type = streams_script[i].stream_data.type;
    }

    return ECMTOOL_OK;
}


static ecm3_return_code task_to_sectors_header (
    sector *&sectors_toc,
    sec_str_size &sectors_toc_count,
    std::vector<stream_script> &streams_script
) {
    // Set the sizes
    sectors_toc_count.count = 1;
    for (uint32_t i = 0; i < streams_script.size(); i++) {
        sectors_toc_count.count += streams_script[i].sectors_data.size();
    }
    sectors_toc_count.uncompressed_size = sectors_toc_count.count * ECM_SECTOR_SIZE;

    // Reserve the required memory. Must be freed later
    sectors_toc = (sector *)calloc(sectors_toc_count.count, sizeof(sector));
    if (!sectors_toc) {
        fprintf(stderr, "There was an error reserving the memory for the sectors toc.\n");
        return ECMTOOL_BUFFER_MEMORY_ERROR;
    }
    // Set the data
    uint32_t current_sector_data = 0;
    for (uint32_t i = 0; i < streams_script.size(); i++) {
        for (uint32_t j = 0; j < streams_script[i].sectors_data.size(); j++) {
            sectors_toc[current_sector_data].mode = streams_script[i].sectors_data[j].mode;
            sectors_toc[current_sector_data].sector_count = streams_script[i].sectors_data[j].sector_count;
            current_sector_data++;
        }
    }

    return ECMTOOL_OK;
}


/**
 * @brief Converts an standard streams & sectors streams to a STREAM_SCRIPT vector. Also will check
 *        if they are correct
 * 
 * 
 * @param stream_header The streams header with their data
 * @param stream_header_size The streams count in header
 * @param sectors_header The sectors header whith their data
 * @param sectors_header_size The sectors count in header
 * @param streams_script The output vector which will contains the script
 * @return int 
 */
static ecm3_return_code task_maker (
    stream *streams_toc,
    sec_str_size &streams_toc_count,
    sector *sectors_toc,
    sec_str_size &sectors_toc_count,
    std::vector<stream_script> &streams_script
) {
    size_t actual_sector = 0;
    uint32_t actual_sector_pos = 0;

    for (uint32_t i = 0; i < streams_toc_count.count; i++) {
        streams_script.push_back(stream_script());
        streams_script.back().stream_data = streams_toc[i];

        while (actual_sector < streams_toc[i].end_sector) {
            if (actual_sector_pos > sectors_toc_count.count) {
                // The streams sectors doesn't fit the sectors count
                // Headers could be corrupted
                fprintf(stderr, "There was an error generating the script. Maybe the header is corrupted.\n");
                return ECMTOOL_CORRUPTED_STREAM;
            }
            // Append the sector data to the current stream
            streams_script.back().sectors_data.push_back(sectors_toc[actual_sector_pos]);

            actual_sector += sectors_toc[actual_sector_pos].sector_count;
            actual_sector_pos++;
        }

        if (actual_sector > streams_toc[i].end_sector) {
            // The actual sector must be equal to last sector in stream, otherwise could be corrupted
            fprintf(stderr, "There was an error converting the TOC header to script.\n");
            //printf("Actual: %d - Stream End: %d.\n", actual_sector, streams_toc[i].end_sector);
            return ECMTOOL_PROCESSING_ERROR;
        }
    }

    return ECMTOOL_OK;
}


int compress_header (
    uint8_t *dest,
    uint32_t &destLen,
    uint8_t *source,
    uint32_t sourceLen,
    int level
) {
    z_stream strm;
    int err;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    strm.next_out = dest;
    strm.avail_out = destLen;
    strm.next_in = source;
    strm.avail_in = sourceLen;
    
    err = deflateInit(&strm, level);
    if (err != Z_OK) return err;

    err = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    destLen = strm.total_out;

    return err == Z_STREAM_END ? Z_OK : err;
}


void print_help() {
    banner();
    fprintf(stderr, 
        "Usage:\n"
        "\n"
        "  Encode:\n"
        "    ecm3 -i <bin>                     Encode a BIN to .ecm3\n"
        "    ecm3 -i <bin> -o <ecm3>           Encode with explicit output path\n"
        "    ecm3 --cue <cue>                   Encode with CUE sheet (auto-resolves BINs)\n"
        "    ecm3 -i <cue>                      Auto-detect BIN from CUE file\n"
        "\n"
        "  Decode:\n"
        "    ecm3 -i <ecm3>                     Decode to BIN (auto-names output)\n"
        "    ecm3 -i <ecm3> -o <bin>            Decode with explicit output path\n"
        "    ecm3 -i <ecm3> -S                  Decode to split per-track BINs + CUE\n"
        "\n"
        "  Notes:\n"
        "    - Passing a .cue file as input auto-embeds track metadata.\n"
        "    - A .ecm3 file with embedded metadata auto-generates a .cue on decode.\n"
        "    - --cue alone is sufficient (no -i needed); the CUE sheet references BINs.\n"
        "    - If both --cue and -i are given, --cue takes precedence.\n"
        "\n"
        "Options:\n"
        "  Compression:\n"
        "    -a, --acompression <mode>         Audio: zlib, lzma, lzma2, lz4, flac, zstd, wavpack\n"
        "    -d, --dcompression <mode>         Data: zlib, lzma, lzma2, lz4, zstd\n"
        "    -c, --clevel <0-9>                 Compression level\n"
        "    -e, --extreme-compression          Max ratio for LZMA/FLAC (very slow)\n"
        "\n"
        "  CUE / metadata:\n"
        "        --cue <cuefile>              Embed track metadata (encode only)\n"
        "    -S, --split                        Split into per-track BINs (decode only)\n"
        "\n"
        "  Seekable output:\n"
        "    -s, --seekable                     Enable seekable compression blocks\n"
        "    -p, --sectors-per-block <n>         Sectors per block (1-255, default 100)\n"
        "\n"
        "  Seekable mode creates independent compression reset points every N sectors.\n"
        "  This allows emulators and tools to seek directly to a sector group without\n"
        "  decompressing the entire stream. Trade-off: seekable output is slightly larger.\n"
        "\n"
        "  General:\n"
        "    -i, --input <file>                 Input file (.bin, .cue, or .ecm3)\n"
        "    -o, --output <file>                 Output file\n"
        "    -f, --force                        Overwrite existing output file\n"
        "    -k, --keep-output                   Keep output file on error\n"
        "    -V, --verify                       Verify .ecm3 integrity (decode to NUL + EDC check, no output written)\n"
        "    -j, --jobs <n>                       Parallel streams (0=auto, default 0)\n"
        "        --delete-source                  Delete source file after successful encode/decode\n"
        "\n"
    "  CUE Utilities:\n"
    "        --split-cue <cuefile>           Split combined .cue/.bin into per-track files\n"
    "        --combine-cue <cuefile>          Combine per-track .cue/.bin into single image\n"
    "        -o <dir>                          Required output directory (protects original CUE)\n"
    "        -f                                Overwrite existing files in output directory\n"
    "\n"
    "  Batch:\n"
    "        --batch-cue <dir>              Encode all .cue files in a directory tree\n"
    "        --batch-decode <dir>            Decode all .ecm3 files in a directory tree\n"
    );
}


static const char* compression_name(uint8_t comp) {
    switch ((sector_tools_compression)comp) {
        case C_NONE: return "none";
        case C_ZLIB: return "zlib";
        case C_LZMA: return "lzma";
        case C_LZ4:  return "lz4";
        case C_FLAC: return "flac";
        case C_LZMA2: return "lzma2";
        case C_ZSTD: return "zstd";
        case C_WAVPACK: return "wavpack";
        default: return "unknown";
    }
}

static sector_tools_compression parse_compression_name(const char* name) {
    if (strcmp("zlib", name) == 0) return C_ZLIB;
    if (strcmp("lzma", name) == 0) return C_LZMA;
    if (strcmp("lzma2", name) == 0) return C_LZMA2;
    if (strcmp("lz4", name) == 0) return C_LZ4;
    if (strcmp("flac", name) == 0) return C_FLAC;
    if (strcmp("zstd", name) == 0) return C_ZSTD;
    if (strcmp("wavpack", name) == 0) return C_WAVPACK;
    return C_NONE;
}

static void summary(
    std::vector<uint32_t> *sectors_type,
    ecm_options *options,
    size_t compressed_size,
    std::vector<stream_script> *streams_script
) {
    uint16_t optimized_sector_sizes[13];
    // Reference to sectors_type
    std::vector<uint32_t>& sectors_type_ref = *sectors_type;

    // Calculate the size per sector type
    for (uint8_t i = 1; i < 13; i++) {
        size_t bytes_to_read = 0;
        // Getting the sector size prior to read, to read the real sector size and avoid to fseek every time
        sector_tools::encoded_sector_size(
            (sector_tools_types)i,
            bytes_to_read,
            options->optimizations
        );
        optimized_sector_sizes[i] = bytes_to_read;
    }

    // Total sectors
    uint32_t total_sectors = 0;
    for (uint8_t i = 1; i < 13; i++) {
        total_sectors += sectors_type_ref[i];
    }

    // Total size
    size_t total_size = total_sectors * 2352;

    // ECM size without compression and check if there are data sectors
    size_t ecm_size = 0;
    bool it_has_data = false;
    for (uint8_t i = 1; i < 13; i++) {
        ecm_size += sectors_type_ref[i] * optimized_sector_sizes[i];
        if (i > 2 && sectors_type_ref[i] > 0) {
            it_has_data = true;
        }
    }

    fprintf(stdout, "\n\n");
    fprintf(stdout, " ECM cleanup summary\n");
    fprintf(stdout, "------------------------------------------------------------\n");
    fprintf(stdout, " Type               Sectors         In Size        Out Size\n");
    fprintf(stdout, "------------------------------------------------------------\n");
    fprintf(stdout, "CDDA ............... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[1], MB(sectors_type_ref[1] * 2352), MB(sectors_type_ref[1] * optimized_sector_sizes[1])); 
    fprintf(stdout, "CDDA Gap ........... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[2], MB(sectors_type_ref[2] * 2352), MB(sectors_type_ref[2] * optimized_sector_sizes[2]));
    fprintf(stdout, "Mode 1 ............. %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[3], MB(sectors_type_ref[3] * 2352), MB(sectors_type_ref[3] * optimized_sector_sizes[3]));
    fprintf(stdout, "Mode 1 Gap ......... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[4], MB(sectors_type_ref[4] * 2352), MB(sectors_type_ref[4] * optimized_sector_sizes[4]));
    fprintf(stdout, "Mode 1 RAW ......... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[5], MB(sectors_type_ref[5] * 2352), MB(sectors_type_ref[5] * optimized_sector_sizes[5]));
    fprintf(stdout, "Mode 2 ............. %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[6], MB(sectors_type_ref[6] * 2352), MB(sectors_type_ref[6] * optimized_sector_sizes[6]));
    fprintf(stdout, "Mode 2 Gap ......... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[7], MB(sectors_type_ref[7] * 2352), MB(sectors_type_ref[7] * optimized_sector_sizes[7]));
    fprintf(stdout, "Mode 2 XA1 ......... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[8], MB(sectors_type_ref[8] * 2352), MB(sectors_type_ref[8] * optimized_sector_sizes[8]));
    fprintf(stdout, "Mode 2 XA1 Gap ..... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[9], MB(sectors_type_ref[9] * 2352), MB(sectors_type_ref[9] * optimized_sector_sizes[9]));
    fprintf(stdout, "Mode 2 XA2 ......... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[10], MB(sectors_type_ref[10] * 2352), MB(sectors_type_ref[10] * optimized_sector_sizes[10]));
    fprintf(stdout, "Mode 2 XA2 Gap ..... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[11], MB(sectors_type_ref[11] * 2352), MB(sectors_type_ref[11] * optimized_sector_sizes[11]));
    fprintf(stdout, "Unknown data ....... %6d ...... %6.2fMB ...... %6.2fMB\n", sectors_type_ref[12], MB(sectors_type_ref[12] * 2352), MB(sectors_type_ref[12] * optimized_sector_sizes[12]));
    fprintf(stdout, "-------------------------------------------------------------\n");
    fprintf(stdout, "Total .............. %6d ...... %6.2fMB ...... %6.2fMB\n", total_sectors, MB(total_size), MB(ecm_size));
    fprintf(stdout, "ECM reduction (input vs ecm) ..................... %2.2f%%\n", total_size > 0 ? (1.0 - ((float)ecm_size / total_size)) * 100 : 0.0);
    fprintf(stdout, "\n\n");

    fprintf(stdout, " Compression Summary\n");
    fprintf(stdout, "-------------------------------------------------------------\n");
    fprintf(stdout, "Compressed size (output) ............... %3.2fMB\n", MB(compressed_size));
    fprintf(stdout, "Compression ratio (ecm vs output)....... %2.2f%%\n", ecm_size > 0 ? abs((1.0 - ((float)compressed_size / ecm_size)) * 100) : 0.0);
    fprintf(stdout, "\n\n");

    if (streams_script && streams_script->size() > 0 && (options->data_compression || options->audio_compression)) {
        fprintf(stdout, " Per-stream compression\n");
        fprintf(stdout, "-------------------------------------------------------------\n");
        uint64_t audio_in = 0, audio_out = 0, data_in = 0, data_out = 0;
        for (size_t i = 0; i < streams_script->size(); i++) {
            const stream_script& ss = (*streams_script)[i];
            uint32_t start_sector = (i == 0) ? 0 : (*streams_script)[i-1].stream_data.end_sector;
            uint32_t sector_count = ss.stream_data.end_sector - start_sector;
            size_t stream_in = (size_t)sector_count * 2352;
            size_t stream_ecm = 0;
            for (size_t j = 0; j < ss.sectors_data.size(); j++) {
                size_t bytes_per_sector = 0;
                sector_tools::encoded_sector_size(
                    (sector_tools_types)ss.sectors_data[j].mode,
                    bytes_per_sector,
                    options->optimizations
                );
                stream_ecm += ss.sectors_data[j].sector_count * bytes_per_sector;
            }
            uint64_t stream_compressed = ss.stream_data.out_end_position - ((i == 0) ? 0 : (*streams_script)[i-1].stream_data.out_end_position);
            float ratio = (stream_ecm > 0) ? (1.0f - ((float)stream_compressed / (float)stream_ecm)) * 100.0f : 0.0f;
            const char* stream_type = (ss.stream_data.type == 0) ? "audio" : "data";
            const char* comp_name = compression_name(ss.stream_data.compression);
            fprintf(stdout, "  Stream %2zu: %-5s  %-5s  %6u sectors  %6.2fMB in  %6.2fMB out  %5.1f%%\n",
                i + 1, stream_type, comp_name, sector_count, MB(stream_in), MB(stream_compressed), ratio);
            if (ss.stream_data.type == 0) {
                audio_in += stream_in;
                audio_out += stream_compressed;
            } else {
                data_in += stream_in;
                data_out += stream_compressed;
            }
        }
        fprintf(stdout, "-------------------------------------------------------------\n");
        if (audio_in > 0) {
            fprintf(stdout, "  Audio total:  %6.2fMB in  %6.2fMB out  %5.1f%%\n",
                MB(audio_in), MB(audio_out), (1.0f - ((float)audio_out / (float)audio_in)) * 100.0f);
        }
        if (data_in > 0) {
            fprintf(stdout, "  Data total:   %6.2fMB in  %6.2fMB out  %5.1f%%\n",
                MB(data_in), MB(data_out), (1.0f - ((float)data_out / (float)data_in)) * 100.0f);
        }
        fprintf(stdout, "\n");
    }

    fprintf(stdout, " Output summary\n");
    fprintf(stdout, "-------------------------------------------------------------\n");
    fprintf(stdout, "Total reduction (input vs output) ...... %2.2f%%\n", total_size > 0 ? abs((1.0 - ((float)compressed_size / total_size)) * 100) : 0.0);
    if (options->seekable) {
        uint32_t block_count = (total_sectors + options->sectors_per_block - 1) / options->sectors_per_block;
        uint32_t last_block_sectors = total_sectors - (block_count - 1) * options->sectors_per_block;
        if (last_block_sectors == 0 || last_block_sectors > options->sectors_per_block) last_block_sectors = options->sectors_per_block;
        fprintf(stdout, "Seekable blocks ......... %6d (%d sectors/block, last %d sectors)\n", block_count, options->sectors_per_block, last_block_sectors);
    }

    if (!it_has_data) {
        printf("\nWARNING: The image looks like an Audio CD. If not, verify that the image is not damaged\n\n");
    }
}

/**
 * @brief Function to detect the PSX game ID. It reads the entire block and try to find the ID's codes.
 * 
 * @param id String to store the detected ID
 * @param data Data where to search
 * @param data_size Data size
 * @return int: -1 if was not found, 0 if was found, 1 if there was an error
 */
int detect_id_psx(std::string &id, uint8_t *data, uint64_t data_size) {
    // if length is less than the min size, return error
    if (data_size < 11) {
        return 1;
    }
    // Step through the data to try to detect the ID
    for (uint64_t i = 0; i < data_size - 11; i++) {
        // if bytes are similar to S##S_###.##, then return its data
        if (
            data[i] == 'S' &&
            data[i + 3] == 'S' &&
            data[i + 4] == '_' &&
            data[i + 8] == '.'
        ) {
            id += data[i];
            id += data[i + 1];
            id += data[i + 2];
            id += data[i + 3];
            id += data[i + 5];
            id += data[i + 6];
            id += data[i + 7];
            id += data[i + 9];
            id += data[i + 10];
            return 0;
        }
    }
    return -1;
}
