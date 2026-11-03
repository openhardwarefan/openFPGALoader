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
#include "usbBlaster.hpp"
#include "altera.hpp"

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
	unsigned empty_reads = 0, read_calls = 0, write_calls = 0;
	int read_error = 0;
	std::deque<int> read_results;
	unsigned opens = 0, closes = 0, timeout_opens = 0, timeout_after_reads = 0;
	unsigned fail_open = 0;
	unsigned drained = 0;
	bool corrupt_scan = false;
	bool halt_failure = false, endless_tdo = false, drain_after_copy = false;
	int drain_error = 0;
	unsigned copies = 0, drain_calls = 0;
	bool sdm = false, waiting_for_ack = false, final_done = true;
	unsigned accepted = 0, chunks = 0, flow_reads = 0, busy_polls = 0;
	enum FlowFault { NORMAL, PARTIAL, BUSY, BUSY_FOREVER, ERROR, FORWARD, BACKWARD, STALL } flow_fault = NORMAL;
	uint64_t flow_status = 0;
	std::vector<bool> written_dr;
	std::vector<uint8_t> design;
	std::deque<uint64_t> mailbox;
	std::deque<bool> shift;
	std::deque<uint8_t> pending, input;
	// IEEE 1149.1 state transitions, indexed by TMS.
	static constexpr int next[16][2] = {
		{1,0}, {1,2}, {3,9}, {4,5}, {4,5}, {6,8}, {6,7}, {4,8},
		{1,2}, {10,0}, {11,12}, {11,12}, {13,15}, {13,14}, {11,15}, {1,2}
	};

	void append(uint64_t value, unsigned bits)
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
			if (state == 4 && sdm) written_dr.push_back(tdi);
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
		if (state == 8 && sdm && instruction[0] == 2) {
			require(!waiting_for_ack, "next chunk sent before SDM acknowledged its predecessor");
			require(written_dr.size() >= 66, "truncated SDM data frame");
			const unsigned bytes = (written_dr.size() - 66) / 8;
			require(accepted + bytes <= design.size(), "chunk extends beyond design");
			for (unsigned i = 0; i < bytes * 8; i++)
				require(written_dr[64 + i] == ((design[accepted + i/8] >> (i%8)) & 1),
					"SDM data does not match the RBF at the acknowledged position");
			chunks++;
			waiting_for_ack = true;
			flow_status = accepted + bytes;
			if (flow_fault == PARTIAL && chunks == 1) flow_status = accepted + bytes/2;
			if (flow_fault == BUSY) busy_polls = 2;
			if (flow_fault == BUSY_FOREVER) busy_polls = 6001;
			if (flow_fault == ERROR) flow_status |= 3;
			if (flow_fault == FORWARD) flow_status += 4;
			if (flow_fault == BACKWARD && chunks == 2) flow_status = accepted - 4;
			if (flow_fault == STALL) flow_status = accepted;
		}
		if (state == 3) {
			shift.clear();
			written_dr.clear();
			if (sdm && instruction[0] == 0x202) {
				const uint64_t status = mailbox.empty() ? (final_done ? 7 : 3) : mailbox.front();
				if (!mailbox.empty()) mailbox.pop_front();
				append(status, 34);
				append(0, 1);
				return;
			}
			if (sdm && instruction[0] == 0x208) {
				append(flow_status | (busy_polls ? 1 : 0), 37);
				append(0, 1);
				if (waiting_for_ack) {
					flow_reads++;
					if (busy_polls) busy_polls--;
					else {
						waiting_for_ack = false;
						accepted = uint32_t(flow_status) & ~3u;
					}
				}
				return;
			}
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
				copies++;
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
FX2_ll::FX2_ll(uint16_t, uint16_t, uint16_t, uint16_t, const std::string &)
{
	if (++probe.opens == probe.fail_open)
		throw std::runtime_error("mock reopen failure");
}
FX2_ll::~FX2_ll() { probe.closes++; }
bool FX2_ll::clear_halt(uint8_t) { return !probe.halt_failure; }
int FX2_ll::read_ctrl(uint8_t, uint16_t, uint8_t *data, uint16_t size, uint16_t)
{
	memset(data, 0, size);
	return size;
}
int FX2_ll::write(uint8_t, uint8_t *data, uint16_t size)
{
	probe.write_calls++;
	if (probe.fail_write) return -1;
	if (probe.short_write) return size - 1;
	probe.command(data, size);
	return size;
}
static int read_response(uint8_t *data, uint16_t size)
{
	probe.read_calls++;
	if (probe.read_error) return probe.read_error;
	if (probe.timeout_opens && probe.opens <= probe.timeout_opens &&
		probe.read_calls > probe.timeout_after_reads)
		return 0;
	if (probe.empty_reads) {
		probe.empty_reads--;
		return 0;
	}
	if (!probe.read_results.empty()) {
		const int result = probe.read_results.front();
		probe.read_results.pop_front();
		if (result <= 0) return result;
		size = std::min<unsigned>(size, result);
	}
	if (probe.stall_read) return 0;
	if (probe.read_chunk) size = std::min<unsigned>(size,probe.read_chunk);
	require(probe.input.size() >= size, "unexpected USB read length");
	for (unsigned i = 0; i < size; i++) {
		data[i] = probe.input.front();
		if (probe.corrupt_scan && probe.instruction[0] == 1023)
			data[i] ^= 1;
		probe.input.pop_front();
	}
	return size;
}

int FX2_ll::read(uint8_t, uint8_t *data, uint16_t size)
{
	const int ret = read_response(data, size);
	// The original strict read reports a timeout as an error.
	return ret == 0 ? -1 : ret;
}

int FX2_ll::read_quiet(uint8_t, uint8_t *data, uint16_t size, unsigned timeout_ms)
{
	if (timeout_ms == 50) {
		probe.drain_calls++;
		if (!probe.drain_after_copy || probe.copies) {
			if (probe.drain_error) return probe.drain_error;
			if (probe.endless_tdo) {
				memset(data, 0xa5, size);
				return size;
			}
		}
		const auto count = std::min<size_t>(size, probe.input.size());
		for (size_t i = 0; i < count; i++) {
			data[i] = probe.input.front();
			probe.input.pop_front();
		}
		probe.drained += count;
		return count;
	}
	return read_response(data, size);
}

static void startup_drain()
{
	const std::map<uint32_t, misc_device> misc;
	probe = Probe();
	probe.input = {0xba, 0xdc, 0x0f, 0xfe}; // Already on USB.
	probe.pending = {0xde, 0xad, 0xbe, 0xef}; // Still inside the probe.
	{
		Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
			0, "", 0, false, "mock.hex", misc);
		require(j.get_chain_len() == 2, "stale probe data contaminated IDCODE detection");
		require(probe.drained == 8, "startup did not discard both sources of stale TDO");
	}
	require(probe.opens == probe.closes, "successful startup leaked the probe");
	for (unsigned fault = 0; fault < 4; fault++) {
		probe = Probe();
		probe.halt_failure = fault == 0;
		probe.drain_error = fault == 1 || fault == 2 ? LIBUSB_ERROR_NO_DEVICE : 0;
		probe.drain_after_copy = fault >= 2;
		probe.endless_tdo = fault == 3;
		bool failed = false;
		try {
			Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
				0, "", 0, false, "mock.hex", misc);
		} catch (const std::runtime_error &) { failed = true; }
		require(failed, "startup accepted a failed or incomplete TDO drain");
		require(probe.opens == 1 && probe.closes == 1, "startup failure leaked or retried the probe");
		require(probe.read_calls == 0, "startup scanned before the TDO drain finished");
		require(probe.drain_calls < 20, "TDO drain was unbounded");
	}
}

static void initialization_recovery()
{
	const std::map<uint32_t, misc_device> misc;
	probe = Probe();
	unsigned initialization_reads;
	{
		Jtag baseline(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
			0, "", 0, false, "mock.hex", misc);
		initialization_reads = probe.read_calls;
	}
	for (unsigned timeouts : {1u, 3u, 9u, 10u}) {
		// Inject the timeout at every reply boundary, including marker scans.
		for (unsigned preceding_reads = 0; preceding_reads < initialization_reads; preceding_reads++) {
			probe = Probe();
			probe.timeout_opens = timeouts;
			probe.timeout_after_reads = preceding_reads;
			bool failed = false;
			try {
				Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
					0, "", 0, false, "mock.hex", misc);
				require(j.get_chain_len() == 2, "recovery retained partial chain entries");
				j.device_select(1);
				uint8_t ir[2] = {6, 0}, rx[4] = {};
				j.shiftIR(ir, NULL, 10);
				j.shiftDR(NULL, rx, 32);
				require(rx[0] == 0xdd && rx[1] == 0x50 && rx[2] == 0x21 && rx[3] == 0xc3,
					"recovered chain returned a stale or misaligned IDCODE");
			} catch (const std::runtime_error &) { failed = true; }
			require(failed == (timeouts == 10), "initialization retry bound");
			require(probe.opens == std::min(timeouts + 1, 10u), "unexpected probe reopen count");
			require(probe.opens == probe.closes, "probe leaked during recovery");
			require(probe.drained > 0, "recovery did not drain stale TDO");
		}
	}
	probe = Probe();
	probe.read_error = LIBUSB_ERROR_NO_DEVICE;
	bool failed = false;
	try {
		Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
			0, "", 0, false, "mock.hex", misc);
	} catch (const std::runtime_error &) { failed = true; }
	require(failed && probe.opens == 1 && probe.closes == 1, "disconnect reopened the probe");

	probe = Probe();
	probe.timeout_opens = 1;
	probe.fail_open = 2;
	failed = false;
	try {
		Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
			0, "", 0, false, "mock.hex", misc);
	} catch (const std::runtime_error &) { failed = true; }
	require(failed && probe.opens == 2 && probe.closes == 1,
		"failed reopen leaked or reused the closed probe");

	probe = Probe();
	probe.corrupt_scan = true;
	failed = false;
	try {
		Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
			0, "", 0, false, "mock.hex", misc);
	} catch (const std::runtime_error &) { failed = true; }
	require(failed && probe.opens == 1, "corrupted BYPASS readback accepted or blindly retried");

	probe = Probe();
	probe.primary_id = 0x020b10dd; // Cyclone II: preserve existing initialization.
	probe.corrupt_scan = true;
	Jtag other(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
		0, "", 0, false, "mock.hex", misc);
	require(other.get_chain_len() == 2, "Stratix marker checks changed another FPGA's startup");
}

static void read_recovery()
{
	probe = Probe();
	UsbBlasterII cable("mock.hex");
	uint8_t command = 0x4c, reply = 0xff;
	probe.empty_reads = 2;
	require(cable.write(&command, 1, &reply, 1) == 1,
		"transient TDO timeout must allow the queued reply to arrive");
	require(reply == 0 && probe.read_calls == 3, "delayed TDO reply");
	require(probe.write_calls == 2, "read retry reissued JTAG commands or TDO copy");

	probe = Probe();
	probe.state = 4; // Shift-DR, with a known 40-bit reply.
	probe.append(0x67452301, 32);
	probe.append(0x89, 8);
	probe.read_results = {2, 0, 1, 0, 2};
	uint8_t shift[] = {0xc5, 0, 0, 0, 0, 0}, captured[5] = {};
	const uint8_t expected[] = {0x01, 0x23, 0x45, 0x67, 0x89};
	require(cable.write(shift, sizeof(shift), captured, sizeof(captured)) == 5,
		"split TDO reply interrupted by timeouts");
	require(memcmp(captured, expected, sizeof(expected)) == 0,
		"partial TDO data overwritten or skipped after timeout");
	require(probe.write_calls == 2 && probe.read_calls == 5,
		"split TDO reply replayed the scan");

	probe.read_error = LIBUSB_ERROR_NO_DEVICE;
	const auto reads = probe.read_calls;
	require(cable.write(&command, 1, &reply, 1) < 0, "disconnect accepted");
	require(probe.read_calls == reads + 1, "disconnect retried as a timeout");

	probe = Probe();
	probe.stall_read = true;
	require(cable.write(&command, 1, &reply, 1) < 0, "missing TDO reply accepted");
	require(probe.read_calls <= 100 && probe.write_calls == 2,
		"empty TDO reads were unbounded or replayed the command");
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
		const auto opens = probe.opens;
		probe.short_write = short_write;
		probe.stall_read = !short_write;
		try { j.read_write(NULL,bypass,8,false); }
		catch (const std::runtime_error &) { failed = true; }
		probe.short_write = probe.stall_read = false;
		require(failed, "incomplete USB transfer accepted");
		require(probe.opens == opens, "scan outside initialization reopened the probe");
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

static void sdm_chunk_checks()
{
	const auto path = std::filesystem::temp_directory_path()/
		("openfpga-sdm-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".rbf");
	const std::map<uint32_t, misc_device> misc;
	try {
		struct Case { Probe::FlowFault fault; unsigned size; bool done; };
		for (const auto &test : {Case{Probe::NORMAL, 16384, true},
			Case{Probe::BUSY, 16384, true}, Case{Probe::BUSY_FOREVER, 16384, true},
			Case{Probe::PARTIAL, 16384, true}, Case{Probe::ERROR, 16384, true},
			Case{Probe::FORWARD, 16384, true}, Case{Probe::BACKWARD, 16384, true},
			Case{Probe::STALL, 16384, true}, Case{Probe::NORMAL, 16384, false},
			Case{Probe::NORMAL, 0, true}, Case{Probe::NORMAL, 7, true}}) {
			const auto fault = test.fault;
			probe = Probe();
			probe.flow_fault = fault;
			probe.final_done = test.done;
			probe.design.resize(test.size);
			for (size_t i = 0; i < probe.design.size(); i++) probe.design[i] = uint8_t(i * 73 + i/256);
			std::ofstream file(path, std::ios::binary);
			file.write(reinterpret_cast<const char *>(probe.design.data()), probe.design.size());
			file.close();
			Jtag j(cable_list.at("usb-blasterII"), NULL, "", "", 24000000,
				0, "", 0, false, "mock.hex", misc);
			j.device_select(1);
			probe.sdm = true;
			probe.mailbox = {3, 0x3c0004001, ((0x208adc8a6ull << 2) | 3) & 0x3ffffffffull,
				3, 0x4001, 1, 1, 1, test.done ? 7u : 3u};
			Altera fpga(&j, path.string(), "rbf", Device::WR_SRAM, "", "", true, -1, "", false, false);
			bool failed = false;
			try { fpga.program(0, false); }
			catch (const std::runtime_error &) { failed = true; }
			const bool invalid_input = !test.size || (test.size & 3);
			require(failed == (invalid_input || !test.done ||
				(fault != Probe::NORMAL && fault != Probe::PARTIAL && fault != Probe::BUSY)),
				"SDM chunk acceptance result");
			require(probe.flow_reads >= probe.chunks, "not every chunk received an SDM status check");
			if (fault == Probe::BUSY)
				require(probe.flow_reads == probe.chunks * 3, "busy chunk was not polled until accepted");
			if (fault == Probe::BUSY_FOREVER)
				require(probe.chunks == 1 && probe.flow_reads <= 6000, "busy polling was unbounded");
			if (!failed) {
				require(probe.accepted == probe.design.size(), "configuration finished before all data was accepted");
				require(probe.mailbox.empty(), "configuration skipped final CONFIG_STATUS validation");
			}
			if (fault == Probe::ERROR || fault == Probe::FORWARD)
				require(probe.chunks == 1, "data sent after an invalid chunk acknowledgment");
			if (fault == Probe::BACKWARD)
				require(probe.chunks == 2, "backward acknowledgment caused old data to be resent");
			if (fault == Probe::STALL)
				require(probe.chunks <= 128, "unaccepted chunk retried indefinitely");
			if (invalid_input)
				require(probe.chunks == 0 && probe.mailbox.size() == 9,
					"invalid RBF entered SDM configuration mode");
			if (!test.done)
				require(probe.accepted == probe.design.size(),
					"missing CONF_DONE was not tested after all chunks were accepted");
		}
	} catch (...) {
		std::filesystem::remove(path);
		throw;
	}
	std::filesystem::remove(path);
}

int main()
{
	try {
		board_defaults();
		read_recovery();
		startup_drain();
		initialization_recovery();
		sdm_chunk_checks();
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
