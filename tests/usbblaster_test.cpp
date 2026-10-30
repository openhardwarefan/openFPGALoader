// SPDX-License-Identifier: Apache-2.0
// An independent TAP/USB-Blaster model; no hardware or firmware is required.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "jtag.hpp"
#include "fx2_ll.hpp"
#include "stratix10gx.hpp"

static void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

struct Probe {
	unsigned ir_delay = 0, dr_delay = 0;
	unsigned instruction[2] = {6, 6};
	uint32_t primary_id = 0xc32150dd;
	int state = 0;
	uint8_t pins = 0, sampled = 0;
	bool fail_write = false;
	bool short_write = false, stall_read = false;
	unsigned read_chunk = 0;
	std::deque<bool> shift;
	std::deque<uint8_t> pending, input;
	// IEEE 1149.1 state transitions, indexed by TMS.
	static constexpr int next[16][2] = {
		{1,0}, {1,2}, {3,9}, {4,5}, {4,5}, {6,8}, {6,7}, {4,8},
		{1,2}, {10,0}, {11,12}, {11,12}, {13,15}, {13,14}, {11,15}, {1,2}
	};

	void append(uint32_t value, unsigned bits)
	{
		for (unsigned i = 0; i < bits; i++)
			shift.push_back((value >> i) & 1);
	}
	unsigned extract(unsigned pos, unsigned bits)
	{
		unsigned value = 0;
		for (unsigned i = 0; i < bits; i++)
			value |= unsigned(shift.at(pos + i)) << i;
		return value;
	}
	void clock(bool tms, bool tdi)
	{
		sampled = shift.empty() ? 0 : shift.front();
		if (state == 4 || state == 11) {
			shift.pop_front();
			shift.push_back(tdi);
		}
		state = next[state][tms];
		if (state == 0)
			instruction[0] = instruction[1] = 6;
		if (state == 10) {
			shift.clear();
			append(1, 10);
			append(1, ir_delay);
			append(0x155, 10);
		}
		if (state == 15) {
			instruction[0] = extract(0, 10);
			instruction[1] = extract(10 + ir_delay, 10);
		}
		if (state == 3) {
			shift.clear();
			append(instruction[0] == 6 ? primary_id : 0,
				instruction[0] == 6 ? 32 : 1);
			append(1, dr_delay);
			append(instruction[1] == 6 ? 0x020a40dd : 0,
				instruction[1] == 6 ? 32 : 1);
		}
	}
	void command(const uint8_t *data, unsigned size)
	{
		for (unsigned i = 0; i < size; i++) {
			uint8_t v = data[i];
			if (v == 0x5f) {
				input.insert(input.end(), pending.begin(), pending.end());
				pending.clear();
			} else if (v & 0x80) {
				require(!(pins & 1), "byteshift entered with TCK high");
				const unsigned count = v & 63;
				require(i + count < size, "truncated byteshift command");
				for (unsigned b = 0; b < count; b++) {
					uint8_t rx = 0, tx = data[++i];
					for (unsigned bit = 0; bit < 8; bit++) {
						clock(pins & 2, (tx >> bit) & 1);
						rx |= sampled << bit;
					}
					if (v & 0x40) pending.push_back(rx);
				}
			} else {
				if (!(pins & 1) && (v & 1))
					clock(v & 2, v & 0x10);
				pins = v;
				if (v & 0x40) pending.push_back(sampled);
			}
		}
	}
};

static Probe probe;

// Mock only the physical FX2 transport. Exercise the production cable and TAP code.
FX2_ll::FX2_ll(uint16_t, uint16_t, uint16_t, uint16_t, const std::string &) {}
FX2_ll::~FX2_ll() = default;
bool FX2_ll::clear_halt(uint8_t) { return true; }
int FX2_ll::read_quiet(uint8_t, uint8_t *, uint16_t, unsigned) { return 0; }
int FX2_ll::read_ctrl(uint8_t, uint16_t, uint8_t *data, uint16_t size, uint16_t)
{
	memset(data, 0, size);
	return size;
}
int FX2_ll::write(uint8_t, uint8_t *data, uint16_t size)
{
	if (probe.fail_write) return -1;
	if (probe.short_write) return size - 1;
	probe.command(data, size);
	return size;
}
int FX2_ll::read(uint8_t, uint8_t *data, uint16_t size)
{
	if (probe.stall_read) return 0;
	if (probe.read_chunk) size = std::min<unsigned>(size,probe.read_chunk);
	require(probe.input.size() >= size, "unexpected USB read length");
	for (unsigned i = 0; i < size; i++) {
		data[i] = probe.input.front();
		probe.input.pop_front();
	}
	return size;
}

static void run(unsigned delay, uint32_t primary_id = 0xc32150dd)
{
	probe = Probe();
	probe.ir_delay = probe.dr_delay = delay;
	probe.primary_id = primary_id;
	const std::map<uint32_t, misc_device> misc;
	Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
		0, "", 0, false, "mock.hex", misc);
	require(j.get_chain_len() == 2, "device count");
	for (unsigned index = 0; index < 2; index++) {
		j.device_select(index);
		uint8_t ir[2] = {6, 0}, captured[2] = {};
		j.shiftIR(ir, captured, 10);
		require((captured[0] & 3) == 1, "IR capture alignment");
		uint8_t rx[4] = {};
		j.shiftDR(NULL, rx, 32);
		uint32_t id = 0;
		for (unsigned b = 0; b < 4; b++) id |= uint32_t(rx[b]) << (8*b);
		require(id == j.get_target_device_id(), "IDCODE after UPDATE_IR");
		// Continue an IR scan without inserting the leading padding twice.
		uint8_t other_ir = 0x0c;
		j.shiftIR(&other_ir, NULL, 5, Jtag::SHIFT_IR);
		uint8_t rest = 0;
		j.shiftIR(&rest, NULL, 5);
		j.flushTMS(true);
		require(probe.instruction[1-index] == 0x0c, "split IR scan");
	}

	uint8_t bypass[3] = {0xff, 0xff, 0xff};
	j.set_state(Jtag::SHIFT_IR);
	j.read_write(bypass, NULL, 20 + delay, true);
	j.set_state(Jtag::RUN_TEST_IDLE);
	for (unsigned bits : {1u, 2u, 7u, 8u, 9u, 19u, 20u, 21u, 31u, 32u,
		33u, 63u, 64u, 65u, 503u, 504u, 505u, 511u, 512u, 513u, 1008u}) {
		std::vector<uint8_t> tx((bits+7)/8), rx(tx.size(), 0xff);
		probe.read_chunk = 3; // A successful read may span several USB transfers.
		for (size_t i = 0; i < tx.size(); i++) tx[i] = (i * 73 + 0x69) & 255;
		j.set_state(Jtag::SHIFT_DR);
		j.read_write(tx.data(), rx.data(), bits, true);
		j.set_state(Jtag::RUN_TEST_IDLE);
		for (unsigned i = 2 + delay; i < bits; i++) {
			unsigned src = i - 2 - delay;
			require(((rx[i/8] >> (i%8)) & 1) ==
				((tx[src/8] >> (src%8)) & 1), "scan data across USB boundaries");
		}
		for (bool end : {false, true}) {
			std::vector<uint8_t> expected(rx.size(), 0), inplace = tx;
			j.set_state(Jtag::SHIFT_DR);
			j.read_write(tx.data(), expected.data(), bits, end);
			const auto outgoing = probe.shift;
			j.set_state(Jtag::RUN_TEST_IDLE);
			j.set_state(Jtag::SHIFT_DR);
			j.read_write(inplace.data(), inplace.data(), bits, end);
			require(probe.shift == outgoing, "in-place scan changed outgoing bits");
			for (unsigned i = 0; i < bits; i++)
				require(((inplace[i/8] >> (i%8)) & 1) ==
					((expected[i/8] >> (i%8)) & 1), "in-place scan capture");
			j.set_state(Jtag::RUN_TEST_IDLE);
		}
	}
	const auto selected = j.get_target_device_id();
	require(j.device_select(j.get_chain_len()) < 0, "one-past-end chain index accepted");
	require(j.get_target_device_id() == selected, "invalid selection changed the target");
	const int old_state = probe.state;
	j.read_write(NULL,NULL,0,true);
	require(probe.state == old_state, "empty scan changed TAP state");
	for (bool short_write : {false,true}) {
		bool failed = false;
		probe.short_write = short_write;
		probe.stall_read = !short_write;
		try { j.read_write(NULL,bypass,8,false); }
		catch (const std::runtime_error &) { failed = true; }
		probe.short_write = probe.stall_read = false;
		require(failed, "incomplete USB transfer accepted");
	}
	probe.pending.clear(); probe.input.clear();
	bool failed = false;
	probe.fail_write = true;
	try { j.read_write(NULL, NULL, 1, false); }
	catch (const std::runtime_error &) { failed = true; }
	probe.fail_write = false;
	require(failed, "USB error propagation");
}

static void board_defaults()
{
	using namespace Stratix10GX;
	const auto expected = (std::filesystem::path("config root")/"openFPGALoader"/"stratix10gx").string();
	require(configurationDirectory("config root","ignored") == expected,"configuration root precedence");
	require(configurationDirectory(nullptr,"user") ==
		(std::filesystem::path("user")/".config"/"openFPGALoader"/"stratix10gx").string(),"home config fallback");
	require(configurationDirectory("",nullptr).empty(),"missing configuration root");
	require(existingFile("","blaster_6810.hex").empty(),"empty directory searched current directory");
	const auto dir = std::filesystem::temp_directory_path()/
		("openfpgaloader-board-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(dir);
	std::ofstream(dir/"blaster_6810.hex") << "fixture";
	std::ofstream(dir/"openFPGALoader") << "fixture";
	require(relativeDirectory(dir/"openFPGALoader") ==
		(std::filesystem::canonical(dir)/"share"/"openFPGALoader"/"stratix10gx").string(),
		"portable data relative to real executable");
	std::filesystem::create_directory(dir/"bin");
	std::ofstream(dir/"bin"/"openFPGALoader") << "fixture";
	const auto installed = dir/"share"/"openFPGALoader"/"stratix10gx";
	std::filesystem::create_directories(installed);
	require(relativeDirectory(dir/"bin"/"openFPGALoader") ==
		std::filesystem::canonical(installed).string(), "relocated installation data");
	const auto local = dir/"bin"/"share"/"openFPGALoader"/"stratix10gx";
	std::filesystem::create_directories(local);
	require(relativeDirectory(dir/"bin"/"openFPGALoader") ==
		std::filesystem::canonical(local).string(), "adjacent portable data precedence");
	require(relativeDirectory(dir/"missing-executable").empty(),"missing executable used current directory");
	require(!existingFile(dir.string(),"blaster_6810.hex").empty(),"probe firmware discovery");
	require(existingFile(dir.string(),"missing.hex").empty(),"missing firmware selected");
	require(existingFile(dir.parent_path().string(),dir.filename().string().c_str()).empty(),"directory treated as firmware");
	std::filesystem::remove_all(dir);
	require(targetIndex({0x020a40dd,0xc32150dd}) == 1,"MAX V before Stratix 10");
	require(targetIndex({0xc32150dd,0x020a40dd}) == 0,"Stratix 10 before MAX V");
	require(targetIndex({0x032250dd}) == 0,"other recognized Stratix 10 rejected");
	require(targetIndex({0xffffffff,0x020a40dd,0xc32150dd}) == 2,"target index assumed fixed");
	require(targetIndex({0x020a40dd,0x020b10dd}) == -1,"non-Stratix target selected");
	require(targetIndex({0xc32150dd,0x032250dd}) == -2,"ambiguous FPGA selection accepted");
}

int main()
{
	try {
		board_defaults();
		run(0);
		run(0,0x020b10dd); // Existing Cyclone II chain uses upstream JTAG scan behavior.
		run(1);
		probe = Probe();
		probe.ir_delay = 2;
		probe.dr_delay = 1;
		bool rejected = false;
		const std::map<uint32_t, misc_device> misc;
		try {
			Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
				0, "", 0, false, "mock.hex", misc);
		} catch (const std::runtime_error &) { rejected = true; }
		require(rejected, "inconsistent IR/DR padding must be rejected");
		puts("USB-Blaster transport and chain tests passed");
	} catch (const std::exception &e) {
		fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
