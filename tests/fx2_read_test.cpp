// SPDX-License-Identifier: Apache-2.0
// Exercise the production FX2 read wrapper without accessing USB hardware.
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "fx2_ll.hpp"

static int status = LIBUSB_SUCCESS, transferred = 0;
static unsigned expected_timeout = 0;
static int release_status = 0;
static unsigned closes = 0, exits = 0;
static bool missing_device = false;
static unsigned releases = 0;
static int control_result = -999;
static unsigned short_control = 0;
struct Control { uint16_t address, length; uint8_t first; };
static std::vector<Control> controls;

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
	if (missing_device) return nullptr;
	return reinterpret_cast<libusb_device_handle *>(1);
}
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int) { return 0; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int) { releases++; return release_status; }
void LIBUSB_CALL libusb_close(libusb_device_handle *) { closes++; }
void LIBUSB_CALL libusb_exit(libusb_context *) { exits++; }
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *, uint8_t,
	uint8_t, uint16_t address, uint16_t, unsigned char *data, uint16_t length, unsigned)
{
	controls.push_back({address, length, uint8_t(data && length ? data[0] : 0)});
	if (short_control && controls.size() == short_control) return length - 1;
	return control_result == -999 ? length : control_result;
}
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

static void control_checks()
{
	FX2_ll fx2(0, 0, 0x09fb, 0x6010, "");
	uint8_t data[4] = {};
	const int results[] = {0, 2, 4, LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_NO_DEVICE};
	for (int actual : results) {
		control_result = actual;
		require(bool(fx2.write_ctrl(0xa0, 0, data, sizeof(data))) == (actual == 4),
			"incomplete control write accepted");
		require(bool(fx2.read_ctrl(0x94, 0, data, sizeof(data))) == (actual == 4),
			"incomplete control read accepted");
	}
	control_result = 0;
	require(fx2.write_ctrl(0xb0, 0, nullptr, 0), "zero-payload control write rejected");
	require(fx2.read_ctrl(0xb0, 0, nullptr, 0), "zero-payload control read rejected");
	control_result = -999;
}

static void firmware_checks(const std::string &bundled_firmware)
{
	const auto dir = std::filesystem::temp_directory_path()/
		("openfpga-fx2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(dir);
	std::ofstream(dir/"valid.hex") << ":040010001122334442\n:00000001FF\n";
	std::ofstream(dir/"invalid.hex") << ":0100000000FE\n:00000001FF\n";
	std::ofstream(dir/"empty.hex") << ":00000001FF\n";
	std::ofstream(dir/"partial.hex") << ":0100000000FF\n:0100100001EE\n";
	try {
		for (const auto *name : {"missing.hex", "invalid.hex", "empty.hex", "partial.hex", "valid.hex"}) {
			controls.clear();
			short_control = std::string(name) == "valid.hex" ? 2 : 0;
			const auto old_closes = closes, old_exits = exits, old_releases = releases;
			bool failed = false;
			try {
				FX2_ll fx2(0x09fb, 0x6810, 0x09fb, 0x6010, (dir/name).string());
			} catch (const std::runtime_error &) { failed = true; }
			require(failed, "invalid or incomplete firmware upload accepted");
			require(closes == old_closes + 1 && exits == old_exits + 1 &&
				releases == old_releases + 1, "firmware failure leaked a claimed interface, handle or context");
			if (short_control)
				require(controls.size() == 2 && controls.back().address == 0x10,
					"FX2 CPU was released after an incomplete firmware write");
			else
				require(controls.empty(), "FX2 CPU was reset before firmware validation completed");
		}
		short_control = 0;
		controls.clear();
		const auto old_closes = closes, old_exits = exits, old_releases = releases;
		{
			FX2_ll fx2(0x09fb, 0x6810, 0x09fb, 0x6010, (dir/"valid.hex").string());
		}
		require(controls.size() == 3 && controls[0].address == 0xe600 && controls[0].first == 1 &&
			controls[1].address == 0x10 && controls[1].length == 4 &&
			controls[2].address == 0xe600 && controls[2].first == 0,
			"valid firmware upload/reset sequence changed");
		require(closes == old_closes + 2 && releases == old_releases + 2 && exits == old_exits + 1,
			"firmware re-enumeration leaked or double-closed resources");
		controls.clear();
		{
			FX2_ll bundled(0x09fb, 0x6810, 0x09fb, 0x6010, bundled_firmware);
		}
		require(controls.size() > 3 && controls.front().address == 0xe600 &&
			controls.front().first == 1 && controls.back().address == 0xe600 &&
			controls.back().first == 0, "bundled probe firmware no longer loads");
		for (const auto &control : controls)
			require(control.length <= 64, "firmware write exceeds FX2 control-transfer chunk size");
	} catch (...) {
		std::filesystem::remove_all(dir);
		throw;
	}
	std::filesystem::remove_all(dir);
}

int main(int argc, char **argv)
{
	try {
		require(argc == 2, "bundled firmware fixture path is required");
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
		for (int error : {LIBUSB_SUCCESS, LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_BUSY}) {
			release_status = error;
			const auto previous_closes = closes, previous_exits = exits;
			{
				FX2_ll disconnected(0, 0, 0x09fb, 0x6010, "");
			}
			require(closes == previous_closes + 1 && exits == previous_exits + 1,
				"failed interface release leaked the device handle or context");
		}
		release_status = 0;
		missing_device = true;
		const auto previous_exits = exits;
		bool failed = false;
		try {
			FX2_ll missing(0, 0, 0x09fb, 0x6010, "");
		} catch (const std::runtime_error &) { failed = true; }
		require(failed && exits == previous_exits + 1, "failed open leaked the USB context");
		missing_device = false;
		control_checks();
		firmware_checks(argv[1]);
		puts("FX2 timeout, firmware validation, control transfer and cleanup tests passed");
	} catch (const std::exception &error) {
		fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
