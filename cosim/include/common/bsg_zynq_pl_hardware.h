
#ifndef BSG_ZYNQ_PL_HARDWARE_H
#define BSG_ZYNQ_PL_HARDWARE_H

#include <assert.h>
#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <inttypes.h>
#include <iostream>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef UART_ENABLE
#define termios asmtermios
#include <asm/termios.h>
#undef termios
#undef winsize
#include <termios.h>
#endif
#include "bsg_argparse.h"
#include "bsg_printing.h"
#include "zynq_headers.h"

class bsg_zynq_pl_hardware {
  public:
    virtual void start(void) = 0;
    virtual void stop(void) = 0;
    virtual void tick(void) = 0;
    virtual void done(void) = 0;
    virtual void *allocate_dram(unsigned long len_in_bytes,
                                unsigned long *physical_ptr) = 0;
    virtual void free_dram(void *virtual_ptr) = 0;

  protected:
    int serial_port;
    uintptr_t gp0_base_offset = 0;
    uintptr_t gp1_base_offset = 0;

    inline volatile void *axil_get_ptr(uintptr_t address) {
        if (address >= gp1_addr_base)
            return (void *)(address + gp1_base_offset);
        else
            return (void *)(address + gp0_base_offset);
    }

    inline volatile int64_t *axil_get_ptr64(uintptr_t address) {
        return (int64_t *)axil_get_ptr(address);
    }

    inline volatile int32_t *axil_get_ptr32(uintptr_t address) {
        return (int32_t *)axil_get_ptr(address);
    }

    inline volatile int16_t *axil_get_ptr16(uintptr_t address) {
        return (int16_t *)axil_get_ptr(address);
    }

    inline volatile int8_t *axil_get_ptr8(uintptr_t address) {
        return (int8_t *)axil_get_ptr(address);
    }

    void init(void) {
        // open memory device
        int fd = open("/dev/mem", O_RDWR | O_SYNC);
        assert(fd != 0);
#ifdef GP0_ENABLE
        // map in first PLAXI region of physical addresses to virtual addresses
        uintptr_t ptr0 = (uintptr_t)mmap(
            (void *)gp0_addr_base, gp0_addr_size_bytes, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, gp0_addr_base);
        assert((uintptr_t)ptr0 == (uintptr_t)gp0_addr_base);

        printf("// bsg_zynq_pl: mmap returned %" PRIxPTR " (offset %" PRIxPTR
               ") errno=%x\n",
               ptr0, gp0_base_offset, errno);
#endif

#ifdef GP1_ENABLE
        // map in second PLAXI region of physical addresses to virtual addresses
        uintptr_t ptr1 = (uintptr_t)mmap(
            (void *)gp1_addr_base, gp1_addr_size_bytes, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, gp1_addr_base);
        assert((uintptr_t)ptr1 == (uintptr_t)gp1_addr_base);

        printf("// bsg_zynq_pl: mmap returned %" PRIxPTR " (offset %" PRIxPTR
               ") errno=%x\n",
               ptr1, gp1_base_offset, errno);
#endif
        close(fd);

#ifdef UART_ENABLE
        serial_port = open(UART_DEV_STR, O_RDWR | O_NOCTTY);

        struct termios tty;
        assert(!tcgetattr(serial_port, &tty));

        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= (CREAD | CLOCAL);

        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);

        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &=
            ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

        tty.c_oflag &= ~OPOST;
        tty.c_oflag &= ~ONLCR;

        tty.c_cc[VTIME] = 0;
        tty.c_cc[VMIN] = 4;

        cfsetspeed(&tty, UART_BAUD_ENUM);

        assert(!tcsetattr(serial_port, TCSANOW, &tty));
#endif
    }

    void deinit(void) {
#ifdef UART_ENABLE
        close(serial_port);
#endif
    }

#ifdef AXIL_ENABLE
    inline int32_t axil_read(uintptr_t address) {
        // Only aligned 32B reads are currently supported
        assert(alignof(address) >= 4);

        // We use unsigned here because the data is sign extended from the AXI
        // bus
        volatile int32_t *ptr32 = axil_get_ptr32(address);
        int32_t data = *ptr32;
        bsg_pr_dbg_pl("  bsg_zynq_pl: AXI reading [%" PRIxPTR "]->%8.8x\n",
                      address, data);

        return data;
    }

    inline void axil_write(uintptr_t address, int32_t data, uint8_t wstrb) {
        bsg_pr_dbg_pl("  bsg_zynq_pl: AXI writing [%" PRIxPTR
                      "]=%8.8x mask %" PRIu8 "\n",
                      address, data, wstrb);

        // for now we don't support alternate write strobes
        assert(wstrb == 0XF || wstrb == 0x3 || wstrb == 0x1);

        if (wstrb == 0xF) {
            volatile int32_t *ptr32 = axil_get_ptr32(address);
            *ptr32 = data;
        } else if (wstrb == 0x3) {
            volatile int16_t *ptr16 = axil_get_ptr16(address);
            *ptr16 = data;
        } else if (wstrb == 0x1) {
            volatile int8_t *ptr8 = axil_get_ptr8(address);
            *ptr8 = data;
        } else {
            assert(false); // Illegal write strobe
        }
    }
#endif

#ifdef UART_ENABLE
    // Must sync to verilog
    //     typedef struct packed
    //     {
    //       logic [31:0] data;
    //       logic [5:0]  addr7to2;
    //       logic        wr_not_rd;
    //       logic        port;
    //     } bsg_uart_pkt_s;

    void uart_write(uintptr_t addr, int32_t data, uint8_t wmask) {
        int count;

        uint64_t uart_pkt = 0;
        uintptr_t word = addr >> 2;
        int rdwr = 1;

        uart_pkt |= (data & 0xffffffff) << 8;
        uart_pkt |= (word & 0x0000003f) << 2;
        uart_pkt |= (rdwr & 0x00000001) << 1;
        uart_pkt |= (port & 0x00000001) << 0;

        count = write(serial_port, &uart_pkt, 5);
        bsg_pr_dbg_ps("uart tx write: %x bytes\n", count);
    }

    int32_t uart_read(uintptr_t addr) {
        int count;
        uint64_t uart_pkt = 0;
        uintptr_t word = addr >> 2;

        uint64_t uart_pkt = 0;
        uintptr_t word = addr >> 2;
        int32_t data = 0;
        int rdwr = 0;

        uart_pkt |= (data & 0xffffffff) << 8;
        uart_pkt |= (word & 0x0000003f) << 2;
        uart_pkt |= (rdwr & 0x00000001) << 1;
        uart_pkt |= (port & 0x00000001) << 0;

        count = write(serial_port, &uart_pkt, 5);
        bsg_pr_dbg_ps("uart rx write: %x bytes\n", count);

        int32_t read_buf;
        count = read(serial_port, &read_buf, 4);
        bsg_pr_dbg_ps("uart rx read: %x\n", read_buf, count);

        return read_buf;
    }
#endif

  public:
    virtual int32_t shell_read(uintptr_t addr) = 0;
    virtual void shell_write(uintptr_t addr, int32_t data, uint8_t wmask) = 0;

    virtual void shell_read4(uintptr_t addr, int32_t *data0, int32_t *data1,
                             int32_t *data2, int32_t *data3) {
        *data0 = shell_read(addr + 0);
        *data1 = shell_read(addr + 4);
        *data2 = shell_read(addr + 8);
        *data3 = shell_read(addr + 12);
    }

    virtual void shell_write4(uintptr_t addr, int32_t data0, int32_t data1,
                              int32_t data2, int32_t data3) {
        shell_write(addr + 0, data0, 0xf);
        shell_write(addr + 4, data1, 0xf);
        shell_write(addr + 8, data2, 0xf);
        shell_write(addr + 12, data3, 0xf);
    }
};

#endif
