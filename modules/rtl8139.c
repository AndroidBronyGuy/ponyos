#include <module.h>
#include <logging.h>
#include <pci.h>
#include <ipv4.h>
#include <mod/shell.h>

static uint32_t rtl_device_pci = 0x00000000;

static void find_rtl(uint32_t device, uint16_t vendorid, uint16_t deviceid) {
	if ((vendorid == 0x10ec) && (deviceid == 0x8139)) {
		rtl_device_pci = device;
	}
}

#define RTL_PORT_MAC     0x00
#define RTL_PORT_MAR     0x08
#define RTL_PORT_RBSTART 0x30
#define RTL_PORT_CMD     0x37
#define RTL_PORT_IMR     0x3C
#define RTL_PORT_ISR     0x3E
#define RTL_PORT_RCR     0x44
#define RTL_PORT_CONFIG  0x52

static uint8_t rtl_rx_buffer[8192+16];

DEFINE_SHELL_FUNCTION(rtl, "rtl8139 experiments") {
	if (rtl_device_pci) {
		fs_printf(tty, "Located an RTL 8139: 0x%x\n", rtl_device_pci);

		uint16_t command_reg = pci_read_field(rtl_device_pci, PCI_COMMAND, 2);
		fs_printf(tty, "COMMAND register before: 0x%4x\n", command_reg);
		if (command_reg & 0x0002) {
			fs_printf(tty, "Bus mastering already enabled.\n");
		} else {
			command_reg |= 0x2; /* bit 2 */
			fs_printf(tty, "COMMAND register after:  0x%4x\n", command_reg);
			fs_printf(tty, "XXX: I can't write config registers :(\n");
			return -1;
		}

		uint32_t rtl_irq = pci_read_field(rtl_device_pci, PCI_INTERRUPT_LINE, 1);

		fs_printf(tty, "Interrupt Line: %x\n", rtl_irq);

		uint32_t rtl_bar0 = pci_read_field(rtl_device_pci, PCI_BAR0, 4);
		uint32_t rtl_bar1 = pci_read_field(rtl_device_pci, PCI_BAR1, 4);

		fs_printf(tty, "BAR0: 0x%8x\n", rtl_bar0);
		fs_printf(tty, "BAR1: 0x%8x\n", rtl_bar1);

		uint32_t rtl_iobase = 0x00000000;

		if (rtl_bar0 & 0x00000001) {
			rtl_iobase = rtl_bar0 & 0xFFFFFFFC;
		} else {
			fs_printf(tty, "This doesn't seem right! RTL8139 should be using an I/O BAR; this looks like a memory bar.");
		}

		fs_printf(tty, "RTL iobase: 0x%x\n", rtl_iobase);

		fs_printf(tty, "Determining mac address...\n");

		uint8_t mac[6];
		for (int i = 0; i < 6; ++i) {
			mac[i] = inports(rtl_iobase + RTL_PORT_MAC + i);
		}

		fs_printf(tty, "%2x:%2x:%2x:%2x:%2x:%2x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

		fs_printf(tty, "Enabling RTL8139.\n");
		outportb(rtl_iobase + RTL_PORT_CONFIG, 0x0);

		fs_printf(tty, "Resetting RTL8139.\n");
		outportb(rtl_iobase + RTL_PORT_CMD, 0x10);
		while ((inportb(rtl_iobase + 0x37) & 0x10) != 0) { }

		fs_printf(tty, "Done resetting RTL8139.\n");

		fs_printf(tty, "Initializing receive buffer.\n");
		outportl(rtl_iobase + RTL_PORT_RBSTART, (unsigned long)&rtl_rx_buffer);

		fs_printf(tty, "Enabling IRQs.\n");
		outports(rtl_iobase + RTL_PORT_IMR, 0x0005); /* TOK, ROK */

		fs_printf(tty, "Configuring receive buffer.\n");
		outportl(rtl_iobase + RTL_PORT_RCR, 0xF | (1 << 7)); /* 0xF = AB+AM+APM+AAP */

		fs_printf(tty, "Enabling receive and transmit.\n");
		outportb(rtl_iobase + RTL_PORT_CMD, 0x0C);

#if 0
		fs_printf(tty, "Going to try to force-send a UDP packet...\n");
		struct ipv4_packet p;
		p.version_ihl = (4 << 4) & (5 << 0); /* IPv4, no options */
		p.dscp_ecn = 0; /* nope nope nope */
		p.length = sizeof(struct ipv4_packet) + sizeof(struct udp_packet) + sizeof(struct dhcp_packet);
		p.ident = 0;
		p.flags_fragment = 0;
		p.ttl = 0xFF;
		p.protocol = 17;
		p.checksum = 0; /* calculate this later */
		p.source = 0x00000000; /* 0.0.0.0 */
		p.destination = 0xFFFFFFFF; /* 255.255.255.255 */

		uint16_t * packet = (uint16_t *)&p;
		uint32_t total = 0;
		for (int i = 0; i < 10; ++i) {
			total += packet[i];
			if (total & 0x80000000) {
				total = (total & 0xFFFF) + (total >> 16);
			}
		}

		while (total >> 16) {
			total = (total & 0xFFFF) + (total >> 16);
		}

		p.checksum = ~total;

		struct udp_packet u;
		u.source = p.source;
		u.destination = p.destination;
		u.zeroes = 0;
		u.protocol = p.protocol;
		u.udp_length = p.length;
		u.source_port = 68;
		u.destination_port = 67;
		u.length = sizeof(struct dhcp_packet);
		u.checksum = 0;
#endif


	} else {
		return -1;
	}
	return 0;
}

static int init(void) {
	BIND_SHELL_FUNCTION(rtl);
	pci_scan(&find_rtl, -1);
	if (!rtl_device_pci) {
		debug_print(ERROR, "No RTL 8139 found?");
		return 1;
	}
	return 0;
}

static int fini(void) {
	return 0;
}

MODULE_DEF(rtl8139, init, fini);
MODULE_DEPENDS(debugshell);
