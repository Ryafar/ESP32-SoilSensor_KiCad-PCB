/* Bring-up task: WiFi scan, to answer whether the on-board ceramic antenna and
 * its matching network (L1-L4, see docs/hardware/hardware-info.md §1) actually work.
 *
 * Scans every channel the configured country allows, repeats on a timer, and
 * logs each AP with its RSSI plus a summary line. The scan is active, so it
 * sends probe requests and waits for responses: a scan that finds anything has
 * exercised both the transmit and the receive path.
 *
 * What to look for:
 *   - The summary's strongest RSSI is the number that matters. Hold a phone in
 *     the same spot, look at the same AP, and compare. Within a few dB is a
 *     working antenna; consistently 15-25 dB down points at the antenna or the
 *     matching network, not at software.
 *   - AP count against what the phone lists. Seeing a third of them is the same
 *     symptom in a different form.
 *   - Move the board and watch RSSI change. A number that does not respond to
 *     distance or orientation is not coming from the antenna at all.
 *   - The logged country code decides which channels get scanned. Before
 *     concluding an AP is missing, check it is not sitting on channel 12 or 13
 *     while the country policy stops at 11.
 *   - Nothing here is a clean-room measurement. The board's own orientation,
 *     your hand, and whatever it is lying on all move RSSI by several dB, so
 *     compare like for like and repeat before believing a difference.
 *
 * Transmit bursts pull a few hundred mA. If the board browns out during a scan,
 * or the charger starts flagging VINDPM/VDPPM (see charger_test_task and
 * docs/hardware/hardware-info.md §4.4), that is a supply problem and says nothing
 * about the antenna.
 *
 * Owns the whole WiFi lifecycle - NVS, netif, event loop, driver - because
 * nothing else in this build uses the radio. That is a bring-up shortcut; the
 * application will have main own these, per docs/firmware-architecture.md.
 *
 * Never returns, unless WiFi fails to start at all.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void wifi_test_task(void *arg);

#ifdef __cplusplus
}
#endif
