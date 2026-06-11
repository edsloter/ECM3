// ecm3 - Enhanced ECM GUI Tool
// Copyright (C) 2026 Edward Sloter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/statbox.h>
#include <wx/spinctrl.h>
#include <wx/checkbox.h>
#include <wx/gauge.h>
#include <wx/filepicker.h>
#include <wx/choice.h>
#include <wx/thread.h>
#include <wx/msgdlg.h>
#include <wx/dnd.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif
#include "ecm3.h"
#include "ecm3_core.h"
#include "metadata.h"
#include "cue_parser.h"
#include "cue_gen.h"
#include "util.h"
#include "cuesplit.h"

#include <sstream>
#include <streambuf>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>

static std::atomic<bool> g_gui_interrupted{false};

// ── Thread-safe stream redirector: captures cout/cerr into a wxTextCtrl ──
// Uses a mutex + wxCommandEvent to safely post text from any thread.
wxDEFINE_EVENT(EVT_APPEND_TEXT, wxCommandEvent);
wxDEFINE_EVENT(EVT_PROGRESS_GAUGE, wxCommandEvent);

static void on_progress_update(int percent) {
    wxWindow* top = wxTheApp->GetTopWindow();
    if (top) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_PROGRESS_GAUGE);
        evt->SetInt(percent);
        wxQueueEvent(top, evt);
    }
}

class TextCtrlStream : public std::streambuf {
    wxTextCtrl* m_text;
    wxString m_pending;
    std::mutex m_mtx;
public:
    TextCtrlStream(wxTextCtrl* tc) : m_text(tc) {
        m_text->Bind(EVT_APPEND_TEXT, [this](wxCommandEvent& e) {
            m_text->AppendText(e.GetString());
        });
    }
protected:
    int overflow(int c) override {
        if (c == '\n') {
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_pending += '\n';
            }
            flush();
        } else {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_pending += static_cast<wchar_t>(c);
        }
        return c;
    }
    int sync() override { flush(); return 0; }
private:
    void flush() {
        wxString text;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            text.swap(m_pending);
        }
        if (!text.empty()) {
            wxCommandEvent* evt = new wxCommandEvent(EVT_APPEND_TEXT);
            evt->SetString(text);
            wxQueueEvent(m_text, evt);
        }
    }
};

// ── Drop target for file drag-and-drop ──
class FileDropTarget : public wxFileDropTarget {
public:
    FileDropTarget(wxFilePickerCtrl* picker) : m_picker(picker) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override {
        if (!filenames.IsEmpty()) {
            m_picker->SetPath(filenames[0]);
        }
        return true;
    }
private:
    wxFilePickerCtrl* m_picker;
};

static sector_tools_compression compression_from_string(const std::string& s) {
    if (s == "zlib") return C_ZLIB;
    if (s == "lzma") return C_LZMA;
    if (s == "lz4") return C_LZ4;
    if (s == "flac") return C_FLAC;
    if (s == "lzma2") return C_LZMA2;
if (s == "zstd")  return C_ZSTD;
    if (s == "wavpack") return C_WAVPACK;
    return C_NONE;
}

// ── Main frame ──
class Ecm3Frame : public wxFrame {
public:
    Ecm3Frame() : wxFrame(nullptr, wxID_ANY, "ecm3 v" VERSI " - Enhanced ECM Tool",
                           wxDefaultPosition, wxSize(900, 700)) {
#if defined(_WIN32)
        {
            HICON hIcon = ::LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
            if (hIcon) {
                wxIcon icon;
                icon.CreateFromHICON(hIcon);
                SetIcon(icon);
                ::DestroyIcon(hIcon);
            }
        }
#endif
        auto* panel = new wxPanel(this);
        auto* topSizer = new wxBoxSizer(wxVERTICAL);

        m_notebook = new wxNotebook(panel, wxID_ANY);

        CreateEncodeTab();
        CreateDecodeTab();
        CreateBatchEncodeTab();
        CreateBatchDecodeTab();
        CreateCueSplitTab();
        CreateSettingsTab();
        CreateAboutTab();

        topSizer->Add(m_notebook, 1, wxEXPAND | wxALL, 5);

        auto* logBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Output");
        m_output = new wxTextCtrl(panel, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxSize(-1, 200),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        logBox->Add(m_output, 1, wxEXPAND | wxALL, 5);
        topSizer->Add(logBox, 0, wxEXPAND | wxALL, 5);

        m_progressGauge = new wxGauge(panel, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize,
                                      wxGA_HORIZONTAL | wxGA_SMOOTH);
        topSizer->Add(m_progressGauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

        auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
        m_runBtn = new wxButton(panel, wxID_ANY, "Run");
        m_runBtn->Bind(wxEVT_BUTTON, &Ecm3Frame::OnRun, this);
        auto* clearBtn = new wxButton(panel, wxID_ANY, "Clear Output");
        clearBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_output->Clear(); });
        auto* quitBtn = new wxButton(panel, wxID_EXIT, "Quit");
        quitBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

        Bind(EVT_PROGRESS_GAUGE, [this](wxCommandEvent& e) {
            m_progressGauge->SetValue(e.GetInt());
        });

        btnSizer->Add(m_runBtn, 0, wxRIGHT, 5);
        btnSizer->Add(clearBtn, 0, wxRIGHT, 5);
        btnSizer->AddStretchSpacer();
        btnSizer->Add(quitBtn, 0, wxRIGHT, 5);
        topSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 5);

        panel->SetSizer(topSizer);
        CreateStatusBar();
        SetStatusText("Ready");

        m_coutBuf = std::cout.rdbuf();
        m_cerrBuf = std::cerr.rdbuf();
        m_stream = new TextCtrlStream(m_output);
        std::cout.rdbuf(m_stream);
        std::cerr.rdbuf(m_stream);
    }

    ~Ecm3Frame() override {
        if (m_worker.joinable()) {
            m_worker.detach();
        }
        g_gui_interrupted.store(true);
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::cout.rdbuf(m_coutBuf);
        std::cerr.rdbuf(m_cerrBuf);
        delete m_stream;
    }

wxNotebook* m_notebook;

    // Encode tab
    wxPanel* m_encodePanel;
    wxFilePickerCtrl* m_encodeInput;
    wxFilePickerCtrl* m_encodeCue;
    wxFilePickerCtrl* m_encodeOutput;
    wxChoice* m_encodeAudioComp;
    wxChoice* m_encodeDataComp;
    wxSpinCtrl* m_encodeLevel;
    wxCheckBox* m_encodeExtreme;
    wxCheckBox* m_encodeSeekable;
    wxSpinCtrl* m_encodeSectorsPerBlock;
    wxStaticText* m_encodeSpbLabel;
    wxCheckBox* m_encodeForce;
    wxCheckBox* m_encodeDeleteSource;

    // Decode tab
    wxPanel* m_decodePanel;
    wxFilePickerCtrl* m_decodeInput;
    wxFilePickerCtrl* m_decodeOutput;
    wxCheckBox* m_decodeSplit;
    wxCheckBox* m_decodeForce;
    wxCheckBox* m_decodeVerify;
    wxCheckBox* m_decodeDeleteSource;

private:
    wxPanel* m_batchEncodePanel;
    wxDirPickerCtrl* m_batchEncodeDir;
    wxChoice* m_batchEncodeAudioComp;
    wxChoice* m_batchEncodeDataComp;
    wxSpinCtrl* m_batchEncodeLevel;
    wxCheckBox* m_batchEncodeExtreme;
    wxCheckBox* m_batchEncodeSeekable;
    wxSpinCtrl* m_batchEncodeSectorsPerBlock;
    wxStaticText* m_batchEncodeSpbLabel;
    wxCheckBox* m_batchEncodeForce;
    wxCheckBox* m_batchEncodeDeleteSource;
    wxDirPickerCtrl* m_batchEncodeOutputDir;

    // Batch Decode tab
    wxPanel* m_batchDecodePanel;
    wxDirPickerCtrl* m_batchDecodeDir;
    wxDirPickerCtrl* m_batchDecodeOutputDir;
    wxCheckBox* m_batchDecodeSplit;
    wxCheckBox* m_batchDecodeForce;
    wxCheckBox* m_batchDecodeVerify;
    wxCheckBox* m_batchDecodeDeleteSource;

    // Settings tab
    wxPanel* m_settingsPanel;
    wxCheckBox* m_settingsAssoc;
    wxStaticText* m_settingsStatus;

    // About tab
    wxPanel* m_aboutPanel;

    // CUE Split/Combine tab
    wxPanel* m_cueSplitPanel;
    wxRadioBox* m_cueSplitMode;
    wxFilePickerCtrl* m_cueSplitInput;
    wxDirPickerCtrl* m_cueSplitOutput;
    wxCheckBox* m_cueSplitForce;

    wxTextCtrl* m_output;
    wxGauge* m_progressGauge;
    wxButton* m_runBtn;

    std::thread m_worker;
    std::atomic<bool> m_running{false};

    TextCtrlStream* m_stream = nullptr;
    std::streambuf* m_coutBuf = nullptr;
    std::streambuf* m_cerrBuf = nullptr;

    void OnRun(wxCommandEvent&) {
        if (m_running) {
            wxMessageBox("Operation already in progress.", "Busy", wxOK | wxICON_INFORMATION);
            return;
        }
        if (m_worker.joinable())
            m_worker.join();
        m_running = true;
        m_runBtn->Disable();
        m_output->Clear();
        m_progressGauge->SetValue(0);
        SetStatusText("Running...");
        g_gui_interrupted.store(false);
        g_gui_interrupted.store(false);

        int sel = m_notebook->GetSelection();

        set_progress_callback(on_progress_update);

        m_worker = std::thread([this, sel]() {
            int rc = 0;
            auto start = std::chrono::high_resolution_clock::now();

            if (sel == 0) {
                rc = RunEncode();
            } else if (sel == 1) {
                rc = RunDecode();
            } else if (sel == 2) {
                rc = RunBatchEncode();
            } else if (sel == 3) {
                rc = RunBatchDecode();
            } else if (sel == 4) {
                rc = RunCueSplit();
            }

            auto stop = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count();
            if (rc == 0) {
                std::cout << "\nThe file was processed without any problem\n";
                std::cout << "Total execution time: " << (ms / 1000.0f) << "s\n\n";
            } else if (g_gui_interrupted.load()) {
                std::cout << "\nInterrupted.\n";
            } else {
                std::cout << "\nError: operation failed (code " << rc << ").\n";
            }

            CallAfter([this]() {
                m_running = false;
                m_progressGauge->SetValue(100);
                m_runBtn->Enable();
                SetStatusText("Done");
                set_progress_callback(nullptr);
            });
        });
    }

    int RunEncode() {
        std::string inputPath = m_encodeInput->GetPath().ToStdString();
        std::string cuePath = m_encodeCue->GetPath().ToStdString();
        std::string outputPath = m_encodeOutput->GetPath().ToStdString();

        if (inputPath.empty()) {
            std::cerr << "ERROR: No input file specified.\n";
            return 1;
        }

        ecm_options opts;
        opts.in_filename = inputPath;
        opts.cue_filename = cuePath;
        opts.out_filename = outputPath;
        opts.audio_compression = compression_from_string(m_encodeAudioComp->GetStringSelection().ToStdString());
        opts.data_compression = compression_from_string(m_encodeDataComp->GetStringSelection().ToStdString());
        opts.compression_level = static_cast<uint8_t>(m_encodeLevel->GetValue());
        opts.extreme_compression = m_encodeExtreme->GetValue();
        opts.seekable = m_encodeSeekable->GetValue();
        opts.sectors_per_block = m_encodeSeekable->GetValue()
            ? static_cast<uint8_t>(m_encodeSectorsPerBlock->GetValue()) : 0;
        opts.force_rewrite = m_encodeForce->GetValue();
        opts.delete_source = m_encodeDeleteSource->GetValue();

        // Auto-detect CUE
        if (opts.cue_filename.empty()) {
            std::filesystem::path inPath(opts.in_filename);
            std::string ext = inPath.extension().string();
            if (ext == ".cue" || ext == ".CUE") {
                opts.cue_filename = opts.in_filename;
                cue_sheet autoCue;
                if (cue_parse(opts.cue_filename, autoCue) == 0 && !autoCue.file_order.empty()) {
                    std::string cueDir = get_cue_dir(opts.cue_filename);
                    opts.in_filename = (std::filesystem::path(cueDir) / autoCue.file_order[0]).string();
                    std::cout << "CUE file detected, using BIN: " << opts.in_filename << "\n";
                }
            } else {
                std::filesystem::path cuePath = inPath;
                cuePath.replace_extension(".cue");
                if (std::filesystem::exists(cuePath)) {
                    std::cout << "Auto-detected CUE: " << cuePath.filename().string() << "\n";
                    opts.cue_filename = cuePath.string();
                }
            }
        }

        // Derive output filename (use stem() to strip any extension)
        if (opts.out_filename.empty()) {
            std::filesystem::path base = !opts.cue_filename.empty()
                ? std::filesystem::path(opts.cue_filename)
                : std::filesystem::path(opts.in_filename);
            opts.out_filename = (base.parent_path() / (base.stem().string() + ".ecm3")).string();
        }

        bool has_cue = false;
        cue_sheet parsed_cue;
        temp_file temp_concat;

        if (!opts.cue_filename.empty()) {
            if (cue_parse(opts.cue_filename, parsed_cue) != 0) {
                std::cerr << "ERROR: Failed to parse CUE file: " << opts.cue_filename << "\n";
                return 1;
            }
            has_cue = true;
            std::string cue_dir = get_cue_dir(opts.cue_filename);
            if (parsed_cue.file_order.size() > 1) {
                if (concat_split_bins(parsed_cue, cue_dir, temp_concat) != 0) {
                    return 1;
                }
                opts.in_filename = temp_concat.path();
            } else {
                opts.in_filename = (std::filesystem::path(cue_dir) / parsed_cue.file_order[0]).string();
            }
        }

        if (!opts.force_rewrite && std::filesystem::exists(opts.out_filename)) {
            std::cerr << "ERROR: output file already exists. Check 'Force overwrite' to overwrite.\n";
            return 1;
        }

        ecm3_result result;
        int rc = ecm3_encode(opts.in_filename, opts.out_filename, opts,
                              has_cue ? &parsed_cue : nullptr, has_cue, result);

        if (rc == 0 && opts.delete_source) {
            for (const auto& path : result.source_paths) {
                std::error_code ec;
                if (std::filesystem::remove(path, ec)) {
                    std::cout << "Deleted source: " << path << "\n";
                } else if (ec) {
                    std::cerr << "WARNING: Could not delete " << path << ": " << ec.message() << "\n";
                }
            }
        }

        return rc;
    }

    int RunDecode() {
        std::string inputPath = m_decodeInput->GetPath().ToStdString();
        std::string outputPath = m_decodeOutput->GetPath().ToStdString();

        if (inputPath.empty()) {
            std::cerr << "ERROR: No input file specified.\n";
            return 1;
        }

        ecm_options opts;
        opts.in_filename = inputPath;
        opts.out_filename = outputPath;
        opts.force_rewrite = m_decodeForce->GetValue();
        opts.verify = m_decodeVerify->GetValue();
        opts.split_output = m_decodeSplit->GetValue();
        opts.delete_source = m_decodeDeleteSource->GetValue();

        // Derive output filename
        if (opts.out_filename.empty()) {
            std::string in = opts.in_filename;
            if (in.size() >= 5 && in.substr(in.size() - 5) == ".ecm3") {
                in.resize(in.size() - 5);
            }
            opts.out_filename = in + ".bin";
        }

        if (!opts.verify && !opts.force_rewrite && std::filesystem::exists(opts.out_filename)) {
            std::cerr << "ERROR: output file already exists. Check 'Force overwrite' to overwrite.\n";
            return 1;
        }

        ecm3_result result;
        int rc = ecm3_decode(opts.in_filename, opts.out_filename, opts, result);

        if (rc == 0) {
            if (opts.verify) {
                std::cout << "VERIFY: Would write " << opts.out_filename << "\n";
                if (result.has_metadata) {
                    std::filesystem::path cue_path(opts.out_filename);
                    cue_path.replace_extension(".cue");
                    std::cout << "VERIFY: Would write " << cue_path.string() << "\n";
                    if (opts.split_output && result.meta_entries.size() > 1) {
                        std::cout << "VERIFY: Would write " << result.meta_entries.size()
                                  << " track file(s)\n";
                    }
                }
            } else {
                if (result.has_metadata) {
                    write_cue_from_metadata(result.meta_header, result.meta_entries,
                                            opts.out_filename, opts.split_output);
                }

                if (opts.split_output && result.has_metadata && result.meta_entries.size() > 1) {
                    if (split_output_bin(opts.out_filename, result.meta_header, result.meta_entries,
                                         result.meta_header.total_sectors, opts.force_rewrite) == 0) {
                        std::error_code ec;
                        std::filesystem::remove(opts.out_filename, ec);
                    }
                }
            }

            if (opts.delete_source) {
                for (const auto& path : result.source_paths) {
                    std::error_code ec;
                    if (std::filesystem::remove(path, ec)) {
                        std::cout << "Deleted source: " << path << "\n";
                    } else if (ec) {
                        std::cerr << "WARNING: Could not delete " << path << ": " << ec.message() << "\n";
                    }
                }
            }
        }

        return rc;
    }

    int RunBatchEncode() {
        std::string dir = m_batchEncodeDir->GetPath().ToStdString();
        if (dir.empty()) {
            std::cerr << "ERROR: No directory specified.\n";
            return 1;
        }

        ecm_options opts;
        opts.audio_compression = compression_from_string(m_batchEncodeAudioComp->GetStringSelection().ToStdString());
        opts.data_compression = compression_from_string(m_batchEncodeDataComp->GetStringSelection().ToStdString());
        opts.compression_level = static_cast<uint8_t>(m_batchEncodeLevel->GetValue());
        opts.extreme_compression = m_batchEncodeExtreme->GetValue();
        opts.seekable = m_batchEncodeSeekable->GetValue();
        opts.sectors_per_block = m_batchEncodeSeekable->GetValue()
            ? static_cast<uint8_t>(m_batchEncodeSectorsPerBlock->GetValue()) : 0;
        opts.delete_source = m_batchEncodeDeleteSource->GetValue();
        opts.batch_cue_mode = true;
        opts.batch_directory = dir;

        std::vector<std::string> batch_files;
        try {
            for (auto& p : std::filesystem::recursive_directory_iterator(dir)) {
                if (p.is_regular_file()) {
                    auto ext = p.path().extension().string();
                    if (ext == ".cue" || ext == ".CUE")
                        batch_files.push_back(p.path().string());
                }
            }
        } catch (const std::filesystem::filesystem_error&) {
            std::cerr << "ERROR: cannot access directory: " << dir << "\n";
            return 1;
        }

        if (batch_files.empty()) {
            std::cerr << "ERROR: no .cue files found in " << dir << "\n";
            return 1;
        }

        std::cout << "Found " << batch_files.size() << " .cue file(s)\n";

        int overall_rc = 0;
        for (size_t i = 0; i < batch_files.size(); i++) {
            if (g_gui_interrupted.load()) break;

            std::cout << "\n[" << (i + 1) << "/" << batch_files.size() << "] "
                      << std::filesystem::path(batch_files[i]).filename().string() << "\n";

            cue_sheet parsed_cue;
            if (cue_parse(batch_files[i], parsed_cue) != 0) {
                std::cerr << "ERROR: Failed to parse CUE\n";
                std::cerr << "  FAILED\n";
                overall_rc = 1;
                continue;
            }

            std::string cue_dir = get_cue_dir(batch_files[i]);
            temp_file temp_concat;

            std::string bin_path;
            if (parsed_cue.file_order.size() > 1) {
                if (concat_split_bins(parsed_cue, cue_dir, temp_concat) != 0) {
                    std::cerr << "ERROR: Failed to concatenate split bins\n";
                    std::cerr << "  FAILED\n";
                    overall_rc = 1;
                    continue;
                }
                bin_path = temp_concat.path();
            } else {
                bin_path = (std::filesystem::path(cue_dir) / parsed_cue.file_order[0]).string();
            }

            std::filesystem::path cueP(batch_files[i]);
            std::string outDir = m_batchEncodeOutputDir->GetPath().ToStdString();
            std::string outPath;
            if (!outDir.empty()) {
                outPath = (std::filesystem::path(outDir) / (cueP.stem().string() + ".ecm3")).string();
            } else {
                outPath = (cueP.parent_path() / (cueP.stem().string() + ".ecm3")).string();
            }

            opts.in_filename = bin_path;
            opts.cue_filename = batch_files[i];
            opts.out_filename = outPath;
            opts.force_rewrite = m_batchEncodeForce->GetValue();
            opts.delete_paths.clear();

            ecm3_result result;
            int rc = ecm3_encode(opts.in_filename, opts.out_filename, opts, &parsed_cue, true, result);

            if (rc == 0) {
                if (opts.delete_source) {
                    for (const auto& path : result.source_paths) {
                        std::error_code ec;
                        std::filesystem::remove(path, ec);
                    }
                }
                std::cout << "  OK\n";
            } else {
                std::cerr << "  FAILED\n";
                overall_rc = rc;
            }
        }
        return overall_rc;
    }

    int RunBatchDecode() {
        std::string dir = m_batchDecodeDir->GetPath().ToStdString();
        if (dir.empty()) {
            std::cerr << "ERROR: No directory specified.\n";
            return 1;
        }

        ecm_options opts;
        opts.delete_source = m_batchDecodeDeleteSource->GetValue();
        opts.batch_decode_mode = true;
        opts.batch_directory = dir;

        std::vector<std::string> batch_files;
        try {
            for (auto& p : std::filesystem::recursive_directory_iterator(dir)) {
                if (p.is_regular_file()) {
                    auto ext = p.path().extension().string();
                    if (ext == ".ecm3" || ext == ".ECM3")
                        batch_files.push_back(p.path().string());
                }
            }
        } catch (const std::filesystem::filesystem_error&) {
            std::cerr << "ERROR: cannot access directory: " << dir << "\n";
            return 1;
        }

        if (batch_files.empty()) {
            std::cerr << "ERROR: no .ecm3 files found in " << dir << "\n";
            return 1;
        }

        std::cout << "Found " << batch_files.size() << " .ecm3 file(s)\n";

        int overall_rc = 0;
        for (size_t i = 0; i < batch_files.size(); i++) {
            if (g_gui_interrupted.load()) break;

            std::cout << "\n[" << (i + 1) << "/" << batch_files.size() << "] "
                      << std::filesystem::path(batch_files[i]).filename().string() << "\n";

            std::string in = batch_files[i];
            if (in.size() >= 5 && in.substr(in.size() - 5) == ".ecm3") {
                in.resize(in.size() - 5);
            }
            std::string outDir = m_batchDecodeOutputDir->GetPath().ToStdString();
            std::string outPath;
            if (!outDir.empty()) {
                std::string stem = std::filesystem::path(in).filename().string();
                outPath = (std::filesystem::path(outDir) / (stem + ".bin")).string();
            } else {
                outPath = in + ".bin";
            }

            opts.in_filename = batch_files[i];
            opts.out_filename = outPath;
            opts.force_rewrite = m_batchDecodeForce->GetValue();
            opts.split_output = m_batchDecodeSplit->GetValue();
            opts.verify = m_batchDecodeVerify->GetValue();
            opts.delete_paths.clear();

            if (!opts.verify && !opts.force_rewrite && std::filesystem::exists(opts.out_filename)) {
                std::cerr << "ERROR: output file already exists: " << opts.out_filename
                           << ". Check 'Force overwrite' to overwrite.\n";
                std::cerr << "  FAILED\n";
                overall_rc = 1;
                continue;
            }

            ecm3_result result;
            int rc = ecm3_decode(opts.in_filename, opts.out_filename, opts, result);

            if (rc == 0) {
                if (opts.verify) {
                    std::cout << "  VERIFY: Would write " << opts.out_filename << "\n";
                    if (result.has_metadata) {
                        std::filesystem::path cue_path(opts.out_filename);
                        cue_path.replace_extension(".cue");
                        std::cout << "  VERIFY: Would write " << cue_path.string() << "\n";
                        if (opts.split_output && result.meta_entries.size() > 1) {
                            std::cout << "  VERIFY: Would write " << result.meta_entries.size()
                                      << " track file(s)\n";
                        }
                    }
                } else {
                    if (result.has_metadata) {
                        write_cue_from_metadata(result.meta_header, result.meta_entries,
                                                opts.out_filename, opts.split_output);
                    }

                    if (result.has_metadata && result.meta_entries.size() > 1) {
                        if (split_output_bin(opts.out_filename, result.meta_header, result.meta_entries,
                                             result.meta_header.total_sectors, opts.force_rewrite) == 0) {
                            std::error_code ec;
                            std::filesystem::remove(opts.out_filename, ec);
                        }
                    }

                    if (opts.delete_source) {
                        for (const auto& path : result.source_paths) {
                            std::error_code ec;
                            std::filesystem::remove(path, ec);
                        }
                    }
                }
                std::cout << "  OK\n";
            } else {
                std::cerr << "  FAILED\n";
                overall_rc = rc;
            }
        }
        return overall_rc;
    }

    // ── Tab builders ──
    void CreateEncodeTab() {
        m_encodePanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, "Input file (.bin or .cue):"));
        m_encodeInput = new wxFilePickerCtrl(m_encodePanel, wxID_ANY, "", "Select input file",
                                              "BIN/CUE files (*.bin;*.cue)|*.bin;*.cue|All files (*.*)|*.*",
                                              wxDefaultPosition, wxDefaultSize,
                                              wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        m_encodeInput->SetDropTarget(new FileDropTarget(m_encodeInput));
        gb->Add(m_encodeInput, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, "CUE sheet (optional):"));
        m_encodeCue = new wxFilePickerCtrl(m_encodePanel, wxID_ANY, "", "Select CUE sheet",
                                             "CUE files (*.cue)|*.cue|All files (*.*)|*.*",
                                             wxDefaultPosition, wxDefaultSize,
                                             wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        gb->Add(m_encodeCue, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, "Output .ecm3 (optional):"));
        m_encodeOutput = new wxFilePickerCtrl(m_encodePanel, wxID_ANY, "", "Save to...",
                                               "ECM3 files (*.ecm3)|*.ecm3|All files (*.*)|*.*",
                                               wxDefaultPosition, wxDefaultSize,
                                               wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT | wxFLP_USE_TEXTCTRL);
        gb->Add(m_encodeOutput, 1, wxEXPAND);

        const wxArrayString audioCompChoices = {"none", "zlib", "lzma", "lz4", "lzma2", "flac", "zstd", "wavpack"};
        const wxArrayString dataCompChoices = {"none", "zlib", "lzma", "lz4", "lzma2", "zstd"};

        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, "Audio compression:"));
        m_encodeAudioComp = new wxChoice(m_encodePanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, audioCompChoices);
        m_encodeAudioComp->SetStringSelection("flac");
        gb->Add(m_encodeAudioComp, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, "Data compression:"));
        m_encodeDataComp = new wxChoice(m_encodePanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, dataCompChoices);
        m_encodeDataComp->SetStringSelection("lzma2");
        gb->Add(m_encodeDataComp, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, "Compression level (0-9):"));
        m_encodeLevel = new wxSpinCtrl(m_encodePanel, wxID_ANY, "9", wxDefaultPosition, wxDefaultSize,
                                        wxSP_ARROW_KEYS, 0, 9, 9);
        gb->Add(m_encodeLevel, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, ""));
        auto* cbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_encodeExtreme = new wxCheckBox(m_encodePanel, wxID_ANY, "Extreme");
        m_encodeExtreme->SetValue(true);
        m_encodeSeekable = new wxCheckBox(m_encodePanel, wxID_ANY, "Seekable");
        m_encodeForce = new wxCheckBox(m_encodePanel, wxID_ANY, "Force overwrite");
        m_encodeDeleteSource = new wxCheckBox(m_encodePanel, wxID_ANY, "Delete Source");
        cbSizer->Add(m_encodeExtreme, 0, wxRIGHT, 10);
        cbSizer->Add(m_encodeSeekable, 0, wxRIGHT, 10);
        cbSizer->Add(m_encodeForce, 0, wxRIGHT, 10);
        cbSizer->Add(m_encodeDeleteSource);
        gb->Add(cbSizer);

        m_encodeSpbLabel = new wxStaticText(m_encodePanel, wxID_ANY, "Sectors per block (1-255):");
        m_encodeSectorsPerBlock = new wxSpinCtrl(m_encodePanel, wxID_ANY, "100", wxDefaultPosition, wxDefaultSize,
                                                   wxSP_ARROW_KEYS, 1, 255, 100);
        gb->Add(m_encodeSpbLabel);
        gb->Add(m_encodeSectorsPerBlock, 0, wxEXPAND);
        m_encodeSpbLabel->Hide();
        m_encodeSectorsPerBlock->Hide();

        m_encodeSeekable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            if (m_encodeSeekable->GetValue()) {
                m_encodeSpbLabel->Show();
                m_encodeSectorsPerBlock->Show();
            } else {
                m_encodeSpbLabel->Hide();
                m_encodeSectorsPerBlock->Hide();
            }
            m_encodePanel->Layout();
            m_encodePanel->Fit();
        });

        auto* m_encodeHint = new wxStaticText(m_encodePanel, wxID_ANY,
            "Recommended: FLAC Audio, LZMA2 Data, Level=9, Extreme ON");
        m_encodeHint->SetForegroundColour(wxColour(100, 100, 100));
        gb->Add(new wxStaticText(m_encodePanel, wxID_ANY, ""));
        gb->Add(m_encodeHint, 0, wxTOP, -4);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_encodePanel->SetSizer(s);
        m_notebook->AddPage(m_encodePanel, "Encode");
    }

    void CreateDecodeTab() {
        m_decodePanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        const wxString ecm3Filter = "ECM3 files (*.ecm3)|*.ecm3|All files (*.*)|*.*";

        gb->Add(new wxStaticText(m_decodePanel, wxID_ANY, "Input file (.ecm3):"));
        m_decodeInput = new wxFilePickerCtrl(m_decodePanel, wxID_ANY, "", "Select ECM3 file",
                                              ecm3Filter, wxDefaultPosition, wxDefaultSize,
                                              wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        m_decodeInput->SetDropTarget(new FileDropTarget(m_decodeInput));
        gb->Add(m_decodeInput, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_decodePanel, wxID_ANY, "Output .bin (optional):"));
        m_decodeOutput = new wxFilePickerCtrl(m_decodePanel, wxID_ANY, "", "Save to...",
                                               "BIN files (*.bin)|*.bin|All files (*.*)|*.*",
                                               wxDefaultPosition, wxDefaultSize,
                                               wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT | wxFLP_USE_TEXTCTRL);
        gb->Add(m_decodeOutput, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_decodePanel, wxID_ANY, ""));
        auto* cbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_decodeSplit = new wxCheckBox(m_decodePanel, wxID_ANY, "Split tracks");
        m_decodeSplit->SetValue(true);
        m_decodeForce = new wxCheckBox(m_decodePanel, wxID_ANY, "Force overwrite");
        m_decodeVerify = new wxCheckBox(m_decodePanel, wxID_ANY, "Verify");
        m_decodeDeleteSource = new wxCheckBox(m_decodePanel, wxID_ANY, "Delete Source");
        cbSizer->Add(m_decodeSplit, 0, wxRIGHT, 10);
        cbSizer->Add(m_decodeForce, 0, wxRIGHT, 10);
        cbSizer->Add(m_decodeVerify, 0, wxRIGHT, 10);
        cbSizer->Add(m_decodeDeleteSource);
        gb->Add(cbSizer);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_decodePanel->SetSizer(s);
        m_notebook->AddPage(m_decodePanel, "Decode");
    }

    void CreateBatchEncodeTab() {
        m_batchEncodePanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        const wxArrayString audioCompChoices = {"none", "zlib", "lzma", "lz4", "lzma2", "flac", "zstd", "wavpack"};
        const wxArrayString dataCompChoices = {"none", "zlib", "lzma", "lz4", "lzma2", "zstd"};

        gb->Add(new wxStaticText(m_batchEncodePanel, wxID_ANY, "Directory (recurses for .cue files):"));
        m_batchEncodeDir = new wxDirPickerCtrl(m_batchEncodePanel, wxID_ANY, "", "Select directory",
                                                 wxDefaultPosition, wxDefaultSize,
                                                 wxDIRP_USE_TEXTCTRL);
        gb->Add(m_batchEncodeDir, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_batchEncodePanel, wxID_ANY, "Output directory (optional):"));
        m_batchEncodeOutputDir = new wxDirPickerCtrl(m_batchEncodePanel, wxID_ANY, "", "Select output directory",
                                                      wxDefaultPosition, wxDefaultSize,
                                                      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        gb->Add(m_batchEncodeOutputDir, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_batchEncodePanel, wxID_ANY, "Audio compression:"));
        m_batchEncodeAudioComp = new wxChoice(m_batchEncodePanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, audioCompChoices);
        m_batchEncodeAudioComp->SetStringSelection("flac");
        gb->Add(m_batchEncodeAudioComp, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_batchEncodePanel, wxID_ANY, "Data compression:"));
        m_batchEncodeDataComp = new wxChoice(m_batchEncodePanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, dataCompChoices);
        m_batchEncodeDataComp->SetStringSelection("lzma2");
        gb->Add(m_batchEncodeDataComp, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_batchEncodePanel, wxID_ANY, "Compression level (0-9):"));
        m_batchEncodeLevel = new wxSpinCtrl(m_batchEncodePanel, wxID_ANY, "9", wxDefaultPosition, wxDefaultSize,
                                             wxSP_ARROW_KEYS, 0, 9, 9);
        gb->Add(m_batchEncodeLevel, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_batchEncodePanel, wxID_ANY, ""));
        auto* beCbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_batchEncodeExtreme = new wxCheckBox(m_batchEncodePanel, wxID_ANY, "Extreme");
        m_batchEncodeExtreme->SetValue(true);
        m_batchEncodeSeekable = new wxCheckBox(m_batchEncodePanel, wxID_ANY, "Seekable");
        m_batchEncodeForce = new wxCheckBox(m_batchEncodePanel, wxID_ANY, "Force overwrite");
        m_batchEncodeDeleteSource = new wxCheckBox(m_batchEncodePanel, wxID_ANY, "Delete Source");
        beCbSizer->Add(m_batchEncodeExtreme, 0, wxRIGHT, 10);
        beCbSizer->Add(m_batchEncodeSeekable, 0, wxRIGHT, 10);
        beCbSizer->Add(m_batchEncodeForce, 0, wxRIGHT, 10);
        beCbSizer->Add(m_batchEncodeDeleteSource);
        gb->Add(beCbSizer);

        m_batchEncodeSpbLabel = new wxStaticText(m_batchEncodePanel, wxID_ANY, "Sectors per block (1-255):");
        m_batchEncodeSectorsPerBlock = new wxSpinCtrl(m_batchEncodePanel, wxID_ANY, "100", wxDefaultPosition, wxDefaultSize,
                                                        wxSP_ARROW_KEYS, 1, 255, 100);
        gb->Add(m_batchEncodeSpbLabel);
        gb->Add(m_batchEncodeSectorsPerBlock, 0, wxEXPAND);
        m_batchEncodeSpbLabel->Hide();
        m_batchEncodeSectorsPerBlock->Hide();

        m_batchEncodeSeekable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            if (m_batchEncodeSeekable->GetValue()) {
                m_batchEncodeSpbLabel->Show();
                m_batchEncodeSectorsPerBlock->Show();
            } else {
                m_batchEncodeSpbLabel->Hide();
                m_batchEncodeSectorsPerBlock->Hide();
            }
            m_batchEncodePanel->Layout();
            m_batchEncodePanel->Fit();
        });

        auto* m_batchEncodeHint = new wxStaticText(m_batchEncodePanel, wxID_ANY,
            "Recommended: FLAC Audio, LZMA2 Data, Level=9, Extreme ON");
        m_batchEncodeHint->SetForegroundColour(wxColour(100, 100, 100));
        gb->Add(new wxStaticText(m_batchEncodePanel, wxID_ANY, ""));
        gb->Add(m_batchEncodeHint, 0, wxTOP, -4);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_batchEncodePanel->SetSizer(s);
        m_notebook->AddPage(m_batchEncodePanel, "Batch Encode");
    }

    void CreateBatchDecodeTab() {
        m_batchDecodePanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        gb->Add(new wxStaticText(m_batchDecodePanel, wxID_ANY, "Directory (recurses for .ecm3 files):"));
        m_batchDecodeDir = new wxDirPickerCtrl(m_batchDecodePanel, wxID_ANY, "", "Select directory",
                                                 wxDefaultPosition, wxDefaultSize,
                                                 wxDIRP_USE_TEXTCTRL);
        gb->Add(m_batchDecodeDir, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_batchDecodePanel, wxID_ANY, "Output directory (optional):"));
        m_batchDecodeOutputDir = new wxDirPickerCtrl(m_batchDecodePanel, wxID_ANY, "", "Select output directory",
                                                      wxDefaultPosition, wxDefaultSize,
                                                      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        gb->Add(m_batchDecodeOutputDir, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_batchDecodePanel, wxID_ANY, ""));
        auto* bdCbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_batchDecodeSplit = new wxCheckBox(m_batchDecodePanel, wxID_ANY, "Split tracks");
        m_batchDecodeSplit->SetValue(true);
        m_batchDecodeForce = new wxCheckBox(m_batchDecodePanel, wxID_ANY, "Force overwrite");
        m_batchDecodeVerify = new wxCheckBox(m_batchDecodePanel, wxID_ANY, "Verify");
        m_batchDecodeDeleteSource = new wxCheckBox(m_batchDecodePanel, wxID_ANY, "Delete Source");
        bdCbSizer->Add(m_batchDecodeSplit, 0, wxRIGHT, 10);
        bdCbSizer->Add(m_batchDecodeForce, 0, wxRIGHT, 10);
        bdCbSizer->Add(m_batchDecodeVerify, 0, wxRIGHT, 10);
        bdCbSizer->Add(m_batchDecodeDeleteSource);
        gb->Add(bdCbSizer);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_batchDecodePanel->SetSizer(s);
        m_notebook->AddPage(m_batchDecodePanel, "Batch Decode");
    }

    void CreateSettingsTab() {
        m_settingsPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);

        auto* assocSizer = new wxBoxSizer(wxHORIZONTAL);
        m_settingsAssoc = new wxCheckBox(m_settingsPanel, wxID_ANY,
            "Associate .ecm3 files with ecm3 GUI");
        assocSizer->Add(m_settingsAssoc);
        s->Add(assocSizer, 0, wxALL, 10);

        m_settingsStatus = new wxStaticText(m_settingsPanel, wxID_ANY, "",
            wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
        s->Add(m_settingsStatus, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        s->AddStretchSpacer();
        m_settingsPanel->SetSizer(s);
        m_notebook->AddPage(m_settingsPanel, "Settings");

        m_settingsAssoc->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
#if defined(_WIN32)
            wxString exePath = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetFullPath();
            if (m_settingsAssoc->GetValue()) {
                HKEY hkRoot = HKEY_CURRENT_USER;
                HKEY hkClasses;
                if (RegCreateKeyExW(hkRoot, L"Software\\Classes", 0, nullptr,
                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkClasses, nullptr) == ERROR_SUCCESS) {
                    HKEY hkExt;
                    if (RegCreateKeyExW(hkClasses, L".ecm3", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkExt, nullptr) == ERROR_SUCCESS) {
                        LPCWSTR val = L"ecm3file";
                        RegSetValueExW(hkExt, nullptr, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(val), (wcslen(val) + 1) * sizeof(wchar_t));
                        RegCloseKey(hkExt);
                    }
                    HKEY hkProg;
                    if (RegCreateKeyExW(hkClasses, L"ecm3file", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkProg, nullptr) == ERROR_SUCCESS) {
                        LPCWSTR desc = L"ECM3 Disc Image";
                        RegSetValueExW(hkProg, nullptr, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(desc), (wcslen(desc) + 1) * sizeof(wchar_t));
                        HKEY hkIcon;
                        if (RegCreateKeyExW(hkProg, L"DefaultIcon", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkIcon, nullptr) == ERROR_SUCCESS) {
                            wxString iconPath = exePath + L",2";
                            RegSetValueExW(hkIcon, nullptr, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(iconPath.wc_str()),
                                (iconPath.length() + 1) * sizeof(wchar_t));
                            RegCloseKey(hkIcon);
                        }
                        HKEY hkCmd;
                        if (RegCreateKeyExW(hkProg, L"shell\\open\\command", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkCmd, nullptr) == ERROR_SUCCESS) {
                            wxString cmd = L"\"" + exePath + L"\" \"%1\"";
                            RegSetValueExW(hkCmd, nullptr, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(cmd.wc_str()),
                                (cmd.length() + 1) * sizeof(wchar_t));
                            RegCloseKey(hkCmd);
                        }
                        RegCloseKey(hkProg);
                    }
                    RegCloseKey(hkClasses);
                }
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                m_settingsStatus->SetLabel(".ecm3 association registered for current user.");
            } else {
                HKEY hkClasses;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, KEY_WRITE, &hkClasses) == ERROR_SUCCESS) {
                    RegDeleteTreeW(hkClasses, L"ecm3file");
                    RegDeleteTreeW(hkClasses, L".ecm3");
                    RegCloseKey(hkClasses);
                }
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                m_settingsStatus->SetLabel(".ecm3 association removed.");
            }
            wxMessageBox("File association updated.\n\n"
                "Changes apply to the current user only.\n"
                "You may need to restart Explorer for the icon to update.",
                "Settings", wxOK | wxICON_INFORMATION, m_settingsPanel);
#else
            m_settingsStatus->SetLabel("File association not supported on this platform.");
#endif
        });

        bool alreadyAssociated = false;
#if defined(_WIN32)
        {
            HKEY hkExt;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.ecm3",
                0, KEY_READ, &hkExt) == ERROR_SUCCESS) {
                alreadyAssociated = true;
                RegCloseKey(hkExt);
            }
        }
#endif
        m_settingsAssoc->SetValue(alreadyAssociated);
    }

    void CreateAboutTab() {
        m_aboutPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* txt = new wxStaticText(m_aboutPanel, wxID_ANY,
            wxString::Format("ecm3 v%s\n\nEnhanced ECM (Error Code Modeler) for CD-ROM images\n"
                             "Copyright (C) 2026 Edward Sloter\n\n"
                             "Based on the original ECM by Neill Corlett and\n"
                             "the ecmtool project by Daniel Carrasco.\n\n"
                             "This program is free software under the GNU AGPL v3.", VERSI),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        wxFont f = txt->GetFont();
        f.SetPointSize(f.GetPointSize() + 2);
        txt->SetFont(f);
        s->AddStretchSpacer();
        s->Add(txt, 0, wxALIGN_CENTER | wxALL, 20);
        s->AddStretchSpacer();
        m_aboutPanel->SetSizer(s);
        m_notebook->AddPage(m_aboutPanel, "About");
    }

    void CreateCueSplitTab() {
        m_cueSplitPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);

        // Mode radio box (Split / Combine)
        wxString modeChoices[] = {
            wxString::FromUTF8("Split (combined \xe2\x96\xb6 per-track)"),
            wxString::FromUTF8("Combine (per-track \xe2\x96\xb6 combined)  ")
        };
        m_cueSplitMode = new wxRadioBox(m_cueSplitPanel, wxID_ANY, "Mode",
            wxDefaultPosition, wxDefaultSize, 2, modeChoices, 1, wxRA_SPECIFY_ROWS);
        m_cueSplitMode->SetSelection(0);
        s->Add(m_cueSplitMode, 0, wxALL, 10);

        // Input .cue file
        auto* inSizer = new wxBoxSizer(wxHORIZONTAL);
        inSizer->Add(new wxStaticText(m_cueSplitPanel, wxID_ANY, "CUE file:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        m_cueSplitInput = new wxFilePickerCtrl(m_cueSplitPanel, wxID_ANY, wxEmptyString,
            "Select CUE file", "CUE files (*.cue)|*.cue|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize, wxFLP_OPEN | wxFLP_FILE_MUST_EXIST);
        inSizer->Add(m_cueSplitInput, 1, wxEXPAND);
        s->Add(inSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        // Output directory
        auto* outSizer = new wxBoxSizer(wxHORIZONTAL);
        outSizer->Add(new wxStaticText(m_cueSplitPanel, wxID_ANY, "Output dir:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        m_cueSplitOutput = new wxDirPickerCtrl(m_cueSplitPanel, wxID_ANY, wxEmptyString,
            "Select output directory", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        outSizer->Add(m_cueSplitOutput, 1, wxEXPAND);
        s->Add(outSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        // Options
        auto* cbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_cueSplitForce = new wxCheckBox(m_cueSplitPanel, wxID_ANY, "Force overwrite");
        cbSizer->Add(m_cueSplitForce);
        s->Add(cbSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        s->AddStretchSpacer();
        m_cueSplitPanel->SetSizer(s);
        m_notebook->AddPage(m_cueSplitPanel, "Split/Combine CUE");

        m_cueSplitInput->SetDropTarget(new FileDropTarget(m_cueSplitInput));
    }

    int RunCueSplit() {
        std::string cuePath = m_cueSplitInput->GetPath().ToStdString();
        if (cuePath.empty()) {
            std::cerr << "ERROR: No CUE file selected.\n";
            return 1;
        }
        if (!std::filesystem::exists(cuePath)) {
            std::cerr << "ERROR: File not found: " << cuePath << "\n";
            return 1;
        }
        std::string outDir = m_cueSplitOutput->GetPath().ToStdString();
        if (outDir.empty()) {
            std::cerr << "ERROR: No output directory selected.\n";
            return 1;
        }
        bool force = m_cueSplitForce->GetValue();
        bool doSplit = (m_cueSplitMode->GetSelection() == 0);
        if (doSplit) {
            return cue_cmd_split(cuePath, outDir, force);
        } else {
            return cue_cmd_combine(cuePath, outDir, force);
        }
    }
};

// ── App ──
class Ecm3App : public wxApp {
public:
    bool OnInit() override {
        auto* frame = new Ecm3Frame();
        frame->Show(true);
        if (argc > 1) {
            wxString arg = argv[1];
            if (arg.Lower().EndsWith(".ecm3")) {
                frame->m_decodeInput->SetPath(arg);
                frame->m_notebook->SetSelection(1);
            } else if (arg.Lower().EndsWith(".cue") || arg.Lower().EndsWith(".bin")) {
                frame->m_encodeInput->SetPath(arg);
                frame->m_notebook->SetSelection(0);
            }
        }
        return true;
    }
};

wxIMPLEMENT_APP(Ecm3App);