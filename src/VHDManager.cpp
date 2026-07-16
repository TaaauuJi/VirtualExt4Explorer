/*
 * VirtualExt4Explorer
 * Copyright (c) 2026 Taaauu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "VHDManager.h"
#include <initguid.h>
#include <virtdisk.h>
#include <winioctl.h>
#pragma comment(lib, "virtdisk.lib")

extern "C" {
#include <ext4.h>
#include <ext4_blockdev.h>
}

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

// MBR structures
#pragma pack(push, 1)
struct MBRPartitionEntry {
    uint8_t  status;
    uint8_t  first_chs[3];
    uint8_t  partition_type;
    uint8_t  last_chs[3];
    uint32_t lba_start;
    uint32_t sector_count;
};

struct MBR {
    uint8_t  bootloader[446];
    MBRPartitionEntry partitions[4];
    uint16_t signature;
};

struct VHDFooter {
    char cookie[8];
    uint32_t features;
    uint32_t file_format_version;
    uint64_t data_offset;
    uint32_t timestamp;
    char creator_app[4];
    uint32_t creator_version;
    uint32_t creator_host_os;
    uint64_t original_size;
    uint64_t current_size;
    uint32_t disk_geometry;
    uint32_t disk_type;
    uint32_t checksum;
    uint8_t  unique_id[16];
    uint8_t  saved_state;
    uint8_t  reserved[427];
};

struct VHDDynamicHeader {
    char cookie[8];         // "cxsparse"
    uint64_t data_offset;
    uint64_t table_offset;
    uint32_t header_version;
    uint32_t max_table_entries;
    uint32_t block_size;
    uint32_t checksum;
    uint8_t  parent_uuid[16];
    uint32_t parent_timestamp;
    uint32_t reserved1;
    uint8_t  parent_name[512];
    uint8_t  parent_locators[192];
    uint8_t  reserved2[256];
};
#pragma pack(pop)

static std::string MakeError(const char* msg, const char* detail) {
    char buf[512]; snprintf(buf, sizeof(buf), "%s%s", msg, detail); return std::string(buf);
}

static std::string MakeError(const char* msg, int code) {
    char buf[512]; snprintf(buf, sizeof(buf), "%s%d", msg, code); return std::string(buf);
}

static std::string MakeError(const char* msg, unsigned long code) {
    char buf[512]; snprintf(buf, sizeof(buf), "%s%lu", msg, code); return std::string(buf);
}

struct DynamicVHDInfo {
    bool is_dynamic;
    uint32_t block_size;
    uint32_t max_table_entries;
    uint64_t table_offset;
    uint64_t virtual_size;
    uint32_t* bat;
};

static DynamicVHDInfo g_dynVHD = {};

static uint64_t GetDiskSize(HANDLE h) {
    GET_LENGTH_INFORMATION li = {0};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof(li), &bytesReturned, NULL)) return li.Length.QuadPart;
    LARGE_INTEGER fileSize; if (GetFileSizeEx(h, &fileSize)) return (uint64_t)fileSize.QuadPart;
    return 0;
}

static bool AllocateDynamicBlock(HANDLE hFile, uint32_t block_index) {
    if (block_index >= g_dynVHD.max_table_entries) return false;
    LARGE_INTEGER fileSize; if (!GetFileSizeEx(hFile, &fileSize)) return false;
    uint64_t new_block_file_offset = fileSize.QuadPart - 512;
    uint32_t new_bat_sector = (uint32_t)(new_block_file_offset / 512);
    uint32_t bitmap_sectors = (g_dynVHD.block_size / 512 + 7) / 8;
    uint32_t bitmap_padded = (bitmap_sectors + 511) & ~511u; if (bitmap_padded == 0) bitmap_padded = 512;
    LARGE_INTEGER li; li.QuadPart = new_block_file_offset; if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) return false;
    { std::vector<uint8_t> bitmap(bitmap_padded, 0xFF); DWORD written; WriteFile(hFile, bitmap.data(), bitmap_padded, &written, NULL); }
    { std::vector<uint8_t> zeros(65536, 0); uint32_t rem = g_dynVHD.block_size; while(rem>0) { uint32_t tw=(rem<65536)?rem:65536; DWORD wr; WriteFile(hFile, zeros.data(), tw, &wr, NULL); rem-=tw; } }
    uint8_t footer[512]; li.QuadPart = 0; SetFilePointerEx(hFile, li, NULL, FILE_BEGIN); DWORD br; ReadFile(hFile, footer, 512, &br, NULL);
    li.QuadPart = new_block_file_offset + bitmap_padded + g_dynVHD.block_size; SetFilePointerEx(hFile, li, NULL, FILE_BEGIN); DWORD bw; WriteFile(hFile, footer, 512, &bw, NULL);
    g_dynVHD.bat[block_index] = new_bat_sector;
    uint32_t be_bat = _byteswap_ulong(new_bat_sector); li.QuadPart = g_dynVHD.table_offset + ((uint64_t)block_index * 4); SetFilePointerEx(hFile, li, NULL, FILE_BEGIN); WriteFile(hFile, &be_bat, 4, &bw, NULL);
    FlushFileBuffers(hFile); return true;
}

VHDManager::VHDManager() : m_hVHD(INVALID_HANDLE_VALUE), m_ext4_bdev(nullptr), m_bdif(nullptr), m_ext4_mounted(false), m_mounted_partition_index(-1), m_virtDiskHandle(NULL), m_isVirtDiskAttached(false) { memset(&m_block_device, 0, sizeof(m_block_device)); }
VHDManager::~VHDManager() { CloseVHD(); }

#pragma pack(push, 1)
struct VDIHeader {
    char szFileInfo[64];
    uint32_t u32Signature;
    uint32_t u32Version;
    uint32_t u32HeaderSize;
    uint32_t u32ImageType;   // 1=dynamic, 2=fixed
    uint32_t u32Flags;
    char szComment[256];
    uint32_t u32OffsetBlocks;
    uint32_t u32OffsetData;
    uint32_t u32Geometry[4]; // cylinders, heads, sectors, sector_size
    uint32_t u32Unused;
    uint64_t u64DiskSize;
    uint32_t u32BlockSize;
    uint32_t u32BlockExtraData;
    uint32_t u32BlocksInImage;
    uint32_t u32BlocksAllocated;
};
#pragma pack(pop)

struct DynamicVDIInfo {
    bool is_vdi;
    bool is_dynamic;
    uint32_t block_size;
    uint32_t blocks_in_image;
    uint32_t data_offset;
    uint32_t bat_offset;  // file offset of BAT
    uint64_t virtual_size;
    uint32_t* bat;  // block allocation table
};

static DynamicVDIInfo g_dynVDI = {};

bool VHDManager::OpenVHD(const std::string& path) {
    CloseVHD(); m_vhd_path = path;
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    if (ext == ".vhdx") {
        VIRTUAL_STORAGE_TYPE st = { VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT };
        std::wstring wp(path.begin(), path.end()); HANDLE h = NULL;
        DWORD oret = OpenVirtualDisk(&st, wp.c_str(), VIRTUAL_DISK_ACCESS_ALL, OPEN_VIRTUAL_DISK_FLAG_NONE, NULL, &h);
        if (oret != ERROR_SUCCESS) { m_last_error = MakeError("OpenVirtualDisk failed: ", oret); return false; }
        
        ATTACH_VIRTUAL_DISK_PARAMETERS ap = {};
        ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        DWORD aret = AttachVirtualDisk(h, NULL, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &ap, NULL);
        if (aret != ERROR_SUCCESS) {
            CloseHandle(h); m_last_error = MakeError("AttachVirtualDisk failed: ", aret); return false;
        }
        WCHAR dp[MAX_PATH]; ULONG dps = sizeof(dp); GetVirtualDiskPhysicalPath(h, &dps, dp);
        m_hVHD = CreateFileW(dp, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (m_hVHD == INVALID_HANDLE_VALUE) {
            // Try read-only
            m_hVHD = CreateFileW(dp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
        if (m_hVHD == INVALID_HANDLE_VALUE) {
            DetachVirtualDisk(h, DETACH_VIRTUAL_DISK_FLAG_NONE, 0); CloseHandle(h);
            m_last_error = MakeError("CreateFile on physical path failed: ", (unsigned long)::GetLastError()); return false;
        }
        m_virtDiskHandle = h; m_isVirtDiskAttached = true; g_dynVHD.is_dynamic = false;
        return ParsePartitions();
    }
    m_hVHD = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (m_hVHD == INVALID_HANDLE_VALUE) {
        m_last_error = MakeError("Failed to open file (error ", (unsigned long)::GetLastError());
        return false;
    }
    if (ext == ".vdi") {
        VDIHeader hdr; DWORD br;
        SetFilePointer(m_hVHD, 0, NULL, FILE_BEGIN);
        ReadFile(m_hVHD, &hdr, sizeof(hdr), &br, NULL);
        if (hdr.u32Signature != 0xBEDA107F) {
            m_last_error = MakeError("Not a valid VDI file (bad signature): ", m_vhd_path.c_str());
            CloseVHD();
            return false;
        }
        g_dynVDI.is_vdi = true;
        g_dynVDI.block_size = hdr.u32BlockSize;
        g_dynVDI.blocks_in_image = hdr.u32BlocksInImage;
        g_dynVDI.data_offset = hdr.u32OffsetData;
        g_dynVDI.bat_offset = hdr.u32OffsetBlocks;
        g_dynVDI.virtual_size = hdr.u64DiskSize;
        g_dynVDI.is_dynamic = (hdr.u32ImageType == 1);
        // Read the block allocation table
        g_dynVDI.bat = new uint32_t[hdr.u32BlocksInImage];
        LARGE_INTEGER li; li.QuadPart = hdr.u32OffsetBlocks;
        SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
        ReadFile(m_hVHD, g_dynVDI.bat, hdr.u32BlocksInImage * 4, &br, NULL);

        // Read virtual sector 0 (MBR) through the BAT translation.
        uint8_t s0[512]; memset(s0, 0, sizeof(s0));
        {
            uint32_t bi = 0; // virtual offset 0 -> block 0, sub-offset 0
            if (bi < g_dynVDI.blocks_in_image && g_dynVDI.bat[bi] != 0xFFFFFFFF) {
                LARGE_INTEGER pli;
                pli.QuadPart = (uint64_t)g_dynVDI.data_offset + (uint64_t)g_dynVDI.bat[bi] * g_dynVDI.block_size;
                SetFilePointerEx(m_hVHD, pli, NULL, FILE_BEGIN);
                ReadFile(m_hVHD, s0, 512, &br, NULL);
            }
        }
        MBR* mbr = (MBR*)s0;
        if (mbr->signature == 0xAA55) {
            for (int i = 0; i < 4; i++) {
                if (mbr->partitions[i].partition_type == 0) continue;
                PartitionInfo info = {
                    mbr->partitions[i].partition_type,
                    (uint64_t)mbr->partitions[i].lba_start * 512,
                    (uint64_t)mbr->partitions[i].sector_count * 512,
                    mbr->partitions[i].partition_type == 0x83
                };
                m_partitions.push_back(info);
            }
        }
        if (m_partitions.empty()) {
            // No partition table: treat the whole disk as a raw ext4 image.
            PartitionInfo info = { 0x83, 0, hdr.u64DiskSize, true };
            m_partitions.push_back(info);
        }
        return true;
    }
    if (!ReadVHDFooter()) { CloseVHD(); return false; }
    return ParsePartitions();
}

void VHDManager::CloseVHD() {
    UnmountExt4();
    if (g_dynVHD.bat) delete[] g_dynVHD.bat; g_dynVHD.bat = nullptr; g_dynVHD.is_dynamic = false;
    if (g_dynVDI.bat) delete[] g_dynVDI.bat; g_dynVDI.bat = nullptr; g_dynVDI = {};
    if (m_hVHD != INVALID_HANDLE_VALUE) CloseHandle(m_hVHD); m_hVHD = INVALID_HANDLE_VALUE;
    if (m_isVirtDiskAttached) { DetachVirtualDisk(m_virtDiskHandle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0); CloseHandle(m_virtDiskHandle); }
    m_isVirtDiskAttached = false; m_partitions.clear(); m_vhd_path.clear();
}

bool VHDManager::ReadVHDFooter() {
    uint8_t buffer[512]; DWORD br; SetFilePointer(m_hVHD, 0, NULL, FILE_BEGIN); ReadFile(m_hVHD, buffer, 512, &br, NULL);
    VHDFooter* f = (VHDFooter*)buffer;
    if (memcmp(f->cookie, "conectix", 8) == 0) {
        uint32_t dt = _byteswap_ulong(f->disk_type); uint64_t cur_sz = _byteswap_uint64(f->current_size);
        if (dt == 3) {
            g_dynVHD.is_dynamic = true; g_dynVHD.virtual_size = cur_sz;
            LARGE_INTEGER li; li.QuadPart = _byteswap_uint64(f->data_offset); SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
            uint8_t db[1024]; ReadFile(m_hVHD, db, 1024, &br, NULL); VHDDynamicHeader* dh = (VHDDynamicHeader*)db;
            if (memcmp(dh->cookie, "cxsparse", 8) == 0) {
                g_dynVHD.table_offset = _byteswap_uint64(dh->table_offset); g_dynVHD.max_table_entries = _byteswap_ulong(dh->max_table_entries); g_dynVHD.block_size = _byteswap_ulong(dh->block_size);
                g_dynVHD.bat = new uint32_t[g_dynVHD.max_table_entries]; li.QuadPart = g_dynVHD.table_offset; SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
                ReadFile(m_hVHD, g_dynVHD.bat, g_dynVHD.max_table_entries * 4, &br, NULL);
                for (uint32_t i = 0; i < g_dynVHD.max_table_entries; i++) g_dynVHD.bat[i] = _byteswap_ulong(g_dynVHD.bat[i]);
            }
        }
        return true;
    }
    return false;
}

bool VHDManager::ParsePartitions() {
    uint8_t s0[512]; DWORD br;
    if (g_dynVHD.is_dynamic) {
        uint32_t bat = g_dynVHD.bat[0]; if (bat == 0xFFFFFFFF) return false;
        uint32_t bm = (g_dynVHD.block_size / 512 + 7) / 8; bm = (bm + 511) / 512; if (bm == 0) bm = 1;
        LARGE_INTEGER li; li.QuadPart = (uint64_t)bat * 512 + (uint64_t)bm * 512; SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN); ReadFile(m_hVHD, s0, 512, &br, NULL);
    } else {
        SetFilePointer(m_hVHD, 0, NULL, FILE_BEGIN); ReadFile(m_hVHD, s0, 512, &br, NULL);
        if (memcmp(s0, "conectix", 8) == 0) { SetFilePointer(m_hVHD, 512, NULL, FILE_BEGIN); ReadFile(m_hVHD, s0, 512, &br, NULL); }
    }
    MBR* mbr = (MBR*)s0;
    if (mbr->signature == 0xAA55) {
        for (int i = 0; i < 4; i++) {
            if (mbr->partitions[i].partition_type == 0) continue;
            PartitionInfo info = { mbr->partitions[i].partition_type, (uint64_t)mbr->partitions[i].lba_start * 512, (uint64_t)mbr->partitions[i].sector_count * 512, mbr->partitions[i].partition_type == 0x83 };
            m_partitions.push_back(info);
        }
        if (!m_partitions.empty()) return true;
    }
    PartitionInfo raw = { 0x83, 0, g_dynVHD.is_dynamic ? g_dynVHD.virtual_size : GetDiskSize(m_hVHD), true };
    m_partitions.push_back(raw); return true;
}

bool VHDManager::IsExt4Filesystem(uint64_t offset) {
    LARGE_INTEGER p; p.QuadPart = offset + 1024 + 0x38; SetFilePointerEx(m_hVHD, p, NULL, FILE_BEGIN);
    uint16_t m; DWORD br; return ReadFile(m_hVHD, &m, 2, &br, NULL) && m == 0xEF53;
}

int VHDManager::BlockOpen(struct ext4_blockdev* bdev) { return EOK; }
int VHDManager::BlockClose(struct ext4_blockdev* bdev) { return EOK; }
int VHDManager::BlockRead(struct ext4_blockdev* bdev, void* buf, uint64_t blk_id, uint32_t blk_cnt) {
    VHDBlockDevice* vhd = (VHDBlockDevice*)bdev->bdif->p_user; uint8_t* out = (uint8_t*)buf;
    uint32_t i = 0;
    while (i < blk_cnt) {
        uint64_t start_voff = vhd->partition_offset + ((blk_id + i) * 512);
        uint64_t start_poff;
        if (g_dynVDI.is_vdi) {
            uint32_t bi = (uint32_t)(start_voff / g_dynVDI.block_size);
            uint32_t oi = (uint32_t)(start_voff % g_dynVDI.block_size);
            if (bi >= g_dynVDI.blocks_in_image || g_dynVDI.bat[bi] == 0xFFFFFFFF) {
                memset(out + i*512, 0, 512);
                i++;
                continue;
            }
            start_poff = (uint64_t)g_dynVDI.data_offset + (uint64_t)g_dynVDI.bat[bi] * g_dynVDI.block_size + oi;
        } else if (g_dynVHD.is_dynamic) {
            uint32_t bi = (uint32_t)(start_voff / g_dynVHD.block_size);
            uint32_t oi = (uint32_t)(start_voff % g_dynVHD.block_size);
            uint32_t bat = g_dynVHD.bat[bi];
            if (bat == 0xFFFFFFFF) {
                memset(out + i*512, 0, 512);
                i++;
                continue;
            }
            uint32_t bm = (g_dynVHD.block_size/512+7)/8; bm=(bm+511)/512; if(bm==0) bm=1;
            start_poff = (uint64_t)bat*512 + (uint64_t)bm*512 + oi;
        } else {
            start_poff = start_voff;
        }

        // Count contiguous sectors
        uint32_t run_cnt = 1;
        while (i + run_cnt < blk_cnt) {
            uint64_t next_voff = vhd->partition_offset + ((blk_id + i + run_cnt) * 512);
            uint64_t next_poff;
            if (g_dynVDI.is_vdi) {
                uint32_t bi = (uint32_t)(next_voff / g_dynVDI.block_size);
                uint32_t oi = (uint32_t)(next_voff % g_dynVDI.block_size);
                if (bi >= g_dynVDI.blocks_in_image || g_dynVDI.bat[bi] == 0xFFFFFFFF) break;
                next_poff = (uint64_t)g_dynVDI.data_offset + (uint64_t)g_dynVDI.bat[bi] * g_dynVDI.block_size + oi;
            } else if (g_dynVHD.is_dynamic) {
                uint32_t bi = (uint32_t)(next_voff / g_dynVHD.block_size);
                uint32_t oi = (uint32_t)(next_voff % g_dynVHD.block_size);
                uint32_t bat = g_dynVHD.bat[bi];
                if (bat == 0xFFFFFFFF) break;
                uint32_t bm = (g_dynVHD.block_size/512+7)/8; bm=(bm+511)/512; if(bm==0) bm=1;
                next_poff = (uint64_t)bat*512 + (uint64_t)bm*512 + oi;
            } else {
                next_poff = next_voff;
            }

            if (next_poff == start_poff + (run_cnt * 512)) {
                run_cnt++;
            } else {
                break;
            }
        }

        LARGE_INTEGER li; li.QuadPart = start_poff;
        SetFilePointerEx(vhd->hFile, li, NULL, FILE_BEGIN);
        DWORD br;
        ReadFile(vhd->hFile, out + i*512, run_cnt * 512, &br, NULL);

        i += run_cnt;
    }
    return EOK;
}

int VHDManager::BlockWrite(struct ext4_blockdev* bdev, const void* buf, uint64_t blk_id, uint32_t blk_cnt) {
    VHDBlockDevice* vhd = (VHDBlockDevice*)bdev->bdif->p_user; const uint8_t* in = (const uint8_t*)buf;
    uint32_t i = 0;
    while (i < blk_cnt) {
        uint64_t start_voff = vhd->partition_offset + ((blk_id + i) * 512);
        uint64_t start_poff;
        if (g_dynVDI.is_vdi) {
            uint32_t bi = (uint32_t)(start_voff / g_dynVDI.block_size);
            uint32_t oi = (uint32_t)(start_voff % g_dynVDI.block_size);
            if (bi >= g_dynVDI.blocks_in_image) return EIO;
            if (g_dynVDI.bat[bi] == 0xFFFFFFFF) {
                uint32_t max_blk = 0;
                for (uint32_t j = 0; j < g_dynVDI.blocks_in_image; j++)
                    if (g_dynVDI.bat[j] != 0xFFFFFFFF && g_dynVDI.bat[j] > max_blk) max_blk = g_dynVDI.bat[j];
                uint32_t new_blk = max_blk + 1;
                g_dynVDI.bat[bi] = new_blk;
                LARGE_INTEGER bpos; bpos.QuadPart = (uint64_t)g_dynVDI.bat_offset + (uint64_t)bi * 4;
                SetFilePointerEx(vhd->hFile, bpos, NULL, FILE_BEGIN);
                DWORD bw3; WriteFile(vhd->hFile, &g_dynVDI.bat[bi], 4, &bw3, NULL);
                std::vector<uint8_t> zeros(g_dynVDI.block_size, 0);
                LARGE_INTEGER dpos; dpos.QuadPart = (uint64_t)g_dynVDI.data_offset + (uint64_t)new_blk * g_dynVDI.block_size;
                SetFilePointerEx(vhd->hFile, dpos, NULL, FILE_BEGIN);
                DWORD bw2; WriteFile(vhd->hFile, zeros.data(), g_dynVDI.block_size, &bw2, NULL);
            }
            start_poff = (uint64_t)g_dynVDI.data_offset + (uint64_t)g_dynVDI.bat[bi] * g_dynVDI.block_size + oi;
        } else if (g_dynVHD.is_dynamic) {
            uint32_t bi = (uint32_t)(start_voff / g_dynVHD.block_size);
            uint32_t oi = (uint32_t)(start_voff % g_dynVHD.block_size);
            if (g_dynVHD.bat[bi] == 0xFFFFFFFF && !AllocateDynamicBlock(vhd->hFile, bi)) return EIO;
            uint32_t bm = (g_dynVHD.block_size/512+7)/8; bm=(bm+511)/512; if(bm==0) bm=1;
            start_poff = (uint64_t)g_dynVHD.bat[bi]*512 + (uint64_t)bm*512 + oi;
        } else {
            start_poff = start_voff;
        }

        // Count contiguous sectors
        uint32_t run_cnt = 1;
        while (i + run_cnt < blk_cnt) {
            uint64_t next_voff = vhd->partition_offset + ((blk_id + i + run_cnt) * 512);
            uint64_t next_poff;
            if (g_dynVDI.is_vdi) {
                uint32_t bi = (uint32_t)(next_voff / g_dynVDI.block_size);
                uint32_t oi = (uint32_t)(next_voff % g_dynVDI.block_size);
                if (bi >= g_dynVDI.blocks_in_image || g_dynVDI.bat[bi] == 0xFFFFFFFF) break;
                next_poff = (uint64_t)g_dynVDI.data_offset + (uint64_t)g_dynVDI.bat[bi] * g_dynVDI.block_size + oi;
            } else if (g_dynVHD.is_dynamic) {
                uint32_t bi = (uint32_t)(next_voff / g_dynVHD.block_size);
                uint32_t oi = (uint32_t)(next_voff % g_dynVHD.block_size);
                uint32_t bat = g_dynVHD.bat[bi];
                if (bat == 0xFFFFFFFF) break;
                uint32_t bm = (g_dynVHD.block_size/512+7)/8; bm=(bm+511)/512; if(bm==0) bm=1;
                next_poff = (uint64_t)bat*512 + (uint64_t)bm*512 + oi;
            } else {
                next_poff = next_voff;
            }

            if (next_poff == start_poff + (run_cnt * 512)) {
                run_cnt++;
            } else {
                break;
            }
        }

        LARGE_INTEGER li; li.QuadPart = start_poff;
        SetFilePointerEx(vhd->hFile, li, NULL, FILE_BEGIN);
        DWORD bw;
        WriteFile(vhd->hFile, in + i*512, run_cnt * 512, &bw, NULL);

        i += run_cnt;
    }
    FlushFileBuffers(vhd->hFile);
    return EOK;
}

bool VHDManager::MountExt4Partition(int idx) {
    if (idx < 0 || idx >= (int)m_partitions.size()) { m_last_error = "Invalid partition index"; return false; }
    const PartitionInfo& p = m_partitions[idx]; if (!p.is_ext4) { m_last_error = "Partition is not ext4"; return false; }
    UnmountExt4();
    m_block_device = { m_hVHD, p.offset, p.size, 512 };
    m_ext4_bdev = new ext4_blockdev(); memset(m_ext4_bdev, 0, sizeof(ext4_blockdev));
    m_bdif = new ext4_blockdev_iface(); memset(m_bdif, 0, sizeof(ext4_blockdev_iface));
    m_bdif->open = BlockOpen; m_bdif->bread = BlockRead; m_bdif->bwrite = BlockWrite; m_bdif->close = BlockClose;
    m_bdif->ph_bsize = 512; m_bdif->ph_bcnt = p.size / 512; m_bdif->ph_bbuf = new uint8_t[512]; m_bdif->p_user = &m_block_device;
    m_ext4_bdev->bdif = m_bdif; m_ext4_bdev->part_size = p.size;
    int rc = ext4_device_register(m_ext4_bdev, "vhd");
    if (rc != EOK) { m_last_error = MakeError("ext4_device_register failed: ", rc); return false; }
    rc = ext4_mount("vhd", "/", false);
    if (rc != EOK) { m_last_error = MakeError("ext4_mount failed: ", rc); ext4_device_unregister("vhd"); return false; }
    m_ext4_mounted = true; m_mounted_partition_index = idx; return true;
}

void VHDManager::UnmountExt4() {
    if (!m_ext4_mounted) return;
    ext4_umount("/"); ext4_device_unregister("vhd");
    if (m_bdif) delete[] m_bdif->ph_bbuf; delete m_bdif; delete m_ext4_bdev;
    m_bdif = nullptr; m_ext4_bdev = nullptr; m_ext4_mounted = false;
}

bool VHDManager::FileExists(const std::string& p) { ext4_file f; if (ext4_fopen(&f, p.c_str(), "rb") == EOK) { ext4_fclose(&f); return true; } return false; }

bool VHDManager::CopyFileFromHost(const std::string& h, const std::string& e, ProgressCallback cb) {
    FILE* hf = fopen(h.c_str(), "rb"); if (!hf) return false;
    ext4_file ef; if (ext4_fopen(&ef, e.c_str(), "wb") != EOK) { fclose(hf); return false; }
    const size_t buf_size = 256 * 1024;
    std::vector<char> b(buf_size);
    size_t br, bw;
    while ((br = fread(b.data(), 1, buf_size, hf)) > 0) {
        ext4_fwrite(&ef, b.data(), br, &bw);
        if (cb) cb(br);
    }
    fclose(hf); ext4_fclose(&ef); return true;
}

bool VHDManager::CopyFileToHost(const std::string& e, const std::string& h, ProgressCallback cb) {
    ext4_file ef; if (ext4_fopen(&ef, e.c_str(), "rb") != EOK) return false;
    FILE* hf = fopen(h.c_str(), "wb"); if (!hf) { ext4_fclose(&ef); return false; }
    const size_t buf_size = 256 * 1024;
    std::vector<char> b(buf_size);
    size_t br;
    while (ext4_fread(&ef, b.data(), buf_size, &br) == EOK && br > 0) {
        fwrite(b.data(), 1, br, hf);
        if (cb) cb(br);
    }
    fclose(hf); ext4_fclose(&ef); return true;
}

bool VHDManager::CopyInternal(const std::string& s, const std::string& d) {
    ext4_file sf, df; if (ext4_fopen(&sf, s.c_str(), "rb") != EOK) return false;
    if (ext4_fopen(&df, d.c_str(), "wb") != EOK) { ext4_fclose(&sf); return false; }
    const size_t buf_size = 256 * 1024;
    std::vector<char> b(buf_size);
    size_t br, bw;
    while (ext4_fread(&sf, b.data(), buf_size, &br) == EOK && br > 0) ext4_fwrite(&df, b.data(), br, &bw);
    ext4_fclose(&sf); ext4_fclose(&df); return true;
}

bool VHDManager::DeleteRecursive(const std::string& p) {
    if (ext4_dir_rm(p.c_str()) == EOK) return true;
    return ext4_fremove(p.c_str()) == EOK;
}

bool VHDManager::ExportRecursive(const std::string& e, const std::string& h, ProgressCallback cb) {
    std::vector<FileInfo> entries;
    if (ListDirectoryInfo(e, entries)) {
        if (!CreateDirectoryA(h.c_str(), NULL) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
        for (const auto& entry : entries) {
            std::string sub_e = e + (e.back() == '/' ? "" : "/") + entry.name;
            std::string sub_h = h + "\\" + entry.name;
            if (entry.is_dir) { if (!ExportRecursive(sub_e, sub_h, cb)) return false; }
            else { if (!CopyFileToHost(sub_e, sub_h, cb)) return false; }
        }
        return true;
    }
    return CopyFileToHost(e, h, cb);
}

bool VHDManager::ImportRecursive(const std::string& h, const std::string& e, ProgressCallback cb) {
    DWORD attr = GetFileAttributesA(h.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return CopyFileFromHost(h, e, cb);
    
    MakeDirectory(e);
    WIN32_FIND_DATAA fd;
    std::string search = h + "\\*";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return true;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        std::string sub_h = h + "\\" + n;
        std::string sub_e = e + (e.back() == '/' ? "" : "/") + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ImportRecursive(sub_h, sub_e, cb);
        else CopyFileFromHost(sub_h, sub_e, cb);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return true;
}

uint64_t VHDManager::GetExt4SizeRecursive(const std::string& path) {
    std::vector<FileInfo> entries;
    uint64_t total_size = 0;
    if (ListDirectoryInfo(path, entries)) {
        for (const auto& entry : entries) {
            std::string sub = path + (path.back() == '/' ? "" : "/") + entry.name;
            if (entry.is_dir) {
                total_size += GetExt4SizeRecursive(sub);
            } else {
                total_size += entry.size;
            }
        }
    } else {
        ext4_file f;
        if (ext4_fopen(&f, path.c_str(), "rb") == EOK) {
            total_size += ext4_fsize(&f);
            ext4_fclose(&f);
        }
    }
    return total_size;
}

uint64_t VHDManager::GetHostSizeRecursive(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            GetFileSizeEx(hFile, &li);
            CloseHandle(hFile);
            return li.QuadPart;
        }
        return 0;
    }
    
    uint64_t total_size = 0;
    WIN32_FIND_DATAA fd;
    std::string search = path + "\\*";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        std::string sub = path + "\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total_size += GetHostSizeRecursive(sub);
        } else {
            LARGE_INTEGER li;
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            total_size += li.QuadPart;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return total_size;
}

bool VHDManager::Rename(const std::string& o, const std::string& n) { return ext4_frename(o.c_str(), n.c_str()) == EOK; }

bool VHDManager::ListDirectoryInfo(const std::string& path, std::vector<FileInfo>& entries) {
    ext4_dir d; if (ext4_dir_open(&d, path.c_str()) != EOK) return false;
    const ext4_direntry* de; while ((de = ext4_dir_entry_next(&d)) != nullptr) {
        std::string n((char*)de->name, de->name_length); if (n == "." || n == "..") continue;
        std::string fp = path + (path.back() == '/' ? "" : "/") + n;
        
        uint32_t mode = 0, uid = 0, gid = 0;
        ext4_mode_get(fp.c_str(), &mode);
        ext4_owner_get(fp.c_str(), &uid, &gid);

        FileInfo info;
        info.name = n;
        info.is_dir = (de->inode_type == EXT4_DE_DIR);
        info.mode = mode;
        info.uid = uid;
        info.gid = gid;
        info.size = 0;

        if (!info.is_dir) {
            ext4_file f;
            if (ext4_fopen(&f, fp.c_str(), "rb") == EOK) {
                info.size = ext4_fsize(&f);
                ext4_fclose(&f);
            }
        }
        entries.push_back(info);
    }
    ext4_dir_close(&d); return true;
}

bool VHDManager::SetFilePermissions(const std::string& p, uint32_t m) { return ext4_mode_set(p.c_str(), m) == EOK; }
bool VHDManager::SetFileOwner(const std::string& p, uint32_t u, uint32_t g) { return ext4_owner_set(p.c_str(), u, g) == EOK; }

bool VHDManager::SetPermissionsRecursive(const std::string& path, uint32_t mode, bool recurse) {
    SetFilePermissions(path, mode);
    if (recurse) {
        std::vector<FileInfo> entries;
        if (ListDirectoryInfo(path, entries)) {
            for (auto& e : entries) {
                std::string sub = path + (path.back() == '/' ? "" : "/") + e.name;
                if (e.is_dir) SetPermissionsRecursive(sub, mode, true);
                else SetFilePermissions(sub, mode);
            }
        }
    }
    return true;
}

bool VHDManager::SetOwnerRecursive(const std::string& path, uint32_t uid, uint32_t gid, bool recurse) {
    SetFileOwner(path, uid, gid);
    if (recurse) {
        std::vector<FileInfo> entries;
        if (ListDirectoryInfo(path, entries)) {
            for (auto& e : entries) {
                std::string sub = path + (path.back() == '/' ? "" : "/") + e.name;
                if (e.is_dir) SetOwnerRecursive(sub, uid, gid, true);
                else SetFileOwner(sub, uid, gid);
            }
        }
    }
    return true;
}
bool VHDManager::MakeDirectory(const std::string& p) { return ext4_dir_mk(p.c_str()) == EOK; }
void VHDManager::SetError(const std::string& e) { m_last_error = e; }
