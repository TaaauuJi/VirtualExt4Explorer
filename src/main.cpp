/*
 * VirtualExt4Explorer
 * Copyright (c) 2026 Taaauu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <algorithm>
#include <commdlg.h>
#include <shlobj.h>
#include <set>
#include <shellapi.h>

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_win32.h"
#include "../imgui/backends/imgui_impl_dx11.h"
#include "../imgui/misc/cpp/imgui_stdlib.h"
#include "VHDManager.h"

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

struct ExplorerApp;
static ExplorerApp*             g_pAppInstance = nullptr;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper to open file dialog
std::string OpenFileDialog() {
    char szFile[260] = { 0 };
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "All Supported (*.vhd, *.vhdx, *.vdi)\0*.vhd;*.vhdx;*.vdi\0VHD Files (*.vhd)\0*.vhd\0VHDX Files (*.vhdx)\0*.vhdx\0VDI Files (*.vdi)\0*.vdi\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return szFile;
    return "";
}

std::string SaveFileDialog(const std::string& default_name) {
    char szFile[260] = { 0 };
    strncpy(szFile, default_name.c_str(), sizeof(szFile));
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return szFile;
    return "";
}

// Helper to pick a folder
std::string PickFolder() {
    BROWSEINFOA bi = { 0 };
    bi.lpszTitle = "Select Destination Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != 0) {
        char path[MAX_PATH];
        if (SHGetPathFromIDListA(pidl, path)) {
            CoTaskMemFree(pidl);
            return path;
        }
        CoTaskMemFree(pidl);
    }
    return "";
}

struct ClipboardItem {
    std::string path;
    bool is_cut;
};

struct ExplorerApp {
    VHDManager vhd;
    std::string current_path = "/";
    std::vector<FileInfo> files;
    bool needs_refresh = false;
    std::string error_msg;

    // Selection
    std::set<std::string> selected_items;
    int last_clicked_index = -1;

    // Navigation
    std::vector<std::string> history_back;
    std::vector<std::string> history_forward;

    // Clipboard
    std::vector<ClipboardItem> clipboard;

    // Popups
    std::string rename_old;
    std::string rename_new;
    bool show_rename_popup = false;
    bool show_newfile_popup = false;
    bool show_permissions_popup = false;
    bool show_delete_popup = false;
    bool perm_recurse = false;
    FileInfo perm_target;
    char perm_mode[16];
    char perm_uid[16];
    char perm_gid[16];

    void OpenAndMountImage(const std::string& path) {
        if (vhd.OpenVHD(path)) {
            const auto& parts = vhd.GetPartitions();

            // Prefer the largest ext4 partition (the data partition), then fall
            // back to any other ext4 partition that mounts. The first ext4
            // partition is often a tiny one containing only lost+found.
            std::vector<int> order;
            for (int i = 0; i < (int)parts.size(); i++)
                if (parts[i].is_ext4) order.push_back(i);
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return parts[a].size > parts[b].size;
            });

            bool mounted = false;
            for (int idx : order) {
                if (vhd.MountExt4Partition(idx)) { mounted = true; break; }
            }

            if (mounted) {
                current_path = "/";
                history_back.clear();
                history_forward.clear();
                needs_refresh = true;
                error_msg = "";
            } else {
                error_msg = "No mountable ext4 partition found in this image";
                if (!vhd.GetLastError().empty())
                    error_msg += " (" + vhd.GetLastError() + ")";
            }
        } else {
            error_msg = vhd.GetLastError();
        }
    }

    void HandleDroppedFiles(const std::vector<std::string>& paths) {
        if (paths.empty()) return;

        bool is_single_vhd = false;
        if (paths.size() == 1) {
            size_t dot = paths[0].find_last_of('.');
            if (dot != std::string::npos) {
                std::string ext = paths[0].substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == "vhd" || ext == "vhdx" || ext == "vdi") {
                    is_single_vhd = true;
                }
            }
        }

        if (!vhd.IsExt4Mounted() || is_single_vhd) {
            std::string vhd_to_mount = "";
            for (const auto& path : paths) {
                size_t dot = path.find_last_of('.');
                if (dot != std::string::npos) {
                    std::string ext = path.substr(dot + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == "vhd" || ext == "vhdx" || ext == "vdi") {
                        vhd_to_mount = path;
                        break;
                    }
                }
            }

            if (!vhd_to_mount.empty()) {
                if (vhd.IsExt4Mounted()) {
                    vhd.UnmountExt4();
                    files.clear();
                }
                OpenAndMountImage(vhd_to_mount);
                return;
            }

            if (!vhd.IsExt4Mounted()) {
                error_msg = "Please mount a VHD image first to import files/folders, or drop a VHD file to mount it.";
                return;
            }
        }

        // If we get here, it means we are mounted, and the dropped items are files/folders to be imported
        for (const auto& path : paths) {
            DWORD dwAttrs = GetFileAttributesA(path.c_str());
            if (dwAttrs == INVALID_FILE_ATTRIBUTES) continue;

            size_t s = path.find_last_of("\\/");
            std::string name = (s == std::string::npos) ? path : path.substr(s + 1);
            std::string dest = GetFullPath(name);

            bool is_dir = (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (is_dir) {
                if (!vhd.ImportRecursive(path, dest)) {
                    error_msg = "Failed to import folder: " + vhd.GetLastError();
                }
            } else {
                if (!vhd.CopyFileFromHost(path, dest)) {
                    error_msg = "Failed to import file: " + vhd.GetLastError();
                }
            }
        }
        needs_refresh = true;
    }

    void Refresh() {
        if (vhd.IsExt4Mounted()) {
            files.clear();
            if (!vhd.ListDirectoryInfo(current_path, files)) {
                error_msg = vhd.GetLastError();
            } else {
                if (error_msg.find("Failed to") == std::string::npos && 
                    error_msg.find("Please mount") == std::string::npos && 
                    error_msg.find("No ext4 partition") == std::string::npos) {
                    error_msg = "";
                }
                std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
                    if (a.is_dir != b.is_dir) return a.is_dir;
                    return a.name < b.name;
                });
            }
        }
        selected_items.clear();
        last_clicked_index = -1;
        needs_refresh = false;
    }

    void Navigate(const std::string& path, bool record_history = true) {
        std::string target = path;
        if (path == "..") {
            if (current_path == "/") return;
            size_t pos = current_path.find_last_of('/', current_path.length() - 2);
            target = (pos == std::string::npos) ? "/" : current_path.substr(0, pos + 1);
        }
        if (target == current_path) return;
        if (record_history) {
            history_back.push_back(current_path);
            history_forward.clear();
        }
        current_path = target;
        needs_refresh = true;
        error_msg = "";
    }

    void GoBack() { if (!history_back.empty()) { history_forward.push_back(current_path); current_path = history_back.back(); history_back.pop_back(); needs_refresh = true; error_msg = ""; } }
    void GoForward() { if (!history_forward.empty()) { history_back.push_back(current_path); current_path = history_forward.back(); history_forward.pop_back(); needs_refresh = true; error_msg = ""; } }

    std::string GetFullPath(const std::string& name) {
        std::string p = current_path;
        if (p.back() != '/') p += "/";
        return p + name;
    }

    void DoPaste() {
        for (const auto& item : clipboard) {
            size_t last_slash = item.path.find_last_of('/');
            std::string name = (last_slash == std::string::npos) ? item.path : item.path.substr(last_slash + 1);
            std::string dest = GetFullPath(name);
            if (item.is_cut) {
                vhd.Rename(item.path, dest);
            } else {
                vhd.CopyInternal(item.path, dest);
            }
        }
        if (!clipboard.empty() && clipboard[0].is_cut) clipboard.clear();
        needs_refresh = true;
    }

    ImVec4 GetFileColor(const FileInfo& file) {
        if (file.is_dir) return ImVec4(1.0f, 0.84f, 0.0f, 1.0f);
        size_t dot = file.name.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = file.name.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == "exe" || ext == "bat" || ext == "sh") return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
            if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "apk") return ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
            if (ext == "so" || ext == "dll") return ImVec4(1.0f, 0.4f, 0.7f, 1.0f);
        }
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    void Draw() {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Virtual Ext4 Explorer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Mount")) {
                if (ImGui::MenuItem("Open Image...")) {
                    std::string p = OpenFileDialog();
                    if (!p.empty()) {
                        OpenAndMountImage(p);
                    }
                }
                if (ImGui::BeginMenu("Partitions", vhd.IsOpen())) {
                    for (int i=0; i<(int)vhd.GetPartitions().size(); i++) {
                        char l[64]; snprintf(l,64,"Part %d (%s)", i, vhd.GetPartitions()[i].is_ext4?"ext4":"other");
                        if (ImGui::MenuItem(l,NULL,false,vhd.GetPartitions()[i].is_ext4)) { vhd.MountExt4Partition(i); current_path="/"; needs_refresh=true; error_msg = ""; }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Unmount", NULL, false, vhd.IsExt4Mounted())) { vhd.UnmountExt4(); files.clear(); error_msg = ""; }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Toolbar
        if (ImGui::Button("<")) GoBack(); ImGui::SameLine();
        if (ImGui::Button(">")) GoForward(); ImGui::SameLine();
        if (ImGui::Button("Up")) Navigate(".."); ImGui::SameLine();
        ImGui::Spacing(); ImGui::SameLine();

        if (ImGui::Button("Import")) ImGui::OpenPopup("ImportMenu");
        if (ImGui::BeginPopup("ImportMenu")) {
            if (ImGui::MenuItem("Files...")) {
                std::string h = OpenFileDialog();
                if (!h.empty()) {
                    size_t s = h.find_last_of("\\/");
                    std::string n = (s == std::string::npos) ? h : h.substr(s + 1);
                    error_msg = "";
                    if (vhd.CopyFileFromHost(h, GetFullPath(n))) {
                        needs_refresh = true;
                    } else {
                        error_msg = "Failed to import file: " + vhd.GetLastError();
                    }
                }
            }
            if (ImGui::MenuItem("Folder...")) {
                std::string h = PickFolder();
                if (!h.empty()) {
                    size_t s = h.find_last_of("\\/");
                    if (s == h.length() - 1) s = h.find_last_of("\\/", s - 1);
                    std::string n = (s == std::string::npos) ? h : h.substr(s + 1);
                    error_msg = "";
                    if (vhd.ImportRecursive(h, GetFullPath(n))) {
                        needs_refresh = true;
                    } else {
                        error_msg = "Failed to import folder: " + vhd.GetLastError();
                    }
                }
            }
            ImGui::EndPopup();
        } ImGui::SameLine();

        if (ImGui::Button("Export") && !selected_items.empty()) {
            if (selected_items.size() == 1) {
                std::string name = *selected_items.begin();
                bool is_dir = false; for(auto& f:files) if(f.name == name) { is_dir = f.is_dir; break; }
                if (is_dir) {
                    std::string folder = PickFolder();
                    if (!folder.empty()) vhd.ExportRecursive(GetFullPath(name), folder + "\\" + name);
                } else {
                    std::string h = SaveFileDialog(name);
                    if (!h.empty()) vhd.CopyFileToHost(GetFullPath(name), h);
                }
            } else {
                std::string folder = PickFolder();
                if (!folder.empty()) {
                    for (auto& name : selected_items) vhd.ExportRecursive(GetFullPath(name), folder + "\\" + name);
                }
            }
        } ImGui::SameLine();

        if (ImGui::Button("New...")) ImGui::OpenPopup("NewMenu");
        if (ImGui::BeginPopup("NewMenu")) {
            if (ImGui::MenuItem("Folder")) { rename_old=""; rename_new="New Folder"; show_rename_popup=true; }
            if (ImGui::MenuItem("File")) { rename_new="new_file.txt"; show_newfile_popup=true; }
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh")) needs_refresh = true;
            ImGui::EndPopup();
        }

        ImGui::PushItemWidth(-1);
        if (ImGui::InputText("##Path", &current_path, ImGuiInputTextFlags_EnterReturnsTrue)) needs_refresh = true;
        ImGui::PopItemWidth();

        if (!error_msg.empty()) ImGui::TextColored(ImVec4(1,0,0,1), "Error: %s", error_msg.c_str());
        if (needs_refresh) Refresh();

        if (ImGui::BeginTable("Files", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();

            for (int i=0; i<(int)files.size(); i++) {
                const auto& file = files[i];
                ImGui::PushID(i);
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                
                bool is_selected = selected_items.count(file.name) > 0;
                ImGui::PushStyleColor(ImGuiCol_Text, GetFileColor(file));
                
                if (ImGui::Selectable(file.name.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(0) && file.is_dir) {
                        Navigate(GetFullPath(file.name));
                    } else {
                        if (ImGui::GetIO().KeyCtrl) {
                            if (is_selected) selected_items.erase(file.name); else selected_items.insert(file.name);
                        } else if (ImGui::GetIO().KeyShift && last_clicked_index != -1) {
                            selected_items.clear();
                            int start = min(i, last_clicked_index), end = max(i, last_clicked_index);
                            for (int j=start; j<=end; j++) selected_items.insert(files[j].name);
                        } else {
                            selected_items.clear(); selected_items.insert(file.name);
                        }
                        last_clicked_index = i;
                    }
                }
                ImGui::PopStyleColor();

                if (ImGui::BeginPopupContextItem()) {
                    if (selected_items.count(file.name) == 0) {
                        selected_items.clear();
                        selected_items.insert(file.name);
                        last_clicked_index = i;
                    }
                    if (ImGui::MenuItem("Cut")) { clipboard.clear(); for(auto& s:selected_items) clipboard.push_back({GetFullPath(s), true}); }
                    if (ImGui::MenuItem("Copy")) { clipboard.clear(); for(auto& s:selected_items) clipboard.push_back({GetFullPath(s), false}); }
                    if (ImGui::MenuItem("Paste", NULL, false, !clipboard.empty())) DoPaste();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) { rename_old=file.name; rename_new=file.name; show_rename_popup=true; }
                    if (ImGui::MenuItem("Permissions")) {
                        perm_target = file;
                        snprintf(perm_mode, 16, "%o", file.mode & 0777);
                        snprintf(perm_uid, 16, "%u", file.uid);
                        snprintf(perm_gid, 16, "%u", file.gid);
                        show_permissions_popup = true;
                    }
                    if (ImGui::MenuItem("Delete")) { show_delete_popup = true; }
                    ImGui::EndPopup();
                }

                ImGui::TableNextColumn();
                if (file.is_dir) ImGui::Text("Folder");
                else {
                    const char* ext = strrchr(file.name.c_str(), '.');
                    ImGui::Text("%s", ext ? ext + 1 : "File");
                }

                ImGui::TableNextColumn();
                if (file.is_dir) ImGui::Text("-");
                else {
                    if (file.size < 1024) ImGui::Text("%llu B", file.size);
                    else if (file.size < 1024 * 1024) ImGui::Text("%.1f KB", file.size / 1024.0f);
                    else if (file.size < 1024 * 1024 * 1024) ImGui::Text("%.1f MB", file.size / (1024.0f * 1024.0f));
                    else ImGui::Text("%.1f GB", file.size / (1024.0f * 1024.0f * 1024.0f));
                }

                ImGui::PopID();
            }
            
            if (ImGui::BeginPopupContextWindow("EmptySpace", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
                if (ImGui::BeginMenu("New")) {
                    if (ImGui::MenuItem("Folder")) { rename_old=""; rename_new="New Folder"; show_rename_popup=true; }
                    if (ImGui::MenuItem("File")) { rename_new="new_file.txt"; show_newfile_popup=true; }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Paste", NULL, false, !clipboard.empty())) DoPaste();
                ImGui::Separator();
                if (ImGui::MenuItem("Refresh")) needs_refresh=true;
                ImGui::EndPopup();
            }
            ImGui::EndTable();
        }

        if (show_permissions_popup) { ImGui::OpenPopup("Permissions"); show_permissions_popup=false; perm_recurse=false; }
        if (ImGui::BeginPopupModal("Permissions", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (selected_items.size() == 1) ImGui::Text("Item: %s", perm_target.name.c_str());
            else ImGui::Text("Items: %zu selected", selected_items.size());
            
            ImGui::InputText("Mode (octal)", perm_mode, 16);
            ImGui::InputText("UID", perm_uid, 16);
            ImGui::InputText("GID", perm_gid, 16);
            ImGui::Checkbox("Recursive", &perm_recurse);
            
            if (ImGui::Button("Apply", ImVec2(120,0))) {
                uint32_t m = strtoul(perm_mode, NULL, 8);
                uint32_t u = strtoul(perm_uid, NULL, 10);
                uint32_t g = strtoul(perm_gid, NULL, 10);
                for (auto& name : selected_items) {
                    vhd.SetPermissionsRecursive(GetFullPath(name), m, perm_recurse);
                    vhd.SetOwnerRecursive(GetFullPath(name), u, g, perm_recurse);
                }
                needs_refresh=true; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (show_delete_popup) { ImGui::OpenPopup("Confirm Delete"); show_delete_popup = false; }
        if (ImGui::BeginPopupModal("Confirm Delete", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (selected_items.empty()) {
                ImGui::Text("No items selected for deletion.");
                ImGui::Spacing();
                if (ImGui::Button("Close", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            } else {
                if (selected_items.size() == 1) {
                    ImGui::Text("Are you sure you want to permanently delete '%s'?", selected_items.begin()->c_str());
                } else {
                    ImGui::Text("Are you sure you want to permanently delete these %zu items?", selected_items.size());
                    ImGui::Spacing();
                    ImGui::BeginChild("DeleteList", ImVec2(300, 100), true);
                    for (const auto& item : selected_items) {
                        ImGui::BulletText("%s", item.c_str());
                    }
                    ImGui::EndChild();
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.05f, 0.1f, 1.0f));
                if (ImGui::Button("Delete permanently", ImVec2(160, 0))) {
                    error_msg = "";
                    for (auto& s : selected_items) {
                        if (!vhd.DeleteRecursive(GetFullPath(s))) {
                            error_msg = "Failed to delete: " + vhd.GetLastError();
                        }
                    }
                    needs_refresh = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor(3);
                
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        if (show_rename_popup) { ImGui::OpenPopup("Rename Item"); show_rename_popup=false; }
        if (ImGui::BeginPopupModal("Rename Item", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("Name", &rename_new);
            
            bool exists = false;
            for (const auto& file : files) {
                if (file.name == rename_new && file.name != rename_old) {
                    exists = true;
                    break;
                }
            }
            if (exists) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Warning: An item with this name already exists!");
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            if (ImGui::Button("OK", ImVec2(120,0))) {
                error_msg = "";
                if (rename_old.empty()) {
                    if (!vhd.MakeDirectory(GetFullPath(rename_new))) {
                        error_msg = "Failed to create directory: " + vhd.GetLastError();
                    }
                } else {
                    if (!vhd.Rename(GetFullPath(rename_old), GetFullPath(rename_new))) {
                        error_msg = "Failed to rename: " + vhd.GetLastError();
                    }
                }
                needs_refresh=true; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (show_newfile_popup) { ImGui::OpenPopup("New File"); show_newfile_popup=false; }
        if (ImGui::BeginPopupModal("New File", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("Filename", &rename_new);
            
            bool exists = false;
            for (const auto& file : files) {
                if (file.name == rename_new) {
                    exists = true;
                    break;
                }
            }
            if (exists) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Warning: A file with this name already exists!");
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            if (ImGui::Button("Create", ImVec2(120,0))) {
                error_msg = "";
                HANDLE h = CreateFileA("temp_empty", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    if (!vhd.CopyFileFromHost("temp_empty", GetFullPath(rename_new))) {
                        error_msg = "Failed to create file: " + vhd.GetLastError();
                    }
                    DeleteFileA("temp_empty");
                } else {
                    error_msg = "Failed to create local temporary file.";
                }
                needs_refresh=true; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::End();
    }
};

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"VirtualExt4ExplorerClass", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Virtual Ext4 Explorer", WS_OVERLAPPEDWINDOW, 100, 100, 1024, 768, nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) return 1;
    ::ShowWindow(hwnd, SW_SHOWDEFAULT); ::UpdateWindow(hwnd);
    ::DragAcceptFiles(hwnd, TRUE);
    
    // Allow drag-and-drop messages through Windows User Interface Privilege Isolation (UIPI)
    // if running with elevated administrator privileges.
    typedef BOOL(WINAPI* LPFN_ChangeWindowMessageFilterEx)(HWND, UINT, DWORD, PVOID);
    typedef BOOL(WINAPI* LPFN_ChangeWindowMessageFilter)(UINT, DWORD);
    HMODULE hUser32 = ::GetModuleHandleA("user32.dll");
    if (hUser32) {
        LPFN_ChangeWindowMessageFilterEx pChangeWindowMessageFilterEx = 
            (LPFN_ChangeWindowMessageFilterEx)::GetProcAddress(hUser32, "ChangeWindowMessageFilterEx");
        LPFN_ChangeWindowMessageFilter pChangeWindowMessageFilter = 
            (LPFN_ChangeWindowMessageFilter)::GetProcAddress(hUser32, "ChangeWindowMessageFilter");
        if (pChangeWindowMessageFilterEx) {
            pChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, 1 /* MSGFLT_ALLOW */, NULL);
            pChangeWindowMessageFilterEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, 1 /* MSGFLT_ALLOW */, NULL);
            pChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, 1 /* MSGFLT_ALLOW */, NULL);
        } else if (pChangeWindowMessageFilter) {
            pChangeWindowMessageFilter(WM_DROPFILES, 1 /* MSGFLT_ADD */);
            pChangeWindowMessageFilter(0x0049 /* WM_COPYGLOBALDATA */, 1 /* MSGFLT_ADD */);
            pChangeWindowMessageFilter(WM_COPYDATA, 1 /* MSGFLT_ADD */);
        }
    }

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    ExplorerApp app;
    g_pAppInstance = &app;
    bool done = false;
    while (!done) {
        MSG msg; while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessage(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        app.Draw();
        ImGui::Render();
        const float clear_color[4] = { 0.15f, 0.15f, 0.15f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupDeviceD3D(); ::DestroyWindow(hwnd); ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd; ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pd3dDeviceContext);
    if (res != S_OK) return false;
    CreateRenderTarget(); return true;
}
void CleanupDeviceD3D() { CleanupRenderTarget(); if (g_pSwapChain) g_pSwapChain->Release(); if (g_pd3dDeviceContext) g_pd3dDeviceContext->Release(); if (g_pd3dDevice) g_pd3dDevice->Release(); }
void CreateRenderTarget() { ID3D11Texture2D* p; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&p)); g_pd3dDevice->CreateRenderTargetView(p, nullptr, &g_mainRenderTargetView); p->Release(); }
void CleanupRenderTarget() { if (g_mainRenderTargetView) g_mainRenderTargetView->Release(); }
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE: if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) { CleanupRenderTarget(); g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0); CreateRenderTarget(); } return 0;
    case WM_DROPFILES:
        if (g_pAppInstance) {
            HDROP hDrop = (HDROP)wParam;
            UINT fileCount = DragQueryFileA(hDrop, 0xFFFFFFFF, NULL, 0);
            std::vector<std::string> droppedPaths;
            for (UINT i = 0; i < fileCount; i++) {
                char szFilePath[MAX_PATH];
                if (DragQueryFileA(hDrop, i, szFilePath, MAX_PATH)) {
                    droppedPaths.push_back(szFilePath);
                }
            }
            DragFinish(hDrop);
            g_pAppInstance->HandleDroppedFiles(droppedPaths);
        }
        return 0;
    case WM_SYSCOMMAND: if ((wParam & 0xFFF0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: ::PostQuitMessage(0); return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
