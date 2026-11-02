// SPDX-License-Identifier: Apache-2.0
// Exercise the production FX2 read wrapper without accessing USB hardware.
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "fx2_ll.hpp"

static int status = LIBUSB_SUCCESS, transferred = 0;
static unsigned expected_timeout = 0;

static void require(bool condition, const char *message)
{
	if (!condition) throw std::runtime_error(message);
}

extern "C" {
int LIBUSB_CALL libusb_init(libusb_context **context)
{
	*context = nullptr;
	return 0;
}
libusb_device_handle *LIBUSB_CALL libusb_open_device_with_vid_pid(
	libusb_context *, uint16_t, uint16_t)
{
	return reinterpret_cast<libusb_device_handle *>(1);
}
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int) { return 0; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int) { return 0; }
void LIBUSB_CALL libusb_close(libusb_device_handle *) {}
void LIBUSB_CALL libusb_exit(libusb_context *) {}
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *, unsigned char endpoint,
	unsigned char *data, int length, int *actual, unsigned timeout)
{
	require(endpoint == 0x88 && timeout == expected_timeout, "bulk read arguments");
	require(transferred <= length, "mock transfer exceeds buffer");
	*actual = transferred;
	memset(data, 0xa5, transferred);
	return status;
}
}

int main()
{
	try {
		// Skip firmware loading; every USB entry point used here is mocked.
		FX2_ll fx2(0, 0, 0x09fb, 0x6010, "");
		uint8_t data[8] = {};
		expected_timeout = 123;
		for (int result : {LIBUSB_SUCCESS, LIBUSB_ERROR_TIMEOUT}) {
			status = result;
			for (int count : {0, 3, 8}) {
				transferred = count;
				require(fx2.read_quiet(8, data, sizeof(data), expected_timeout) == count,
					"partial or complete TDO bytes lost on timeout");
				for (int i = 0; i < count; i++)
					require(data[i] == 0xa5, "TDO data corrupted");
			}
		}
		for (int error : {LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_PIPE, LIBUSB_ERROR_OVERFLOW}) {
			status = error;
			transferred = 0;
			require(fx2.read_quiet(8, data, sizeof(data), expected_timeout) == error,
				"fatal USB read error lost");
		}
		puts("FX2 timeout and partial read tests passed");
	} catch (const std::exception &error) {
		fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
