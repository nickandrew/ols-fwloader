#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <libusb.h>
#include <string.h>

#include "boot_if.h"
#include "ols-boot.h"

struct ols_boot_t *BOOT_Init(uint16_t vid, uint16_t pid)
{
	struct ols_boot_t *ob;
	int ret;

	ob = malloc(sizeof(struct ols_boot_t));
	if (ob == NULL) {
		fprintf(stderr, "Not enough memory \n");
		return NULL;
	}
	memset(ob, 0, sizeof(struct ols_boot_t));

	ret = libusb_init(&ob->ctx);
	if (ret != 0) {
		fprintf(stderr, "libusb_init proobem\n");
	}

	libusb_set_debug(ob->ctx, 4);
	ob->dev = libusb_open_device_with_vid_pid(ob->ctx, vid, pid);
	if (ob->dev == NULL) {
		fprintf(stderr, "Device not found\n");
		free(ob);
		return NULL;
	}

	if (libusb_kernel_driver_active(ob->dev, 0)) {
		// reattach later
		ob->attach = 1;
		if (libusb_detach_kernel_driver(ob->dev, 0)) {
			fprintf(stderr, "Error detaching kernel driver \n");
			free(ob);
			return NULL;
		}
	}

	ret = libusb_claim_interface(ob->dev, 0);
	if (ret != 0) {
		fprintf(stderr, "Cannot claim device\n");
	}

	ret = libusb_set_interface_alt_setting(ob->dev, 0, 0);
	if (ret != 0) {
		fprintf(stderr, "Unable to set alternative interface \n");
	}

	return ob;
}

static uint8_t BOOT_Recv(struct ols_boot_t *ob, boot_rsp *rsp)
{
	int ret;
	int len;

	memset (rsp, 0, sizeof(boot_rsp));

	ret = libusb_interrupt_transfer(ob->dev, 0x81, (uint8_t *)rsp, sizeof(boot_rsp), &len, OLS_TIMEOUT);
	if ((ret == 0) && (len == sizeof(boot_rsp))) {
		return 0;
	} else if ((ret == 0) && (len != sizeof(boot_rsp))) {
		fprintf(stderr, "Transfered too little (%d)\n", len);
		return 1;
	} else if (ret == LIBUSB_ERROR_TIMEOUT) {
		fprintf(stderr, "Com timeout\n");
		return 1;
	} else if (ret == LIBUSB_ERROR_PIPE) {
		fprintf(stderr, "Error sending, not ols ?\n");
		return 2;
	} else if (ret == LIBUSB_ERROR_NO_DEVICE) {
		fprintf(stderr, "Device disconnected \n");
		return 3;
	}

	fprintf(stderr, "Other error \n");
	return 4;
}

static uint8_t BOOT_Send(struct ols_boot_t *ob, boot_cmd *cmd)
{
	int ret;
	
	ret = libusb_control_transfer(ob->dev, 0x21, 0x09, 0x0000, 0x0000, (uint8_t *)cmd, sizeof(boot_cmd), OLS_TIMEOUT);
	if (ret == sizeof(boot_cmd)) {
		return 0;
	} else if (ret == LIBUSB_ERROR_TIMEOUT) {
		fprintf(stderr, "Com timeout\n");
		return 1;
	} else if (ret == LIBUSB_ERROR_PIPE) {
		fprintf(stderr, "Error sending, not ols ?\n");
		return 2;
	} else if (ret == LIBUSB_ERROR_NO_DEVICE) {
		fprintf(stderr, "Device disconnected \n");
		return 3;
	}
	fprintf(stderr, "Other error \n");
	return 4;
}

static uint8_t BOOT_SendRecv(struct ols_boot_t *ob, boot_cmd *cmd, boot_rsp *rsp)
{
	int ret;

	ret = BOOT_Send(ob, cmd);

	if (ret != 0)
		return ret;

	ret = BOOT_Recv(ob, rsp);

	if (ret != 0)
		return ret;

	// check echo byte
	if (cmd->header.echo != rsp->header.echo) {
		fprintf(stderr, "Id doesn't match. Bootloader error\n");
		return 1;
	}
	return 0;
}

uint8_t BOOT_Version(struct ols_boot_t *ob)
{
	boot_cmd cmd;
	boot_rsp rsp;
	int ret;

	memset(&cmd, 0, sizeof(cmd));

	cmd.header.cmd = BOOT_GET_FW_VER;

	ret = BOOT_SendRecv(ob, &cmd, &rsp);
	
	if (ret) {
		return 1;
	}

	printf("Bootloader version %d.%d.%d\n", rsp.get_fw_ver.major,
		rsp.get_fw_ver.minor, rsp.get_fw_ver.sub_minor);

	return 0;
}

uint8_t BOOT_Read(struct ols_boot_t *ob, uint16_t addr, uint8_t *buf, uint16_t size)
{
	// todo loop
	boot_cmd cmd;
	boot_rsp rsp;
	uint16_t address = 0;
	uint16_t len;
	int ret;

	memset(&cmd, 0, sizeof(cmd));

	while (size > 0) {
		len = (size > OLS_READ_SIZE) ? OLS_READ_SIZE : size;
		cmd.header.cmd = BOOT_READ_FLASH;
		cmd.header.echo = ob->cmd_id ++;

		cmd.read_flash.addr_hi = (address >> 8) & 0xff;
		cmd.read_flash.addr_lo = address & 0xff;
		cmd.read_flash.size8 = (len%2)?len+1:len; // round if odd

		ret = BOOT_SendRecv(ob, &cmd, &rsp);
		if (ret != 0) {
			fprintf(stderr, "Error reading memory\n");
			return 1;
		}
		memcpy(buf, rsp.read_flash.data, len);
		buf += len;
		size -= len;
		address += len;
	}
	return 0;
}

uint8_t BOOT_Write(struct ols_boot_t *ob, uint16_t addr, uint8_t *buf, uint16_t size)
{
	// todo loop
	boot_cmd cmd;
	boot_rsp rsp;
	uint16_t address = 0;
	uint16_t len;
	int ret;

	address = addr;
	memset(&cmd, 0, sizeof(cmd));

	while (size > 0) {
		len = (size > OLS_PAGE_SIZE) ? OLS_PAGE_SIZE : size;
		len = (len % 2) ? len + 1: len; // round odd 

		cmd.header.cmd = BOOT_WRITE_FLASH;
		cmd.header.echo = ob->cmd_id ++;

		cmd.write_flash.addr_hi = (address >> 8) & 0xff;
		cmd.write_flash.addr_lo = address & 0xff;
		cmd.write_flash.size8 = len; // round odd 
		cmd.write_flash.flush = 0xff;
		memcpy(cmd.write_flash.data, buf, len);

		if (address < OLS_FLASH_ADDR) {
			fprintf(stderr, "Protecting bootloader - skip @0x%04x\n", address);
		} if (address + len >= OLS_FLASH_ADDR + OLS_FLASH_SIZE) {
			fprintf(stderr, "Protecting bootloader - skip @0x%04x\n", address);
			// we end
			break;
		} else {
			ret = BOOT_SendRecv(ob, &cmd, &rsp);
		}
		if (ret != 0) {
			fprintf(stderr, "Error writing memory\n");
			return 1;
		}	

		buf += len;
		size -= len;
		address += len;
	}
	return 0;
}

uint8_t BOOT_Erase(struct ols_boot_t *ob)
{
	// todo loop
	boot_cmd cmd;
	boot_rsp rsp;
	uint16_t address = 0;
	int ret;

	memset(&cmd, 0, sizeof(cmd));

	cmd.header.cmd = BOOT_ERASE_FLASH;
	cmd.header.echo = ob->cmd_id ++;

	cmd.erase_flash.addr_hi = 0x08;//(address >> 8) & 0xff;
	cmd.erase_flash.addr_lo = 0x00;//address & 0xff;
	cmd.erase_flash.size_x64 = 0x0d;//64;

	ret = BOOT_SendRecv(ob, &cmd, &rsp);
	if (ret != 0) {
		fprintf(stderr, "Error erasing memory\n");
	}
	return ret;
}

uint8_t BOOT_Reset(struct ols_boot_t *ob)
{
	boot_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));

	cmd.header.cmd = BOOT_RESET;
	BOOT_Send(ob, &cmd);

	// device wont exist after reset
	ob->attach = 0;
}


void BOOT_Deinit(struct ols_boot_t *ob)
{
	libusb_release_interface(ob->dev, 0);

	if (ob->attach) {
		if (libusb_attach_kernel_driver(ob->dev, 0)) {
			fprintf(stderr, "Unable to reattach kernel driver\n");
		}
	}

	libusb_close(ob->dev);
	libusb_exit(ob->ctx);

}

