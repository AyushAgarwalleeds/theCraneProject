# Crane Project ESP32 bench simulator

For the physical relay bench firmware, browser installer, and full sender pinout, see [CraneLink setup and wiring](CraneLink_PhoneBench/README.md). The older simulator is described below.

One Arduino sketch runs on two classic ESP32-WROOM-32 development boards. The sender has six motion inputs, AUX, simulated stop and a mode button. The receiver has its own mode button and reports simulated commands over USB serial. The boards communicate with each other exclusively through ESP-NOW. In Wi-Fi mode each board hosts its own local setup page, and the receiver also hosts a phone simulation page.

**Use on an isolated USB bench only. There are no relay or actuator outputs. Keep both boards disconnected from the crane, pendant and 110 VAC circuit.**

## Flash the boards

1. Install the [Arduino ESP32 board package](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html) (target 3.x). Open `CraneBench/CraneBench.ino` and choose the matching board, or **ESP32 Dev Module** for a compatible generic board.
2. In `CraneBench/BenchConfig.h`, set your private setup password (8–63 characters) and replace the example encryption keys. Match both keys on the two boards and keep a private copy for replacements. Use a different key pair for each independent bench pair.
3. Set `CRANE_BENCH_RECEIVER` to `0` and upload to the sender. Set it to `1` and upload to the receiver. To keep receiver setup but remove its phone simulation page, set `CRANE_BENCH_PHONE` to `0` for the receiver build.

Upload this revision to both boards before pairing. It uses a newer ESP-NOW message format than the earlier sketch.

Each bench switch closes its GPIO to **GND on the same board**. Use normally open switches for this setup. The inputs use the ESP32's internal pull-ups. The eight sender switches, pairing button, and one shared ground account for the ten sender connections.

| Sender input | GPIO | Default simulated action |
| --- | ---: | --- |
| Button 1 | 4 | Up |
| Button 2 | 32 | Down |
| Button 3 | 33 | Left |
| Button 4 | 25 | Right |
| Button 5 | 26 | Front |
| Button 6 | 27 | Back |
| Button 7 | 14 | Simulated stop |
| Button 8 | 13 | AUX |
| Pair / mode | 23 | Short press requests a receiver mode change |
| Common | GND | Shared switch return |

The receiver bench wiring is **GPIO23 to GND through its pairing button** and USB for power and serial. GPIO23 is the default on both boards and can be changed in `BenchConfig.h`.

## Pair and operate

1. Connect both boards to USB. A board without a saved peer starts its Wi-Fi access point. A board with a saved peer starts in ESP-NOW mode. To enter local setup on an already paired sender, hold its pairing button for about 1.2 seconds, then release. On the receiver, tap its pairing button. The serial monitor at **115200 baud** shows the access point name, starting with `CraneBench-TX-` or `CraneBench-RX-`.
2. Join each network in turn and open **http://192.168.4.1/**. Stay connected if the phone reports “no internet.” Record each page's **My station MAC**. Enter the *other board's* station MAC on each setup page.
3. Save the same **pair ID** (a positive number) and **channel** (1–11) on both boards. The pair ID is a logical identifier, and the channel selects the 2.4 GHz frequency. Use distinct IDs and keys for separate pairs. In the receiver setup page, assign one action to each sender button. To make the physical AUX button command Up, swap those two assignments. Settings survive power loss. Tap each board's local pairing button to exit setup and apply the saved radio settings.
4. Release all sender switches, then hold a motion or AUX button. The receiver prints a simulated mask. Up, down, left, right, front, back, and AUX use `0x01`, `0x02`, `0x04`, `0x08`, `0x10`, `0x20`, and `0x40` respectively. Different axes may run together. If the receiver sees one direction first, it keeps that direction while held and ignores the opposite direction. If both appear together, the axis stays idle. In either conflict, release both before reversing.
5. For phone jog, tap the receiver pairing button or briefly tap the sender pairing button while it has a radio link. Join the RX Wi-Fi, open the setup page, select **Open phone simulation**, and tap **Enable simulation**. The phone jogs one action at a time. In receiver Wi-Fi mode, ESP-NOW remains available on the same channel to receive paired mode requests. Only the phone can jog in this mode. A second short sender press can return the receiver to ESP-NOW mode.
6. To try the simulated stop, press the mapped sender stop switch in ESP-NOW mode or tap **Simulated stop** in an enabled phone session. Release the switches and send **R** to the receiver's serial monitor to reset the latch. The latch survives mode changes and clears on receiver reboot. Start from neutral after reset.
7. To try the simulated feedback fault, send **F** in the receiver serial monitor. This clears all simulated commands and latches stop. **R** is blocked while the injected fault is present. Send **C** to clear the injected fault and then **R** to reset.

A receiver with a saved peer stays in ESP-NOW mode and keeps looking for that peer while it is offline. Use the receiver's physical pairing button to enter Wi-Fi while the sender is unavailable. To return to standalone Wi-Fi on startup, clear the receiver's peer MAC in setup. For a replacement, flash the correct role and keys, then update each board's settings. The surviving board needs the replacement's station MAC.

## Bench checks

| Try | Expected simulated behavior |
| --- | --- |
| Hold a sender button before enabling the radio pair | Starts idle. Release it and press again to command. |
| Hold a channel and remove sender power | The receiver's session expires and returns to idle. |
| Restore power while a motion button is held | A new session requires neutral before it accepts a command. |
| Hold Up, then press Down while keeping Up held | Up continues. Releasing Up while Down is still held leaves the axis idle. Release both before the next command. |
| Hold Up and Left together | Both axes can be simulated at the same time. |
| Change the channel or pair ID on one board | The boards stop exchanging valid commands. Restore the match to resume. |
| Change receiver mode during a hold | The receiver clears its simulated command and starts a new session. |
| Leave the phone page while holding a channel | The phone stops refreshing the session. The receiver returns to idle when it expires. |
| Trigger simulated stop, then try a motion | Motion stays idle until reset from the receiver serial monitor. |
| Send F, then R, then C and R | The first reset is blocked. Clearing the injected fault allows the second reset. |

The receiver uses a new session nonce after link expiry and accepts increasing command sequence numbers. The simulated lease is 350 ms from the last accepted command. Packet loss, Wi-Fi and HTTP delays, and software stalls make this unsuitable as a safety stop timer.

For a real retrofit, use a safety-rated crane radio integrated with the crane's independent stop circuit. A GPIO command does not reveal the actual state of relay contacts. Detecting a welded contact requires separate monitored feedback and an independent way to stop motion. The bench fault command only tests the simulated response. Have a qualified crane controls engineer review each OEM schematic and specify the safety system. [IEC 62745](https://webstore.iec.ch/en/publication/31998) covers cableless machinery controls.
