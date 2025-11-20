# DDIO Modify Project Analysis Document

## Project Overview

This project is a system-level tool to enable/disable the **DDIO (Data Direct I/O)** feature on Intel Xeon Scalable processors.

### What is DDIO?
- Technology that allows PCIe devices to write data directly to the CPU's Last Level Cache (LLC) instead of main memory
- Used for network performance optimization (reduced latency, improved bandwidth)
- References: [ddio-bench](https://github.com/aliireza/ddio-bench), [USENIX ATC'20 paper](https://www.usenix.org/conference/atc20/presentation/farshin)

## Project Structure

```
ddio-modify/
├── ddio-modify.cpp    # Main source code
├── CMakeLists.txt     # Build configuration
├── Readme.md          # User documentation
└── claude.md          # Project analysis document for AI assistant
```

## Technology Stack

### Programming Language
- C++ (C++17 standard)

### Key Dependencies
1. **libpci** (pciutils-dev)
   - PCI/PCIe device discovery and configuration register access
   - Header: `<pci/pci.h>`

2. **gflags**
   - Command-line argument parsing
   - Handles `--enable`, `--nic_bus` flags

3. **msr-tools** (runtime)
   - CPU Model-Specific Register access
   - For DDIO configuration verification (`rdmsr`, `wrmsr`)

### Build System
- CMake (minimum version 3.10)

## Code Structure and Key Functions

### File: ddio-modify.cpp:1-162

#### Core Constant Definitions
```cpp
#define PCI_VENDOR_ID_INTEL 0x8086
#define SKX_PERFCTRLSTS_0 0x180           // PCI configuration register offset
#define SKX_use_allocating_flow_wr_MASK 0x80  // bit 7: DDIO enable bit
#define SKX_nosnoopopwren_MASK 0x8        // bit 3: NoSnoopOpWrEn
```

#### Key Functions

1. **`init_pci_access()`** (ddio-modify.cpp:35-39)
   - Initialize PCI library
   - Setup global `pci_access` structure

2. **`find_ddio_device(uint8_t nic_bus)`** (ddio-modify.cpp:42-52)
   - Find PCIe Root Port based on NIC bus number
   - Search for root port in `bus:00.0` format from BDF (Bus:Device:Function)
   - Returns: `pci_dev*` or NULL

3. **`ddio_status(uint8_t nic_bus)`** (ddio-modify.cpp:63-81)
   - Check current DDIO status
   - Check bit 7 (`Use_Allocating_Flow_Wr`) of `PERFCTRLSTS_0` register
   - Returns: 1 (enabled) or 0 (disabled)

4. **`ddio_enable(uint8_t nic_bus)`** (ddio-modify.cpp:83-100)
   - Enable DDIO
   - Set bit 7 to 1 (OR operation)

5. **`ddio_disable(uint8_t nic_bus)`** (ddio-modify.cpp:102-119)
   - Disable DDIO
   - Set bit 7 to 0 (AND NOT operation)

6. **`print_dev_info(struct pci_dev *dev)`** (ddio-modify.cpp:121-135)
   - Print PCI device information (for debugging)

## Hardware Specifications

### Test Platform
| Component | Specification |
|-----------|--------------|
| CPU | Intel Xeon Silver 4214 |
| Motherboard | X11DPG-OT-CPU |
| Memory | DDR4 2400 x8 |

## Usage

### Build
```bash
mkdir build && cd build
cmake ..
make
```

### Execution
```bash
# Disable DDIO
sudo ./ddio_modify --enable=false --nic_bus=0x3a

# Enable DDIO
sudo ./ddio_modify --enable=true --nic_bus=0x3a
```

### How to Find NIC Bus Number
```bash
lspci -v -t  # View PCIe tree structure
```
- Find the PCIe Root Port bus number of the NIC
- Example: If NIC is at `3a:00.0`, use `--nic_bus=0x3a`

### Direct MSR Control (Alternative Method)
```bash
# Check current setting
sudo rdmsr -a 0xc8b

# Set LLC ways allocation (0x600 ~ 0x7ff)
sudo wrmsr -a 0xc8b 0x600
```

## Precautions

### 1. Permission Requirements
- **Root privileges required** (for writing PCI configuration registers)
- `/dev/mem` access needed

### 2. Hardware Compatibility
- Intel Xeon Scalable Family only (Skylake-SP and later)
- `PERFCTRLSTS_0` register offset may vary by platform
- Register address verification needed for different CPU models

### 3. Code Modification Considerations
- **Register Access**: Use `pci_read_long()`, `pci_write_long()`
- **Bit Masking**: Preserve bits other than bit 7 and bit 3
- **Error Handling**: `exit(1)` if PCI device not found

### 4. Build Issue Resolution
```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential cmake libpci-dev libgflags-dev msr-tools

# Load msr kernel module
sudo modprobe msr
```

## Extensibility

### Considerations for Additional Features
1. **Multi-NIC Support**
   - Currently handles only single NIC
   - Can add iteration logic for multiple NICs

2. **LLC Ways Tuning**
   - Add direct MSR `0xc8b` control feature
   - Set 0x600~0x7ff range with `--llc_ways` option

3. **Status Monitoring**
   - Periodic DDIO status checking feature
   - Performance counter reading (PMU integration)

4. **Support for Other CPU Models**
   - Dynamically select register offset after CPU ID check
   - Support Cascade Lake, Ice Lake, etc.

## Troubleshooting

### Common Errors
1. **"Could not find the proper PCIe root!"**
   - Check correct bus number with `lspci -v -t`
   - Verify NIC is actually connected to that bus

2. **"No device found!"**
   - PCI device does not exist
   - Permission issue or incorrect bus number

3. **Build Failure**
   - Check dependency package installation
   - Verify CMake version (>= 3.10)

## Reference Documentation

1. [Intel Xeon Scalable Family Datasheet Vol.2](https://cdrdv2-public.intel.com/614073/614073_Intel%C2%AE%20Xeon%C2%AE%20Processor%20Scalable%20Fam_v005.pdf)
   - Detailed description of PERFCTRLSTS_0 register (p.68)

2. [DDIO Benchmark](https://github.com/aliireza/ddio-bench)
   - DDIO performance measurement tool

3. [USENIX ATC'20 Paper](https://www.usenix.org/conference/atc20/presentation/farshin)
   - DDIO performance characteristics and optimization techniques

## Git Information

- **Current Branch**: master
- **Recent Commit**: `1a6a1c7 feat: add modify readme and c++ file`
- **Status**: clean (no modified files)
