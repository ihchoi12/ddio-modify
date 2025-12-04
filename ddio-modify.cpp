/*
 * Changing DDIO State
 */

#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include<pci/pci.h>
}

#include <sys/io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <string.h>

#define PCI_VENDOR_ID_INTEL 0x8086
#define SKX_PERFCTRLSTS_0 0x180
#define SKX_use_allocating_flow_wr_MASK 0x80
#define SKX_nosnoopopwren_MASK 0x8

// CPU generation detection
enum CpuGeneration {
    CPU_GEN_UNKNOWN = 0,
    CPU_GEN_SKYLAKE_SP,      // Skylake-SP (1st/2nd Gen Xeon Scalable) - DDIO register writable
    CPU_GEN_ICELAKE_SP,      // Ice Lake-SP (3rd Gen Xeon Scalable) - DDIO register locked
    CPU_GEN_SAPPHIRE_RAPIDS  // Sapphire Rapids (4th Gen) - DDIO register locked
};

/*
 * Find the proper pci device (i.e., PCIe Root Port) based on the nic device
 * For instance, if the NIC is located on 0000:3a:00.0 (i.e., BDF)
 * 0x3a is the nic_bus (B)
 * 0x00 is the nic_device (D)
 * 0x0  is the nic_function (F)
 */

struct pci_access *pacc;

// State file stored in /tmp (works across NFS mounts and different machines)
const char *state_file_path = "/tmp/.ddio_state.dat";

/*
 * Detect CPU generation from /proc/cpuinfo
 * Ice Lake-SP and newer have DDIO register locked by BIOS
 */
CpuGeneration detect_cpu_generation() {
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) return CPU_GEN_UNKNOWN;

    char line[256];
    int family = 0, model = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu family", 10) == 0) {
            sscanf(line, "cpu family : %d", &family);
        } else if (strncmp(line, "model", 5) == 0 && line[5] == '\t') {
            sscanf(line, "model : %d", &model);
            break;  // Got both values
        }
    }
    fclose(fp);

    // Intel Family 6:
    // Skylake-SP/Cascade Lake-SP: model 85 (0x55)
    // Ice Lake-SP: model 106 (0x6A)
    // Sapphire Rapids: model 143 (0x8F)
    if (family == 6) {
        if (model == 85) return CPU_GEN_SKYLAKE_SP;
        if (model == 106) return CPU_GEN_ICELAKE_SP;
        if (model == 143) return CPU_GEN_SAPPHIRE_RAPIDS;
    }

    return CPU_GEN_UNKNOWN;
}

const char* get_cpu_gen_name(CpuGeneration gen) {
    switch (gen) {
        case CPU_GEN_SKYLAKE_SP: return "Skylake-SP/Cascade Lake-SP";
        case CPU_GEN_ICELAKE_SP: return "Ice Lake-SP";
        case CPU_GEN_SAPPHIRE_RAPIDS: return "Sapphire Rapids";
        default: return "Unknown";
    }
}

void init_pci_access(void) {
    pacc = pci_alloc(); /* Get the pci_access structure */
    pci_init(pacc);         /* Initialize the PCI library */
    pci_scan_bus(pacc); /* We want to get the list of devices */
}

/*
 * Read MSR 0xc8b (LLC Ways Allocation for DDIO)
 * Returns: 0 on success, -1 on error
 */
int read_msr_llc_ways(uint32_t *value) {
    FILE *fp = popen("rdmsr -p 0 0xc8b 2>/dev/null", "r");
    if (!fp) {
        return -1;
    }

    char buf[64];
    if (fgets(buf, sizeof(buf), fp)) {
        if (sscanf(buf, "%x", value) == 1) {
            pclose(fp);
            return 0;
        }
    }
    pclose(fp);
    return -1;
}

/*
 * Write MSR 0xc8b (LLC Ways Allocation for DDIO)
 * Writes to all CPUs (-a flag)
 * Returns: 0 on success, -1 on error
 */
int write_msr_llc_ways(uint32_t value) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wrmsr -a 0xc8b 0x%x 2>/dev/null", value);
    int ret = system(cmd);
    return (ret == 0) ? 0 : -1;
}

/*
 * Detect number of LLC (L3) ways from sysfs
 * Returns: number of ways, or 11 as default if detection fails
 */
int detect_llc_ways() {
    FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cache/index3/ways_of_associativity", "r");
    if (!fp) {
        return 11;  // Default fallback
    }

    int ways = 11;
    if (fscanf(fp, "%d", &ways) != 1) {
        ways = 11;
    }
    fclose(fp);
    return ways;
}

/*
 * Get valid LLC ways range for this system
 * min_val: minimum value (2 ways enabled - bits for ways-1 and ways-2)
 * max_val: maximum value (all ways enabled)
 */
void get_llc_ways_range(int total_ways, uint32_t *min_val, uint32_t *max_val) {
    // Maximum: all ways enabled (e.g., 11 ways = 0x7ff, 12 ways = 0xfff)
    *max_val = (1U << total_ways) - 1;

    // Minimum: only 2 ways enabled (top 2 bits)
    // For 11 ways: 0x600 (bits 9,10), for 12 ways: 0xc00 (bits 10,11)
    *min_val = (0x3U << (total_ways - 2));
}

/*
 * Count number of set bits in LLC ways value
 */
int count_llc_ways_bits(uint32_t value, int total_ways) {
    uint32_t mask = (1U << total_ways) - 1;
    return __builtin_popcount(value & mask);
}

static int search_count = 0;

struct pci_dev *
find_ddio_device(uint8_t nic_bus, int verbose) {
    struct pci_dev *dev;
    char namebuf[1024];
    uint8_t parent_bus = nic_bus - 1;

    search_count++;

    if (verbose) {
        printf("\n[SEARCH #%d] Looking for PCIe Root Port for NIC on bus 0x%02x...\n",
               search_count, nic_bus);
        printf("[STRATEGY] Searching for PCI Bridge (class 0x0604) on parent bus 0x%02x\n",
               parent_bus);
    }

    // STEP 1: Search for PCIe Root Port (PCI Bridge class 0x0604) on parent bus
    for (dev = pacc->devices; dev; dev = dev->next) {
        pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_NUMA_NODE | PCI_FILL_PHYS_SLOT | PCI_FILL_CLASS);

        // Log all devices on parent bus for debugging
        if (verbose && dev->bus == parent_bus) {
            const char *device_name = pci_lookup_name(pacc, namebuf, sizeof(namebuf),
                                                      PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
            printf("[DEBUG] Parent bus device: %02x:%02x.%x - Class: 0x%04x - %s\n",
                   dev->bus, dev->dev, dev->func, dev->device_class, device_name);
        }

        // Look for PCI Bridge (class 0x0604) on parent bus
        if (dev->bus == parent_bus && dev->device_class == 0x0604) {
            if (verbose) {
                const char *device_name = pci_lookup_name(pacc, namebuf, sizeof(namebuf),
                                                          PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
                printf("\n[FOUND] ✓ PCIe Root Port: %02x:%02x.%x\n", dev->bus, dev->dev, dev->func);
                printf("        Device Class: 0x%04x (PCI Bridge/Root Port)\n", dev->device_class);
                printf("        Device Name: %s\n", device_name);
                printf("        ✓ CORRECT: This is a PCIe Root Port!\n");
                printf("        ✓ DDIO register (0x180) should be accessible here!\n");
            }
            return dev;
        }
    }

    // STEP 2: Fallback - try old method but with strong warnings
    if (verbose) {
        printf("\n[WARNING] Could not find PCI Bridge on parent bus 0x%02x\n", parent_bus);
        printf("[WARNING] Trying fallback: searching for %02x:00.0...\n", nic_bus);
    }

    for (dev = pacc->devices; dev; dev = dev->next) {
        pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_NUMA_NODE | PCI_FILL_PHYS_SLOT | PCI_FILL_CLASS);

        if (dev->func == 0 && dev->dev == 0 && dev->bus == nic_bus) {
            if (verbose) {
                const char *device_name = pci_lookup_name(pacc, namebuf, sizeof(namebuf),
                                                          PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
                printf("\n[FALLBACK] Found device: %02x:%02x.%x\n", dev->bus, dev->dev, dev->func);
                printf("           Device Class: 0x%04x", dev->device_class);

                // Decode device class
                uint16_t class_code = dev->device_class >> 8;
                uint16_t subclass = dev->device_class & 0xFF;

                if (class_code == 0x02) {
                    printf(" (Network Controller)\n");
                } else if (class_code == 0x06) {
                    if (subclass == 0x04) {
                        printf(" (PCI Bridge/Root Port)\n");
                    } else {
                        printf(" (PCI Bridge - subclass 0x%02x)\n", subclass);
                    }
                } else if (class_code == 0x08) {
                    printf(" (System Peripheral - IOMMU/VT-d)\n");
                } else {
                    printf(" (Class 0x%02x)\n", class_code);
                }

                printf("           Device Name: %s\n", device_name);

                // Validate device type
                if (class_code == 0x06 && subclass == 0x04) {
                    printf("           ✓ This is a PCI Bridge/Root Port\n");
                    printf("           ✓ DDIO operations should work\n");
                } else if (class_code == 0x02) {
                    printf("           ✗ CRITICAL: This is the NIC itself!\n");
                    printf("           ✗ DDIO register is NOT in the NIC!\n");
                    printf("           ✗ Register writes will FAIL!\n");
                } else if (class_code == 0x08) {
                    printf("           ✗ CRITICAL: This is IOMMU/VT-d!\n");
                    printf("           ✗ DDIO register is NOT here!\n");
                    printf("           ✗ Register writes will FAIL!\n");
                } else {
                    printf("           ✗ WARNING: Unexpected device class 0x%02x/0x%02x\n", class_code, subclass);
                    printf("           ✗ Expected: Class 0x06 Subclass 0x04\n");
                }
            }

            return dev;
        }
    }

    printf("\n[ERROR] Could not find any suitable device for NIC bus 0x%02x\n", nic_bus);
    return NULL;
}

/*
 * perfctrlsts_0
 * bit 3: NoSnoopOpWrEn -> Should be 1b
 * bit 7: Use_Allocating_Flow_Wr -> Should be 0b
 * Check p. 68 of IntelÂ® XeonÂ® Processor Scalable Family
 * Datasheet, Volume Two: Registers
 * May 2019
 * link: https://cdrdv2-public.intel.com/614073/614073_Intel%C2%AE%20Xeon%C2%AE%20Processor%20Scalable%20Fam_v005.pdf
 */
int ddio_status(uint8_t nic_bus) {
    uint32_t val;
    if (!pacc)
        init_pci_access();

    struct pci_dev *dev = find_ddio_device(nic_bus, 0);  // quiet mode
    if (!dev) {
        printf("No device found!\n");
        exit(1);
    }
    val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
    printf("perfctrlsts_0 val: 0x%" PRIx32 "\n", val);
    printf("NoSnoopOpWrEn val: 0x%" PRIx32 "\n", val & SKX_nosnoopopwren_MASK);
    printf("Use_Allocating_Flow_Wr val: 0x%" PRIx32 "\n", val & SKX_use_allocating_flow_wr_MASK);
    if (val & SKX_use_allocating_flow_wr_MASK)
        return 1;
    else
        return 0;
}

void ddio_enable(uint8_t nic_bus) {
    uint32_t val, val_after;
    if (!pacc)
        init_pci_access();

    if (!ddio_status(nic_bus)) {
        struct pci_dev *dev = find_ddio_device(nic_bus, 0);  // quiet mode
        if (!dev) {
            printf("No device found!\n");
            exit(1);
        }

        printf("\n[ENABLE] Attempting to enable DDIO...\n");
        val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[ENABLE] Register 0x180 BEFORE write: 0x%08x\n", val);
        printf("[ENABLE]   - bit 7 (DDIO): %s\n", (val & 0x80) ? "ON" : "OFF");

        pci_write_long(dev, SKX_PERFCTRLSTS_0, val | SKX_use_allocating_flow_wr_MASK);

        val_after = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[ENABLE] Register 0x180 AFTER write:  0x%08x\n", val_after);
        printf("[ENABLE]   - bit 7 (DDIO): %s\n", (val_after & 0x80) ? "ON" : "OFF");

        if ((val_after & 0x80) == 0) {
            printf("[ENABLE] ✗ FAILED: Register write did not take effect!\n");
            printf("[ENABLE]    This likely means we're writing to the wrong device.\n");
        } else {
            printf("[ENABLE] ✓ SUCCESS: DDIO is now enabled!\n");
        }
    } else {
        printf("DDIO was already enabled!\n");
    }
}

void ddio_disable(uint8_t nic_bus) {
    uint32_t val, val_after;
    if (!pacc)
        init_pci_access();

    if (ddio_status(nic_bus)) {
        struct pci_dev *dev = find_ddio_device(nic_bus, 0);  // quiet mode
        if (!dev) {
            printf("No device found!\n");
            exit(1);
        }

        printf("\n[DISABLE] Attempting to disable DDIO...\n");
        val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[DISABLE] Register 0x180 BEFORE write: 0x%08x\n", val);
        printf("[DISABLE]   - bit 7 (DDIO): %s\n", (val & 0x80) ? "ON" : "OFF");

        pci_write_long(dev, SKX_PERFCTRLSTS_0, val & (~SKX_use_allocating_flow_wr_MASK));

        val_after = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[DISABLE] Register 0x180 AFTER write:  0x%08x\n", val_after);
        printf("[DISABLE]   - bit 7 (DDIO): %s\n", (val_after & 0x80) ? "ON" : "OFF");

        if ((val_after & 0x80) != 0) {
            printf("[DISABLE] ✗ FAILED: Register write did not take effect!\n");
            printf("[DISABLE]    This likely means we're writing to the wrong device.\n");
        } else {
            printf("[DISABLE] ✓ SUCCESS: DDIO is now disabled!\n");
        }
    } else {
        printf("DDIO was already disabled\n");
    }
}

void print_dev_info(struct pci_dev *dev) {
    if (!dev) {
        printf("No device found!\n");
        exit(1);
    }
    unsigned int c;
    char namebuf[1024], *name;
    printf("========================\n");
    printf("%04x:%02x:%02x.%d vendor=%04x device=%04x class=%04x irq=%d (pin %d) base0=%lx \n",
           dev->domain, dev->bus, dev->dev, dev->func, dev->vendor_id, dev->device_id,
           dev->device_class, dev->irq, c, (long) dev->base_addr[0]);
    name = pci_lookup_name(pacc, namebuf, sizeof(namebuf), PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
    printf(" (%s)\n", name);
    printf("========================\n");
}

/*
 * Save current DDIO state to file
 */
void save_current_state(uint8_t nic_bus, struct pci_dev *dev, uint32_t reg_val) {
    FILE *fp = fopen(state_file_path, "w");
    if (!fp) {
        printf("[WARNING] Could not save state to file: %s\n", state_file_path);
        return;
    }

    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(fp, "# DDIO State Snapshot\n");
    fprintf(fp, "timestamp=%s\n", timestamp);
    fprintf(fp, "nic_bus=0x%02x\n", nic_bus);
    fprintf(fp, "root_port=%02x:%02x.%x\n", dev->bus, dev->dev, dev->func);
    fprintf(fp, "register_value=0x%08x\n", reg_val);
    fprintf(fp, "ddio_state=%s\n", (reg_val & 0x80) ? "enabled" : "disabled");

    // Save LLC Ways (MSR 0xc8b)
    uint32_t llc_ways;
    if (read_msr_llc_ways(&llc_ways) == 0) {
        fprintf(fp, "llc_ways=0x%03x\n", llc_ways);
    }

    fclose(fp);

    // If running as root (via sudo), change file ownership to original user
    if (getuid() == 0) {
        const char *sudo_uid = getenv("SUDO_UID");
        const char *sudo_gid = getenv("SUDO_GID");
        if (sudo_uid && sudo_gid) {
            uid_t uid = atoi(sudo_uid);
            gid_t gid = atoi(sudo_gid);
            if (chown(state_file_path, uid, gid) == 0) {
                chmod(state_file_path, 0644);  // rw-r--r--
            }
        }
    }

    printf("[INFO] Current state saved to %s\n", state_file_path);
}

/*
 * Load saved DDIO state from file
 * Returns: 0 on success, -1 on error
 */
int load_saved_state(uint8_t *nic_bus, uint32_t *reg_val, uint32_t *llc_ways) {
    FILE *fp = fopen(state_file_path, "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    int found_nic_bus = 0, found_reg_val = 0;
    *llc_ways = 0;  // Optional field

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        if (sscanf(line, "nic_bus=0x%hhx", nic_bus) == 1) {
            found_nic_bus = 1;
        } else if (sscanf(line, "register_value=0x%x", reg_val) == 1) {
            found_reg_val = 1;
        } else if (sscanf(line, "llc_ways=0x%x", llc_ways) == 1) {
            // Optional: LLC Ways
        }
    }

    fclose(fp);

    if (found_nic_bus && found_reg_val) {
        return 0;
    }
    return -1;
}

/*
 * Display interactive menu
 */
void show_menu(int has_saved_state, int total_llc_ways, uint32_t min_llc, uint32_t max_llc, int ddio_locked) {
    printf("\n");
    printf("╔════════════════════════════════════════════╗\n");
    printf("║         DDIO Control Menu                  ║\n");
    printf("╚════════════════════════════════════════════╝\n");
    printf("\n");
    if (has_saved_state) {
        printf("  1. Restore to saved default state\n");
    } else {
        printf("  1. Restore to saved default state (no saved state)\n");
    }
    if (ddio_locked) {
        printf("  2. Enable DDIO  [LOCKED - use BIOS]\n");
        printf("  3. Disable DDIO [LOCKED - use BIOS]\n");
    } else {
        printf("  2. Enable DDIO\n");
        printf("  3. Disable DDIO\n");
    }
    printf("  4. Set LLC Ways (valid: 0x%03x-0x%03x, %d-%d ways)\n",
           min_llc, max_llc, 2, total_llc_ways);
    printf("  0. Exit\n");
    printf("\n");
    printf("Your choice: ");
    fflush(stdout);
}

/**
 * Usage: sudo ./ddio_modify [nic_bus_hex]
 * Example: sudo ./ddio_modify 0xb3
 * If no argument provided, uses default 0xb3
 */

int main(int argc, char *argv[]) {
    uint8_t nic_bus = 0xb3;  // default

    // Parse NIC bus from command line if provided
    if (argc > 1) {
        sscanf(argv[1], "0x%hhx", &nic_bus);
    }

    init_pci_access();

    printf("\n");
    printf("════════════════════════════════════════════════════════\n");
    printf("              DDIO Control Tool\n");
    printf("════════════════════════════════════════════════════════\n");

    // Find PCIe Root Port
    struct pci_dev *dev = find_ddio_device(nic_bus, 0);
    if (!dev) {
        printf("[ERROR] Could not find PCIe Root Port!\n");
        pci_cleanup(pacc);
        return 1;
    }

    printf("Target NIC Bus:  0x%02x\n", nic_bus);
    printf("PCIe Root Port:  %02x:%02x.%x\n", dev->bus, dev->dev, dev->func);

    // Detect CPU generation
    CpuGeneration cpu_gen = detect_cpu_generation();
    printf("CPU Generation:  %s\n", get_cpu_gen_name(cpu_gen));

    // Warn about DDIO register lock on newer CPUs
    int ddio_register_locked = 0;
    if (cpu_gen == CPU_GEN_ICELAKE_SP || cpu_gen == CPU_GEN_SAPPHIRE_RAPIDS) {
        ddio_register_locked = 1;
        printf("\n");
        printf("════════════════════════════════════════════════════════\n");
        printf("⚠  WARNING: DDIO REGISTER LOCKED ON THIS CPU\n");
        printf("════════════════════════════════════════════════════════\n");
        printf("On %s, the DDIO enable/disable register (0x180) is\n", get_cpu_gen_name(cpu_gen));
        printf("locked by BIOS and cannot be modified at runtime.\n");
        printf("\n");
        printf("Options:\n");
        printf("  • Check BIOS for 'DDIO' or 'Data Direct I/O' setting\n");
        printf("  • LLC Ways (MSR 0xc8b) can still be modified\n");
        printf("════════════════════════════════════════════════════════\n");
    }

    // Detect LLC ways for this system
    int total_llc_ways = detect_llc_ways();
    uint32_t min_llc_val, max_llc_val;
    get_llc_ways_range(total_llc_ways, &min_llc_val, &max_llc_val);
    printf("System LLC Ways: %d (valid range: 0x%03x-0x%03x)\n",
           total_llc_ways, min_llc_val, max_llc_val);
    printf("\n");

    // Check current DDIO state
    uint32_t current_reg = pci_read_long(dev, SKX_PERFCTRLSTS_0);
    int current_state = (current_reg & SKX_use_allocating_flow_wr_MASK) ? 1 : 0;

    // Save system default state on first run only
    uint8_t test_nic_bus;
    uint32_t test_reg;
    uint32_t saved_llc_ways;
    int has_saved_state = (load_saved_state(&test_nic_bus, &test_reg, &saved_llc_ways) == 0);
    int is_first_run = !has_saved_state;  // Remember if this is first run

    if (!has_saved_state) {
        // First run: save current state as default
        save_current_state(nic_bus, dev, current_reg);
        has_saved_state = 1;
        test_reg = current_reg;  // Update test_reg for summary
        printf("[INFO] First run: Current state saved as system default.\n");
        printf("[INFO] To reset, delete: %s\n", state_file_path);
    }

    printf("\n");
    printf("════════════════════════════════════════════════════════\n");
    printf("                   Status Summary\n");
    printf("════════════════════════════════════════════════════════\n");

    if (ddio_register_locked) {
        // On Ice Lake-SP+, register 0x180 doesn't reliably indicate DDIO status
        printf("Register 0x180:  0x%08x (bit 7 = %d)\n", current_reg, current_state);
        printf("⚠  NOTE: On %s, this register does NOT indicate\n", get_cpu_gen_name(cpu_gen));
        printf("   actual DDIO status. DDIO is likely ENABLED by BIOS.\n");
        printf("   Check 'dmesg | grep dca' - if DCA service is running,\n");
        printf("   DDIO is enabled. PCM miss rate confirms DDIO status.\n");
    } else if (is_first_run) {
        printf("System Default:  JUST SAVED (0x%08x)\n", current_reg);
        printf("Current State:   %s (0x%08x)\n",
               current_state ? "ENABLED " : "DISABLED", current_reg);
        printf("Status:          ✓ INITIAL STATE\n");
    } else {
        int saved_state = (test_reg & SKX_use_allocating_flow_wr_MASK) ? 1 : 0;
        const char *default_str = saved_state ? "ENABLED " : "DISABLED";
        const char *current_str = current_state ? "ENABLED " : "DISABLED";

        printf("System Default:  %s (0x%08x)\n", default_str, test_reg);
        printf("Current State:   %s (0x%08x)\n", current_str, current_reg);

        if (saved_state == current_state) {
            printf("Status:          ✓ MATCHES default\n");
        } else {
            printf("Status:          ⚠ CHANGED from default\n");
        }
    }

    // Show LLC Ways allocation (MSR 0xc8b)
    uint32_t current_llc_ways;
    if (read_msr_llc_ways(&current_llc_ways) == 0) {
        int current_bits = count_llc_ways_bits(current_llc_ways, total_llc_ways);
        printf("────────────────────────────────────────────────────────\n");

        if (is_first_run) {
            printf("LLC Ways (0xc8b):  0x%03x (%d ways, CPU 0)\n",
                   current_llc_ways, current_bits);
            printf("                   ✓ INITIAL STATE\n");
        } else if (saved_llc_ways > 0) {
            int saved_bits = count_llc_ways_bits(saved_llc_ways, total_llc_ways);
            printf("System Default:    0x%03x (%d ways)\n", saved_llc_ways, saved_bits);
            printf("Current State:     0x%03x (%d ways)\n", current_llc_ways, current_bits);

            if (saved_llc_ways == current_llc_ways) {
                printf("Status:            ✓ MATCHES default\n");
            } else {
                printf("Status:            ⚠ CHANGED from default\n");
            }
        } else {
            // Old state file without LLC Ways
            printf("LLC Ways (0xc8b):  0x%03x (%d ways, CPU 0)\n",
                   current_llc_ways, current_bits);
            printf("                   (no saved default)\n");
        }

        if (current_llc_ways >= min_llc_val && current_llc_ways <= max_llc_val) {
            printf("Valid Range:       0x%03x-0x%03x (%d-%d ways) ✓\n",
                   min_llc_val, max_llc_val, 2, total_llc_ways);
        } else {
            printf("Valid Range:       0x%03x-0x%03x (%d-%d ways)\n",
                   min_llc_val, max_llc_val, 2, total_llc_ways);
            printf("                   ⚠ Current value outside valid range\n");
        }
    }
    printf("════════════════════════════════════════════════════════\n");

    // STEP 5: Show menu and get user choice
    show_menu(has_saved_state, total_llc_ways, min_llc_val, max_llc_val, ddio_register_locked);

    int choice;
    if (scanf("%d", &choice) != 1) {
        printf("[ERROR] Invalid input\n");
        pci_cleanup(pacc);
        return 1;
    }

    printf("\n");
    printf("════════════════════════════════════════════════════════\n");

    // Execute user choice
    switch (choice) {
        case 1: {  // Restore to saved default
            printf("[ACTION] Restoring to saved default state...\n\n");
            uint8_t saved_nic_bus;
            uint32_t saved_reg;
            uint32_t saved_llc;

            if (load_saved_state(&saved_nic_bus, &saved_reg, &saved_llc) != 0) {
                printf("[ERROR] No saved state found in %s\n", state_file_path);
                printf("        Cannot restore. Exiting...\n");
                break;
            }

            int saved_state = (saved_reg & SKX_use_allocating_flow_wr_MASK) ? 1 : 0;
            printf("[INFO] Saved PCI Register: 0x%08x (%s)\n",
                   saved_reg, saved_state ? "enabled" : "disabled");

            if (saved_llc > 0) {
                int saved_bits = count_llc_ways_bits(saved_llc, total_llc_ways);
                printf("[INFO] Saved LLC Ways:     0x%03x (%d ways)\n",
                       saved_llc, saved_bits);
            }
            printf("\n");

            // Restore PCI Register
            if (saved_state) {
                ddio_enable(nic_bus);
            } else {
                ddio_disable(nic_bus);
            }

            // Restore LLC Ways
            if (saved_llc > 0) {
                printf("\n[RESTORE] Setting LLC Ways to 0x%03x...\n", saved_llc);
                if (write_msr_llc_ways(saved_llc) == 0) {
                    printf("[RESTORE] ✓ LLC Ways restored successfully\n");
                } else {
                    printf("[RESTORE] ✗ Failed to restore LLC Ways\n");
                }
            }

            // Verify restoration
            uint32_t new_reg = pci_read_long(dev, SKX_PERFCTRLSTS_0);
            uint32_t new_llc;
            int llc_ok = (read_msr_llc_ways(&new_llc) == 0);

            printf("\n[VERIFY] Checking restoration...\n");
            if (new_reg == saved_reg) {
                printf("         PCI Register: ✓ MATCH (0x%08x)\n", new_reg);
            } else {
                printf("         PCI Register: ✗ MISMATCH\n");
                printf("           Expected: 0x%08x\n", saved_reg);
                printf("           Current:  0x%08x\n", new_reg);
            }

            if (saved_llc > 0 && llc_ok) {
                if (new_llc == saved_llc) {
                    printf("         LLC Ways:     ✓ MATCH (0x%03x)\n", new_llc);
                } else {
                    printf("         LLC Ways:     ✗ MISMATCH\n");
                    printf("           Expected: 0x%03x\n", saved_llc);
                    printf("           Current:  0x%03x\n", new_llc);
                }
            }

            if (new_reg == saved_reg && (!saved_llc || new_llc == saved_llc)) {
                printf("\n[SUCCESS] ✓ Fully restored to saved state!\n");
            } else {
                printf("\n[WARNING] ⚠ Partial restoration\n");
            }
            break;
        }

        case 2:  // Enable DDIO
            printf("[ACTION] Enabling DDIO...\n\n");
            if (ddio_register_locked) {
                printf("⚠  WARNING: DDIO register is LOCKED on this CPU!\n");
                printf("   This operation will likely fail.\n");
                printf("   To change DDIO state, use BIOS settings.\n\n");
            }
            ddio_enable(nic_bus);
            break;

        case 3:  // Disable DDIO
            printf("[ACTION] Disabling DDIO...\n\n");
            if (ddio_register_locked) {
                printf("⚠  WARNING: DDIO register is LOCKED on this CPU!\n");
                printf("   This operation will likely fail.\n");
                printf("   To change DDIO state, use BIOS settings.\n\n");
            }
            ddio_disable(nic_bus);
            break;

        case 4: {  // Set LLC Ways with user input
            printf("[ACTION] Set LLC Ways\n\n");
            printf("Valid range: 0x%03x - 0x%03x (%d - %d ways)\n",
                   min_llc_val, max_llc_val, 2, total_llc_ways);
            printf("\n");
            printf("Enter value (hex, e.g., 0x%03x): ", max_llc_val);
            fflush(stdout);

            uint32_t new_llc_val;
            if (scanf("%x", &new_llc_val) != 1) {
                printf("[ERROR] ✗ Invalid input\n");
                break;
            }

            // Validate range
            if (new_llc_val < min_llc_val || new_llc_val > max_llc_val) {
                printf("[ERROR] ✗ Value 0x%03x is outside valid range (0x%03x-0x%03x)\n",
                       new_llc_val, min_llc_val, max_llc_val);
                break;
            }

            int new_bits = count_llc_ways_bits(new_llc_val, total_llc_ways);
            printf("\n[ACTION] Setting LLC Ways to 0x%03x (%d ways)...\n\n", new_llc_val, new_bits);

            if (write_msr_llc_ways(new_llc_val) == 0) {
                printf("[SUCCESS] ✓ LLC Ways set to 0x%03x (%d ways)\n", new_llc_val, new_bits);

                // Verify
                uint32_t verify_llc;
                if (read_msr_llc_ways(&verify_llc) == 0) {
                    int verify_bits = count_llc_ways_bits(verify_llc, total_llc_ways);
                    printf("[VERIFY] Current LLC Ways: 0x%03x (%d ways)\n", verify_llc, verify_bits);
                }
            } else {
                printf("[ERROR] ✗ Failed to set LLC Ways\n");
            }
            break;
        }

        case 0:  // Exit
            printf("[INFO] Exiting without changes.\n");
            break;

        default:
            printf("[ERROR] Invalid choice: %d\n", choice);
            break;
    }

    printf("════════════════════════════════════════════════════════\n");
    printf("\n");

    pci_cleanup(pacc);
    return 0;
}