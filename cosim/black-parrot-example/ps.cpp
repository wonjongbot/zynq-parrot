//
// this is an example of "host code" that can either run in cosim or on the PS
// we can use the same C host code and
// the API we provide abstracts away the
// communication plumbing differences.

#include <bitset>
#include <locale.h>
#include <pthread.h>
#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "ps.hpp"

#include "bsg_printing.h"
#include "bsg_tag_bitbang.h"
#include "bsg_zynq_pl.h"

#ifndef FREE_DRAM
#define FREE_DRAM 0
#endif

#ifndef ZERO_DRAM
#define ZERO_DRAM 0
#endif

#ifndef DRAM_ALLOCATE_SIZE_MB
#define DRAM_ALLOCATE_SIZE_MB 128
#endif
#define DRAM_ALLOCATE_SIZE (DRAM_ALLOCATE_SIZE_MB * 1024 * 1024)

#ifndef ZYNQ_PL_DEBUG
#define ZYNQ_PL_DEBUG 0
#endif

#ifndef BP_NCPUS
#define BP_NCPUS 1
#endif

// Helper functions
void nbf_load(bsg_zynq_pl *zpl, char *filename);
bool decode_bp_output(bsg_zynq_pl *zpl, long data);

// Globals
std::queue<int> getchar_queue;
std::bitset<BP_NCPUS> done_vec;

void *monitor(void *vargp) {
    char c;
    while (1) {
        c = getchar();
        if (c != -1)
            getchar_queue.push(c);
    }
    bsg_pr_info("Exiting from pthread\n");

    return NULL;
}

inline uint64_t get_counter_64(bsg_zynq_pl *zpl, uint64_t addr) {
    uint64_t val;
    do {
        uint64_t val_hi = zpl->shell_read(addr + 4);
        uint64_t val_lo = zpl->shell_read(addr + 0);
        uint64_t val_hi2 = zpl->shell_read(addr + 4);
        if (val_hi == val_hi2) {
            val = val_hi << 32;
            val += val_lo;
            return val;
        } else
            bsg_pr_err("ps.cpp: timer wrapover!\n");
    } while (1);
}

int ps_main(int argc, char **argv) {

    bsg_zynq_pl *zpl = new bsg_zynq_pl(argc, argv);

    long data;
    long val1 = 0x1;
    long val2 = 0x0;
    long mask1 = 0xf;
    long mask2 = 0xf;

    pthread_t thread_id;

    int32_t val;
    bsg_pr_info("ps.cpp: reading four base registers\n");
    bsg_pr_info(
        "ps.cpp: reset(lo)=%d, bitbang=%d, dram_init=%d, dram_base=%d\n",
        zpl->shell_read(GP0_RD_CSR_SYS_RESETN),
        zpl->shell_read(GP0_RD_CSR_TAG_BITBANG),
        zpl->shell_read(GP0_RD_CSR_DRAM_INITED),
        val = zpl->shell_read(GP0_RD_CSR_DRAM_BASE));

    bsg_pr_info("ps.cpp: attempting to write and read register 0x8\n");

    zpl->shell_write(GP0_WR_CSR_DRAM_BASE, 0xDEADBEEF, mask1);
    assert((zpl->shell_read(GP0_RD_CSR_DRAM_BASE) == (0xDEADBEEF)));
    zpl->shell_write(GP0_WR_CSR_DRAM_BASE, val, mask1);
    assert((zpl->shell_read(GP0_RD_CSR_DRAM_BASE) == (val)));

    bsg_pr_info(
        "ps.cpp: successfully wrote and read registers in bsg_zynq_shell "
        "(verified ARM GP0 connection)\n");

    bsg_tag_bitbang *btb = new bsg_tag_bitbang(zpl, GP0_WR_CSR_TAG_BITBANG,
                                               TAG_NUM_CLIENTS, TAG_MAX_LEN);
    bsg_tag_client *pl_reset_client =
        new bsg_tag_client(TAG_CLIENT_PL_RESET_ID, TAG_CLIENT_PL_RESET_WIDTH);
    bsg_tag_client *wd_reset_client =
        new bsg_tag_client(TAG_CLIENT_WD_RESET_ID, TAG_CLIENT_WD_RESET_WIDTH);

    // Reset the bsg tag master
    btb->reset_master();
    // Reset bsg client0
    btb->reset_client(pl_reset_client);
    // Reset bsg client1
    btb->reset_client(wd_reset_client);
    // Set bsg client0 to 1 (assert BP reset)
    btb->set_client(pl_reset_client, 0x1);
    // Set bsg client1 to 1 (assert WD reset)
    btb->set_client(wd_reset_client, 0x1);
    // Set bsg client0 to 0 (deassert BP reset)
    btb->set_client(pl_reset_client, 0x0);

    // We need some additional toggles for data to propagate through
    btb->idle(50);
    // Deassert the active-low system reset as we finish initializing the whole
    // system
    zpl->shell_write(GP0_RD_CSR_SYS_RESETN, 0x1, 0xF);

    unsigned long phys_ptr;
    volatile int *buf;
    data = zpl->shell_read(GP0_RD_CSR_DRAM_INITED);
    if (data == 0) {
        bsg_pr_info(
            "ps.cpp: CSRs do not contain a DRAM base pointer; calling allocate "
            "dram with size %ld\n",
            (long)DRAM_ALLOCATE_SIZE);
        buf = (volatile int *)zpl->allocate_dram(DRAM_ALLOCATE_SIZE, &phys_ptr);
        bsg_pr_info("ps.cpp: received %p (phys = %lx)\n", buf, phys_ptr);
        zpl->shell_write(GP0_WR_CSR_DRAM_BASE, phys_ptr, mask1);
        assert((zpl->shell_read(GP0_RD_CSR_DRAM_BASE) == (int32_t)(phys_ptr)));
        bsg_pr_info("ps.cpp: wrote and verified base register\n");
        zpl->shell_write(GP0_WR_CSR_DRAM_INITED, 0x1, mask2);
        assert(zpl->shell_read(GP0_RD_CSR_DRAM_INITED) == 1);
    } else {
        bsg_pr_info("ps.cpp: reusing dram base pointer %x\n",
                    zpl->shell_read(GP0_RD_CSR_DRAM_BASE));
    }

    int outer = 1024 / 4;

    if (argc == 1) {
        bsg_pr_warn(
            "No nbf file specified, sleeping for 2^31 seconds (this will hold "
            "onto allocated DRAM)\n");
        sleep(1U << 31);
        delete zpl;
    }

    bsg_pr_info("ps.cpp: attempting to read mtime reg in BP CFG space, should "
                "increase monotonically  (testing ARM GP1 connections)\n");

    for (int q = 0; q < 10; q++) {
        int z = get_counter_64(zpl, GP1_CSR_BASE_ADDR + 0x30bff8);
        bsg_pr_dbg_ps("ps.cpp: %d-%d\n", q, z);
    }

    bsg_pr_info(
        "ps.cpp: attempting to read and write mtime reg in BP CFG space "
        "(testing ARM GP1 connections)\n");

    bsg_pr_info("ps.cpp: reading mtimecmp\n");
    int y = zpl->shell_read(GP1_CSR_BASE_ADDR + 0x304000);

    bsg_pr_info("ps.cpp: writing mtimecmp\n");
    zpl->shell_write(GP1_CSR_BASE_ADDR + 0x304000, y + 1, mask1);

    bsg_pr_info("ps.cpp: reading mtimecmp\n");
    assert(zpl->shell_read(GP1_CSR_BASE_ADDR + 0x304000) == y + 1);

#ifdef DRAM_TEST

    long num_times = DRAM_ALLOCATE_SIZE / 32768;
    bsg_pr_info(
        "ps.cpp: attempting to write L2 %ld times over %ld MB (testing ARM GP1 "
        "and HP0 connections)\n",
        num_times * outer, (long)((DRAM_ALLOCATE_SIZE) >> 20));
    zpl->shell_write(GP1_DRAM_BASE_ADDR, 0x12345678, mask1);

    for (int s = 0; s < outer; s++)
        for (int t = 0; t < num_times; t++) {
            zpl->shell_write(GP1_DRAM_BASE_ADDR + 32768 * t + s * 4,
                             0x1ADACACA + t + s, mask1);
        }
    bsg_pr_info("ps.cpp: finished write L2 %ld times over %ld MB\n",
                num_times * outer, (long)((DRAM_ALLOCATE_SIZE) >> 20));

    int mismatches = 0;
    int matches = 0;

#ifdef ZYNQ
    for (int s = 0; s < outer; s++)
        for (int t = 0; t < num_times; t++)
            if (buf[(32768 * t + s * 4) / 4] == 0x1ADACACA + t + s)
                matches++;
            else
                mismatches++;

    bsg_pr_info("ps.cpp: DIRECT access from ARM to DDR (some L1/L2 coherence "
                "mismatches expected) %d matches, %d mismatches, %f\n",
                matches, mismatches,
                ((float)matches) / (float)(mismatches + matches));
#endif

    bsg_pr_info(
        "ps.cpp: attempting to read L2 %ld times over %ld MB (testing ARM GP1 "
        "and HP0 connections)\n",
        num_times * outer, (long)((DRAM_ALLOCATE_SIZE) >> 20));
    for (int s = 0; s < outer; s++)
        for (int t = 0; t < num_times; t++)
            if (zpl->shell_read(GP1_DRAM_BASE_ADDR + 32768 * t + s * 4) ==
                0x1ADACACA + t + s)
                matches++;
            else
                mismatches++;

    bsg_pr_info("ps.cpp: READ access through BP (some L1 coherence mismatch "
                "expected): %d matches, %d mismatches, %f\n",
                matches, mismatches,
                ((float)matches) / (float)(mismatches + matches));

#endif // DRAM_TEST

    // Must zero DRAM for FPGA Linux boot, because opensbi payload mode
    //   obliterates the section names of the payload (Linux)
    if (ZERO_DRAM) {
        bsg_pr_info("ps.cpp: Zero-ing DRAM (%d bytes)\n",
                    (int)DRAM_ALLOCATE_SIZE);
        for (int i = 0; i < DRAM_ALLOCATE_SIZE; i += 4) {
            //if (i % (1024 * 1024) == 0)
            if (i % (1024) == 0)
                bsg_pr_info("ps.cpp: (%d) zero-d %d MB\n", i, i / (1024 * 1024));
            zpl->shell_write(gp1_addr_base + i, 0x0, mask1);
        }
    }

    bsg_pr_info("ps.cpp: beginning nbf load\n");
    nbf_load(zpl, argv[1]);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    unsigned long long minstret_start = get_counter_64(zpl, GP0_RD_MINSTRET);
    unsigned long long mtime_start =
        get_counter_64(zpl, GP1_CSR_BASE_ADDR + 0x30bff8);
    bsg_pr_info("ps.cpp: finished nbf load\n");

    // Set bsg client1 to 0 (deassert WD reset)
    btb->set_client(wd_reset_client, 0x1);
    bsg_pr_info("ps.cpp: starting watchdog\n");
    // We need some additional toggles for data to propagate through
    btb->idle(50);
    zpl->start();

    bsg_pr_info("ps.cpp: Starting scan thread\n");
    pthread_create(&thread_id, NULL, monitor, NULL);

    bsg_pr_info("ps.cpp: Starting i/o polling thread\n");
    while (1) {
        // keep reading as long as there is data
        if (zpl->shell_read(GP0_RD_PL2PS_FIFO_CTRS) != 0) {
            decode_bp_output(zpl, zpl->shell_read(GP0_RD_PL2PS_FIFO_DATA));
        }
        // break loop when all cores done
        if (done_vec.all()) {
            break;
        }
    }

    // Set bsg client1 to 1 (assert WD reset)
    btb->set_client(wd_reset_client, 0x1);
    bsg_pr_info("ps.cpp: stopping watchdog\n");
    // We need some additional toggles for data to propagate through
    btb->idle(50);
    zpl->stop();

    unsigned long long minstret_stop = get_counter_64(zpl, GP0_RD_MINSTRET);
    unsigned long long mtime_stop =
        get_counter_64(zpl, GP1_CSR_BASE_ADDR + 0x30bff8);
    // test delay for reading counter
    unsigned long long counter_data = get_counter_64(zpl, GP0_RD_MINSTRET);
    clock_gettime(CLOCK_MONOTONIC, &end);
    setlocale(LC_NUMERIC, "");
    bsg_pr_info("ps.cpp: end polling i/o\n");
    bsg_pr_info("ps.cpp: minstret (instructions retired): %'16llu (%16llx)\n",
                minstret_start, minstret_start);
    bsg_pr_info("ps.cpp: minstret (instructions retired): %'16llu (%16llx)\n",
                minstret_stop, minstret_stop);
    unsigned long long minstret_delta = minstret_stop - minstret_start;
    bsg_pr_info("ps.cpp: minstret delta:                  %'16llu (%16llx)\n",
                minstret_delta, minstret_delta);
    bsg_pr_info("ps.cpp: MTIME start:                     %'16llu (%16llx)\n",
                mtime_start, mtime_start);
    bsg_pr_info("ps.cpp: MTIME stop:                      %'16llu (%16llx)\n",
                mtime_stop, mtime_stop);
    unsigned long long mtime_delta = mtime_stop - mtime_start;
    bsg_pr_info("ps.cpp: MTIME delta (=1/8 BP cycles):    %'16llu (%16llx)\n",
                mtime_delta, mtime_delta);
    bsg_pr_info("ps.cpp: IPC        :                     %'16f\n",
                ((double)minstret_delta) / ((double)(mtime_delta)) / 8.0);
    bsg_pr_info("ps.cpp: minstret (instructions retired): %'16llu (%16llx)\n",
                counter_data, counter_data);
    unsigned long long diff_ns =
        1000LL * 1000LL * 1000LL *
            ((unsigned long long)(end.tv_sec - start.tv_sec)) +
        (end.tv_nsec - start.tv_nsec);
    bsg_pr_info(
        "ps.cpp: wall clock time                : %'16llu (%16llx) ns\n",
        diff_ns, diff_ns);
    bsg_pr_info("ps.cpp: sim/emul speed                 : %'16.2f BP cycles "
                "per minute\n",
                mtime_delta * 8 /
                    ((double)(diff_ns) / (60.0 * 1000.0 * 1000.0 * 1000.0)));

    bsg_pr_info(
        "ps.cpp: BP DRAM USAGE MASK (each bit is 8 MB): "
        "%-8.8d%-8.8d%-8.8d%-8.8d\n",
        zpl->shell_read(GP0_RD_MEM_PROF_3), zpl->shell_read(GP0_RD_MEM_PROF_2),
        zpl->shell_read(GP0_RD_MEM_PROF_1), zpl->shell_read(GP0_RD_MEM_PROF_0));
    // in general we do not want to free the dram; the Xilinx allocator has a
    // tendency to
    // fail after many allocate/fail cycle. instead we keep a pointer to the
    // dram in a CSR in the accelerator, and if we reload the bitstream, we copy
    // the pointer back in.

    if (FREE_DRAM) {
        bsg_pr_info("ps.cpp: freeing DRAM buffer\n");
        zpl->free_dram((void *)buf);
        zpl->shell_write(GP0_WR_CSR_DRAM_INITED, 0x0, mask2);
    }

    zpl->done();
    delete zpl;
    return 0;
}

std::uint32_t rotl(std::uint32_t v, std::int32_t shift) {
    std::int32_t s = shift >= 0 ? shift % 32 : -((-shift) % 32);
    return (v << s) | (v >> (32 - s));
}

void nbf_load(bsg_zynq_pl *zpl, char *nbf_filename) {
    string nbf_command;
    string tmp;
    string delimiter = "_";

    long long int nbf[3];
    int pos = 0;
    long unsigned int base_addr;
    int data;
    std::ifstream nbf_file(nbf_filename);

    if (!nbf_file.is_open()) {
        bsg_pr_err("ps.cpp: error opening nbf file.\n");
        delete zpl;
        return;
    }

    int line_count = 0;
    while (getline(nbf_file, nbf_command)) {
        line_count++;
        int i = 0;
        while ((pos = nbf_command.find(delimiter)) != std::string::npos) {
            tmp = nbf_command.substr(0, pos);
            nbf[i] = std::stoull(tmp, nullptr, 16);
            nbf_command.erase(0, pos + 1);
            i++;
        }
        nbf[i] = std::stoull(nbf_command, nullptr, 16);

        if (nbf[0] == 0x3 || nbf[0] == 0x2 || nbf[0] == 0x1 || nbf[0] == 0x0) {
            // we map BP physical addresses for DRAM (0x8000_0000 - 0x9FFF_FFFF)
            // (256MB)
            // to the same ARM physical addresses
            // see top_zynq.v for more details

            // we map BP physical address for CSRs etc (0x0000_0000 -
            // 0x0FFF_FFFF) to ARM address to 0xA0000_0000 - 0xAFFF_FFFF
            // (256MB)
            if (nbf[1] >= DRAM_BASE_ADDR)
                base_addr = gp1_addr_base - DRAM_BASE_ADDR;
            else
                base_addr = GP1_CSR_BASE_ADDR;

            if (nbf[0] == 0x3) {
                zpl->shell_write(base_addr + nbf[1], nbf[2], 0xf);
                zpl->shell_write(base_addr + nbf[1] + 4, nbf[2] >> 32, 0xf);
            } else if (nbf[0] == 0x2) {
                zpl->shell_write(base_addr + nbf[1], nbf[2], 0xf);
            } else if (nbf[0] == 0x1) {
                int offset = nbf[1] % 4;
                int shift = 2 * offset;
                data = zpl->shell_read(base_addr + nbf[1] - offset);
                data = data & rotl((uint32_t)0xffff0000, shift) + nbf[2] &
                       ((uint32_t)0x0000ffff << shift);
                zpl->shell_write(base_addr + nbf[1] - offset, data, 0xf);
            } else {
                int offset = nbf[1] % 4;
                int shift = 2 * offset;
                data = zpl->shell_read(base_addr + nbf[1] - offset);
                data = data & rotl((uint32_t)0xffffff00, shift) + nbf[2] &
                       ((uint32_t)0x000000ff << shift);
                zpl->shell_write(base_addr + nbf[1] - offset, data, 0xf);
            }
        } else if (nbf[0] == 0xfe) {
            continue;
        } else if (nbf[0] == 0xff) {
            bsg_pr_dbg_ps("ps.cpp: nbf finish command, line %d\n", line_count);
            continue;
        } else {
            bsg_pr_dbg_ps("ps.cpp: unrecognized nbf command, line %d : %llx\n",
                          line_count, nbf[0]);
            return;
        }
    }

    bsg_pr_dbg_ps("ps.cpp: finished loading %d lines of nbf.\n", line_count);
}

bool decode_bp_output(bsg_zynq_pl *zpl, long data) {
    long rd_wr = data >> 31;
    long address = (data >> 8) & 0x7FFFFC;
    char print_data = data & 0xFF;
    char core = (address - 0x102000) >> 3;
    // write from BP
    if (rd_wr) {
        if (address == 0x101000) {
            printf("%c", print_data);
            fflush(stdout);
        } else if (address >= 0x102000 && address < 0x103000) {
            done_vec[core] = true;
            if (print_data == 0) {
                bsg_pr_info("CORE[%d] PASS\n", core);
            } else {
                bsg_pr_info("CORE[%d] FAIL\n", core);
            }
        } else if (address == 0x103000) {
            bsg_pr_dbg_ps("ps.cpp: Watchdog tick\n");
        } else {
            bsg_pr_err("ps.cpp: Errant write to %lx\n", address);
            return false;
        }
    }
    // read from BP
    else {
        // getchar
        if (address == 0x100000) {
            if (getchar_queue.empty()) {
                zpl->shell_write(GP0_WR_PS2PL_FIFO_DATA, -1, 0xf);
            } else {
                zpl->shell_write(GP0_WR_PS2PL_FIFO_DATA, getchar_queue.front(),
                                 0xf);
                getchar_queue.pop();
            }
        }
        // parameter ROM, only partially implemented
        else if (address >= 0x120000 && address <= 0x120128) {
            bsg_pr_dbg_ps("ps.cpp: PARAM ROM read from (%lx)\n", address);
            int offset = address - 0x120000;
            // CC_X_DIM, return number of cores
            if (offset == 0x0) {
                zpl->shell_write(GP0_WR_PS2PL_FIFO_DATA, BP_NCPUS, 0xf);
            }
            // CC_Y_DIM, just return 1 so X*Y == number of cores
            else if (offset == 0x4) {
                zpl->shell_write(GP0_WR_PS2PL_FIFO_DATA, 1, 0xf);
            }
        } else if (address >= 0x110000 && address < 0x111000) {
            int bootrom_addr = (address >> 2) & 0xfff;
            zpl->shell_write(GP0_WR_CSR_BOOTROM_ADDR, bootrom_addr, 0xf);
            int bootrom_data = zpl->shell_read(GP0_RD_BOOTROM_DATA);
            zpl->shell_write(GP0_WR_PS2PL_FIFO_DATA, bootrom_data, 0xf);
            // if not implemented, print error
        } else {
            bsg_pr_err("ps.cpp: Errant read from (%lx)\n", address);
            return false;
        }
    }

    return true;
}
