/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host-lane characterization + fault-injection tests for the file-transfer
 * protocols, driven through ttpfile_harness (a fake TFileIO/TComm). These pin
 * the EOF/error conflation in the TFileIO read path: today a failed read and a
 * clean EOF both surface as a 0-byte return, so a mid-transfer I/O error is
 * silently completed as success. The clean-EOF cases are the behavioral
 * equivalence anchor (must stay byte-identical across the fix); the error-inject
 * cases assert the contract the fix establishes — an I/O error is never reported
 * as a successful transfer.
 */
#include "catch_amalgamated.hpp"

#include "ttpfile_harness.h"

namespace {

constexpr unsigned char SOH = 0x01;
constexpr unsigned char EOT = 0x04;
constexpr unsigned char PAD = 0x1A;
constexpr unsigned char CAN = 0x18;

unsigned char synth(size_t i)
{
	return static_cast<unsigned char>(i * 7 + 1);
}

// The XMODEM cancel sequence XCancel_ emits is five CANs; finding it on the wire
// proves the transfer actively aborted rather than ending as a normal EOT.
bool has_cancel_run(const TtpResult &r)
{
	int run = 0;
	for (int i = 0; i < r.tx_len; i++) {
		run = (r.tx[i] == CAN) ? run + 1 : 0;
		if (run >= 5) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("xmodem send streams a clean file to a well-formed transcript", "[ttpfile][xmodem]")
{
	TtpResult r;
	ttp_xmodem_send(/*file_len=*/200, /*fail_at=*/static_cast<size_t>(-1), &r);

	REQUIRE(r.rc == 0);
	CHECK(r.io_error == 0);
	CHECK(r.success == 1);

	// Two 128-byte checksum packets (3 header + 128 data + 1 checksum) then EOT.
	REQUIRE(r.tx_len == 3 + 128 + 1 + 3 + 128 + 1 + 1);

	CHECK(r.tx[0] == SOH);
	CHECK(r.tx[1] == 1);
	CHECK(r.tx[2] == static_cast<unsigned char>(~1));
	for (int i = 0; i < 128; i++) {
		CHECK(r.tx[3 + i] == synth(static_cast<size_t>(i)));
	}

	CHECK(r.tx[132] == SOH);
	CHECK(r.tx[133] == 2);
	CHECK(r.tx[134] == static_cast<unsigned char>(~2));
	for (int i = 0; i < 72; i++) {
		CHECK(r.tx[135 + i] == synth(static_cast<size_t>(128 + i)));
	}
	for (int i = 72; i < 128; i++) {
		CHECK(r.tx[135 + i] == PAD); // short final packet is 0x1A padded
	}

	CHECK(r.tx[264] == EOT);
}

TEST_CASE("xmodem send does not report success when a read error truncates the file", "[ttpfile][xmodem]")
{
	TtpResult r;
	ttp_xmodem_send(/*file_len=*/200, /*fail_at=*/150, &r);

	REQUIRE(r.rc == 0);
	REQUIRE(r.io_error == 1); // the fake's error-read path did fire mid-transfer

	// The contract the fix must establish: a mid-transfer read failure aborts the
	// transfer rather than being padded out and reported as a completed send.
	CHECK(r.success == 0);
	// ...and aborts actively — the cancel sequence goes on the wire, no EOT.
	CHECK(has_cancel_run(r));
	CHECK(r.tx[r.tx_len - 1] != EOT);
}
