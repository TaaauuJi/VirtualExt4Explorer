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
#include <functional>
#include <mutex>

typedef std::function<void(uint64_t)> ProgressCallback;


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
    bool IsOpen() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return m_hVHD != INVALID_HANDLE_VALUE; }
    std::string GetVHDPath() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return m_vhd_path; }


    std::vector<PartitionInfo> GetPartitions() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return m_partitions; }


    bool MountExt4Partition(int partition_index);
    void UnmountExt4();
    bool IsExt4Mounted() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return m_ext4_mounted; }
    int GetMountedPartitionIndex() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return m_mounted_partition_index; }


    bool FileExists(const std::string& path);
    bool DeleteRecursive(const std::string& path);
    bool CopyFileFromHost(const std::string& host_path, const std::string& ext4_path, ProgressCallback cb = nullptr);
    bool CopyFileToHost(const std::string& ext4_path, const std::string& host_path, ProgressCallback cb = nullptr);
    bool ExportRecursive(const std::string& ext4_path, const std::string& host_path, ProgressCallback cb = nullptr);
    bool ImportRecursive(const std::string& host_path, const std::string& ext4_path, ProgressCallback cb = nullptr);
    uint64_t GetExt4SizeRecursive(const std::string& path);
    uint64_t GetHostSizeRecursive(const std::string& path);
    bool CopyInternal(const std::string& src_path, const std::string& dst_path);
    bool Rename(const std::string& old_path, const std::string& new_path);
    bool ListDirectoryInfo(const std::string& path, std::vector<FileInfo>& entries);
    bool SetFilePermissions(const std::string& path, uint32_t mode);
    bool SetFileOwner(const std::string& path, uint32_t uid, uint32_t gid);
    bool SetPermissionsRecursive(const std::string& path, uint32_t mode, bool recurse);
    bool SetOwnerRecursive(const std::string& path, uint32_t uid, uint32_t gid, bool recurse);
    bool MakeDirectory(const std::string& path);


    std::string GetLastError() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return m_last_error; }
    // Flush ext4 block cache + host file buffers so a copied file
    // survives remount / reopen. Must be called with m_mutex held
    // (or from a context that already holds it).
    bool FlushNoLock();

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

    // Serializes ALL lwext4 access. lwext4 has no internal OS locks
    // (os_locks == nullptr in this port), and BlockRead/BlockWrite touch
    // shared BAT state + a Win32 file handle, so concurrent calls from
    // the UI thread (Refresh/List) and background import/export threads
    // corrupt metadata and produce 0-byte / disappearing files.
    // Recursive because ImportRecursive/ExportRecursive/Set*Recursive
    // re-enter other locked methods, and Mount calls Unmount.
    mutable std::recursive_mutex m_mutex;
};

#endif // VHD_MANAGER_H