# CraneLink phone and ESP-NOW relay controller

This sketch builds two roles for classic ESP32-WROOM boards. The receiver operates the physical relay module from the phone or an ESP-NOW handheld. Keep the relay contacts disconnected from the crane and the 110 V circuit.

## USB setup website

Open [CraneLink USB Setup](https://ayushagarwalleeds.github.io/theCraneProject/) in desktop Chrome or Edge. If you have a local copy of the repository, run `node tools/serve-wizard.mjs` from its root and open the printed localhost address. A USB data cable is required. Close Arduino Serial Monitor before using the site.

1. Choose how many senders to install, from one to four, and click **Start setup**.
2. Connect the receiver by USB. Click **Connect and upload**, then pick its COM port in the browser. The receiver gets the relay firmware and a new set of sender pairings. Existing sender pairings are replaced.
3. Unplug the receiver when its step is complete. Connect the first sender, click **Connect and upload**, and select its port. The wizard saves its own pairing identity and verifies it after restart. Repeat for each sender.
4. Power the boards. Test them one at a time, with the relay contacts disconnected from the crane. If USB setup stops after flashing, reconnect that board and use **Retry pairing only** in the same tab.

The wizard supports classic ESP32-WROOM with 4 MB or more flash. It does not use Arduino IDE during installation. Pairing keys are generated in the browser and saved on the boards over USB.

## Flash the receiver manually

1. Open `CraneLink_PhoneBench.ino` from this folder in Arduino IDE. Select **Tools → Board → esp32 → ESP32 Dev Module** with the Espressif Arduino 3.x core. Set `CRANELINK_SENDER` to `0` in **BuildRole.h**, then upload to the receiver.
2. Open Serial Monitor at **115200 baud**. If you missed the startup output, press the board's **EN** or **RESET** button. The output shows its `CraneLink-Bench-…` network name and its generated Wi-Fi password. The password is normally kept across reboots.
3. Join that Wi-Fi on your phone. If prompted, choose to stay connected without internet. Type **http://192.168.4.1/** into the browser address bar.
4. Tap **Web Run** and hold one direction. Its assigned relay and R8 should energize. Both should release when you lift your finger. **Stop relays** turns all eight outputs off and latches stop. **Reset stop** clears the latch. Tap Web Run again before starting another jog.

Use the relay module's specified external supply. For a module with non-isolated logic inputs, connect the module's input ground to ESP32 GND and use inputs compatible with 3.3 V logic. Do not feed 5 V into the ESP32 GPIOs. The sketch defaults to active-low inputs, as in the original code. For active-high inputs, set `relayActiveLow` to `false` in `RelayController.h` before flashing.

| Relay input | ESP32 GPIO | Function |
| --- | ---: | --- |
| IN1 | 26 | Up |
| IN2 | 21 | Down |
| IN3 | 33 | Left |
| IN4 | 32 | Right |
| IN5 | 27 | Front |
| IN6 | 14 | Back |
| IN7 | 13 | AUX latch |
| IN8 | 4 | Enable during a held directional jog |

If you wired an earlier revision, turn off the ESP32 and relay module before moving the IN2 signal wire from GPIO25 to GPIO21.

## AUX and settings

- Hold **AUX** for three seconds and release. Its assigned relay stays on. Release the button before starting another three-second hold to turn it off. The phone shows the hold progress and the idle countdown.
- AUX defaults to **60 minutes of inactivity**. Accepted directional jogs and AUX holds count as activity. An open phone page and idle ESP-NOW packets do not count. Releasing a jog, turning off Web Run, or losing the link drops motion and R8. AUX stays in its latched state until toggled off, timed out, stopped, or the receiver reboots.
- Open **Settings and sender pairing** to set an AUX timeout from **1 to 720 minutes** and assign **Up, Down, Left, Right, Front, Back, and AUX** to R1–R7. Use each relay once. R8 is reserved. Turn Web Run off and release the sender before saving. Turn AUX off before changing assignments or pairing. Settings survive a reboot.
- **All directions together** is the default for the disconnected relay bench. You can hold multiple phone buttons with multiple fingers or hold all six physical direction switches. **NO/NC interlock** means one direction per axis in software. Up/Down, Left/Right and Front/Back are separate pairs. If one direction in a pair is already held, it takes priority. If both arrive together the pair stays off. Release both before reversing after a conflict. This setting does not change electrical trigger polarity or the physical NO and NC contacts.
- To move Up from R1 to R3, select **R3** beside **Up**. The page swaps the previous R3 assignment to R1 automatically. Click **Save relay settings**, then test Up. Relay assignments and sender pairing are saved separately.
- Only one source can jog at a time. A held sender command keeps Web Run unavailable until released. An enabled Web Run session owns jog control until turned off or expired. If you press a sender button while Web Run owns control, release and press it again after Web Run ends. Both sources can stop the relays.

The ESP32 checks the 650 ms jog timeout in a separate task. **Commanded relays** and the serial monitor show the driven outputs. Check the actual contacts separately with a continuity meter.

## Wire and pair a sender manually

1. Set `CRANELINK_SENDER` to `1` in **BuildRole.h**, select the sender's USB port, and upload the same sketch. Use the GPIO labels printed on the board. Each momentary bench switch connects its GPIO to the **sender's GND** when pressed. The sketch enables the internal pull-ups.

   | Button | Sender GPIO |
   | --- | ---: |
   | Up | 4 |
   | Down | 32 |
   | Left | 33 |
   | Right | 25 |
   | Front | 26 |
   | Back | 27 |
   | AUX | 13 |
   | Stop | 14 |

2. In the sender's Serial Monitor, choose **115200 baud** and **New Line** or **Both NL & CR**. Send `info` to display the sender's station MAC address.
3. On the phone connected to the receiver's Wi-Fi, open **Settings and sender pairing** and choose an empty **Sender slot**. Enter that sender's station MAC, tap **Generate pairing key**, and copy the 32-character key privately. Click **Save sender slot**. Also copy the **receiver station MAC** shown on that page.
4. In the sender's Serial Monitor, enter `pair RECEIVER_MAC 32_HEX_KEY`, replacing both placeholders with the values from the page. Keep the spaces between the command, MAC, and key. The sender saves the pairing and prints **Receiver link found** when it hears the receiver. Both use Wi-Fi channel 6. Close and reopen Settings to refresh the link status.
5. Turn Web Run off and release all sender buttons before testing. Hold Up and check its assigned relay plus R8, then release. Hold AUX for three seconds, release, and repeat to toggle it off. If the sender Stop button is pressed, release it before tapping **Reset stop** on the phone.
6. Repeat for additional senders, with a separate slot and key for each. The phone or one sender owns the jog at a time. A second sender whose first press is blocked must release and press again to take over.

## Replace a board

| Replacement | What to do |
| --- | --- |
| Sender | Flash the sender role. Get its new station MAC with `info`. On the receiver page, choose the old sender's slot, replace its MAC and generate a new key. Save the slot, then enter `pair RECEIVER_MAC 32_HEX_KEY` on the new sender. |
| Receiver | Flash the receiver role and join its Wi-Fi using Serial Monitor. Restore the relay settings. For each sender, get its station MAC with `info`, save its slot with a new key, and run `pair RECEIVER_MAC 32_HEX_KEY` on that sender. You can also restart the USB wizard with all boards. |

Record the relay assignments and AUX timeout if you want to reproduce them on a replacement receiver.

| Bench check | Expected result |
| --- | --- |
| Hold a direction, then lift or slide the finger off it | The selected relay and R8 release. |
| Hold all six directions in default mode | All six direction relays and R8 energize. Releasing one drops its relay. |
| Enable NO/NC interlock. Hold Up, then touch Down | Up remains active. Lift both before starting a new hold for Down. Right and Back can run at the same time as Up. |
| Switch apps or turn off phone Wi-Fi during a jog | All relays release. Reopen or reconnect, then explicitly enable Web Run. |
| Open the page on a second tab or device | The second tab reports that Web Run is in use. |
| Tap Stop relays during a jog, then reset it | All relays release. Web Run stays off after reset. |
| Set AUX timeout to one minute, then hold AUX for three seconds and release | AUX stays on without R8. It turns off after one idle minute. |
| Power off the sender while holding a direction | The assigned relay and R8 release. A previously latched AUX follows its own idle timer. |

## Findings in the supplied sketch

- The supplied receiver reopened pairing after a silent sender, which blocked phone jogs. The new phone access point stays available even if the handheld is absent or ESP-NOW fails to initialize.
- Presses, releases, and heartbeats can overlap. A delayed press can arrive after a release. Web Run off also leaves the old command mask intact, and the status poll can enable Web Run in a second browser. The phone bench uses one session per browser, numbered requests, and receiver-acknowledged state.
- Configuration reuses the same field for the current PIN and the replacement PIN, and the UI accepts longer PINs than firmware can store. Some status fields are also inserted as HTML. The phone bench has a generated Wi-Fi password and displays status as text.
- The original relay test path could hold a coil on independently of jog handling. The current relay driver uses the same timeout and stop behavior for every command.
- The original ESP-NOW callbacks modified shared state and saved settings directly in the Wi-Fi task. The current callback queues received frames. The receiver uses the configured sender MAC and key.
