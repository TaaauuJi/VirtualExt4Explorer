```

```

# Virtual Ext4 Explorer

**Virtual Ext4 Explorer** is a high-performance, standalone C++ utility designed to bridge the gap between Windows and Linux virtual environments. It provides a seamless, GUI to manage **ext4** filesystems encapsulated within various virtual disk formats.

---

## 🚀 Key Features

### 📁 Virtual Disk Support

- **VHD/VHDX:** Native support via Windows VirtDisk API. Supports fixed and dynamic allocation.
- **VDI (VirtualBox):** Custom parser for `.vdi` images, supporting both fixed and dynamic images.
- **Raw Images:** Ability to treat disks as raw block devices.
- **Drag-and-Drop Mounting:** Drag a virtual disk file directly onto the window to open and mount it.

### 🐧 ext4 Filesystem Mastery

- **Mounting:** Automatically detects and mounts ext4 partitions.
- **Navigation:** Deep directory traversal with a responsive "Back/Forward" history system.
- **Metadata Management:** View and modify Linux file **Permissions (Mode)**, **UID**, and **GID**. Supports **Recursive** application to entire folder trees and **Multi-selection** for batch updates.
- **Run as Administrator:** Automatically requests elevation to handle low-level disk mounting and VirtDisk APIs.

### 🛠 Advanced File Operations

- **Drag-and-Drop Import:** Drag any file or folder from Windows Explorer directly onto the app window to import it recursively into the active directory.
- **Recursive Import/Export:** Transfer entire directory trees between the host (Windows) and the guest (ext4) with a single click.
- **Internal Clipboard:** Full support for **Cut**, **Copy**, and **Paste** operations within the virtual disk.
- **Creation & Cleanup:** Create new files/folders and perform recursive deletions of non-empty directories.
- **Enhanced Visibility:** Fixed UI columns displaying **File Type** and **Formatted Size** (B, KB, MB, GB).

### 🖱 UX/UI

- **Shortcuts:** Supports Mouse Side Buttons (X1/X2) and `Alt + Arrow` keys for browser-like navigation.
- **Multi-Selection:** Standard `Ctrl + Click` and `Shift + Click` (range selection) support.
- **Visual Cues:** Color-coded entries:
  - `<span style="color:gold">`**Folders (Gold)**
  - `<span style="color:green">`**Executables (Green)**
  - `<span style="color:orange">`**Archives (Orange)**
  - `<span style="color:pink">`**Libraries (Pink)**

---

## 🛠 Technical Specifications

| Feature                   | Detail                                     |
| :------------------------ | :----------------------------------------- |
| **Framework**       | Dear ImGui (v1.89+ style)                  |
| **Backend**         | DirectX 11 / Win32                         |
| **Filesystem Core** | lwext4 (Modified for dynamic disk support) |
| **Binary Size**     | ~920 KB (Standalone EXE)                   |
| **Privileges**      | Administrator (UAC Manifest embedded)      |
| **Dependencies**    | None (Static CRT, Native Windows APIs)     |

---

## 📖 Usage Guide

### Opening a Disk

1. Click `Mount > Open Image...` and select your file, or drag and drop a `.vhd`, `.vhdx`, or `.vdi` file directly into the application window.
2. If the disk has multiple partitions, use `Mount > Partitions` to switch.

### Managing Permissions

1. Select one or more files/folders.
2. Right-click and select **Permissions**.
3. Edit the octal mode (e.g., `777` for full access) or the User/Group IDs.
4. Check **Recursive** if you want to apply changes to all items within selected folders.
5. Click **Apply**.

### Importing Files & Folders

You can import items from the host to the guest in two ways:

- **Using Drag-and-Drop:** Drag files and folders from Windows Explorer and drop them directly into the application window to recursively import them into the active directory.
- **Using the Toolbar:** Click **Import > Files...** or **Import > Folder...** to browse and select the files/folders you want to import.

---

## 🔨 Building from Source

### Requirements

- **Visual Studio 2022**
- **Windows SDK**

### Build Process

1. Open a terminal (PowerShell or Command Prompt).
2. Run `requirements.bat` to automatically download and configure the dependencies (Dear ImGui and lwext4).
3. Run `build.bat`.
4. The script will automatically locate your Visual Studio installation, compile the resources (manifest for Admin privileges), and build the standalone executable in the `build/` directory.

The script uses `/O1` optimization for minimum size and `/MT` for static runtime linking, ensuring the output is a single portable executable.

---

## 📜 License

This project is licensed under the **[GPLv3 License](LICENSE)**.

## 🤝 Credits

- [Dear ImGui](https://github.com/ocornut/imgui) - Graphical Interface
- [lwext4](https://github.com/gabr1el/lwext4) - ext4 implementation
