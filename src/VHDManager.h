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

#ifndef VHD_MANAGER_H
#define VHD_MANAGER_H

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>


struct ext4_blockdev;
struct ext4_blockdev_iface;


struct PartitionInfo {
    uint8_t  type;
    uint64_t offset;
    uint64_t size;
    bool     is_ext4;
};


struct VHDBlockDevice {
    HANDLE   hFile;
    uint64_t partition_offset;
    uint64_t partition_size;
    uint32_t block_size;
};

struct FileInfo {
    std::string name;
    bool is_dir;
    uint64_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
};

class VHDManager {
public:
    VHDManager();
    ~VHDManager();


    bool OpenVHD(const std::string& path);
    void CloseVHD();
    bool IsOpen() const { return m_hVHD != INVALID_HANDLE_VALUE; }
    std::string GetVHDPath() const { return m_vhd_path; }


    const std::vector<PartitionInfo>& GetPartitions() const { return m_partitions; }


    bool MountExt4Partition(int partition_index);
    void UnmountExt4();
    bool IsExt4Mounted() const { return m_ext4_mounted; }


    bool FileExists(const std::string& path);
    bool BackupFile(const std::string& source, const std::string& backup);
    bool DeleteRecursive(const std::string& path);
    bool CopyFileFromHost(const std::string& host_path, const std::string& ext4_path);
    bool CopyFileToHost(const std::string& ext4_path, const std::string& host_path);
    bool ExportRecursive(const std::string& ext4_path, const std::string& host_path);
    bool ImportRecursive(const std::string& host_path, const std::string& ext4_path);
    bool CopyInternal(const std::string& src_path, const std::string& dst_path);
    bool Rename(const std::string& old_path, const std::string& new_path);
    bool ListDirectoryInfo(const std::string& path, std::vector<FileInfo>& entries);
    bool SetFilePermissions(const std::string& path, uint32_t mode);
    bool SetFileOwner(const std::string& path, uint32_t uid, uint32_t gid);
    bool SetPermissionsRecursive(const std::string& path, uint32_t mode, bool recurse);
    bool SetOwnerRecursive(const std::string& path, uint32_t uid, uint32_t gid, bool recurse);
    bool MakeDirectory(const std::string& path);


    std::string GetLastError() const { return m_last_error; }

private:

    bool ReadVHDFooter();
    bool ParsePartitions();
    bool IsExt4Filesystem(uint64_t offset);
    void SetError(const std::string& error);


    static int BlockOpen(struct ext4_blockdev* bdev);
    static int BlockClose(struct ext4_blockdev* bdev);
    static int BlockRead(struct ext4_blockdev* bdev, void* buf,
                         uint64_t blk_id, uint32_t blk_cnt);
    static int BlockWrite(struct ext4_blockdev* bdev, const void* buf,
                          uint64_t blk_id, uint32_t blk_cnt);


    HANDLE m_hVHD;
    std::string m_vhd_path;
    std::string m_last_error;
    std::vector<PartitionInfo> m_partitions;


    struct ext4_blockdev* m_ext4_bdev;
    struct ext4_blockdev_iface* m_bdif;
    VHDBlockDevice m_block_device;
    bool m_ext4_mounted;
    int m_mounted_partition_index;


    HANDLE m_virtDiskHandle;
    bool m_isVirtDiskAttached;
};

#endif // VHD_MANAGER_H