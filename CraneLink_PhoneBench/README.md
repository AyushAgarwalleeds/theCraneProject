# CraneLink phone and ESP-NOW relay controller

This sketch builds two roles for classic ESP32-WROOM boards. The receiver operates the physical relay module from the phone or an ESP-NOW handheld. Keep the relay contacts disconnected from the crane and the 110 V circuit.

## Release 8 and daily startup

Update installed receivers first, then their handhelds, using the firmware update buttons. Saved pairings stay in place. A release 8 handheld requires a release 8 receiver for motion and Power. A release 8 receiver also sends the original protocol 4 beacons for older handhelds during the upgrade. Every receiver reboot starts with all outputs commanded off and Stop latched. Release all buttons, hold **Power + Emergency Stop** for three seconds and release both, or use **Reset Emergency Stop** on the receiver page. Give a fresh command after reset. A radio reconnection alone does not require a Stop reset unless Stop was latched.

The handheld accepts advancing, authenticated command receipts from its selected receiver. A short Stop press sampled by a powered handheld stays pending for that receiver until it acknowledges the Stop. Loss of sender power clears its pending requests. USB diagnostics report whether the receiver is heard, whether it acknowledged packets, and whether Stop delivery is pending. Command receipts describe software state. Check physical contact opening and stopping time separately.

## USB setup website

Open [CraneLink USB Setup](https://thecraneproject.vercel.app/) in desktop Chrome or Edge, or Android Chrome on a phone with USB host support. Use a data cable plugged into the ESP32 development board. Some phones require a USB OTG adapter. Keep Chrome in the foreground during upload. The Android path supports common CP210x, CH340/CH341 and FTDI bridges through WebUSB. Use desktop Chrome or Edge for USB setup if your phone cannot see the board or you use an iPhone or iPad. If you have a local copy of the repository, run `node tools/serve-wizard.mjs` from its root and open the printed localhost address. Close Arduino Serial Monitor before using the site. [ESPHome USB bridge support](https://github.com/esphome/esp-web-tools/pull/750)

1. Choose how many senders to install, from one to four, and click **Start setup**.
2. Connect the receiver by USB. Click **Connect and upload**, then pick its COM port in the browser. The receiver gets the relay firmware and a new set of sender pairings. Existing sender pairings are replaced.
3. Unplug the receiver when its step is complete. Connect the first sender, click **Connect and upload**, and select its port. The wizard saves its own pairing identity and verifies it after restart. Repeat for each sender.
4. Power the boards. Test them one at a time, with the relay contacts disconnected from the crane. If USB setup stops after flashing, reconnect that board and use **Retry pairing only** in the same tab.

The wizard supports classic ESP32-WROOM with 4 MB or more flash. It does not use Arduino IDE during installation. Pairing keys are generated in the browser and saved on the boards over USB.

The installer shows the receiver's Wi-Fi name, password, and phone controls address after USB setup. **Download PDF** saves the board addresses and receiver Wi-Fi details on your computer. Keep the report private.

## Master sender

### Add receivers at their installed locations

1. On the [CraneLink installer](https://thecraneproject.vercel.app/#wireless), connect the handheld by USB and choose **Update handheld firmware**. Keep its **Station MAC**, **Setup Wi-Fi name**, and **Setup Wi-Fi password** from the report. This step is needed once for the handheld. Update installed receivers to release 8 before using the updated handheld.
2. With the receiver idle, join its Wi-Fi and open `http://192.168.4.1/`. Open **Settings and sender pairing**. Choose an **Empty slot**, or the existing slot for this same handheld. Paste the handheld’s **Station MAC** into **Sender station MAC**. Tap **Generate pairing key**. Copy the **Shared key** and **Receiver station MAC** before tapping **Save sender slot**. Keep the key private. Each receiver needs its own key.
3. Release all buttons. Hold **Emergency Stop alone for eight seconds**, then release to open setup. This also latches Stop on the selected receiver. The LED gives a repeating double flash as the setup Wi-Fi runs. Join the handheld’s **CraneLink-TX-…** network using its setup password. If the phone reports no internet, stay on this Wi-Fi. Open or reload `http://192.168.4.1/`. Confirm the header says **Handheld setup**.
4. Enter a label such as `MH14AB4292`, the **receiver station MAC**, and the **same shared key** from that receiver. Tap **Save receiver** and check the order shown. To add another, join that receiver’s Wi-Fi and repeat the receiver step, then reconnect to handheld Wi-Fi and add the entry. The same receiver MAC updates its entry in place.
5. Tap **Finish and return to radio**, then disconnect the phone from receiver Wi-Fi. Reset Stop by holding Power + Stop for three seconds, then release both. On a disconnected relay bench, confirm the selected receiver when switching and verify the other receivers don’t respond.

Handheld setup pauses control and returns to ESP-NOW after 15 minutes without setup activity. To reopen it without a power switch, hold Stop alone for eight seconds, then release. A receiver must have an available sender slot or a slot for this handheld. Keep each receiver’s own pairing key with its label until its handheld entry is saved.

### Pair over USB

1. Open **Master sender** and choose two to four receivers. Connect the handheld by USB and click **Read sender over USB**. If prompted, use **Upload sender firmware** for this board.
2. Unplug the handheld. Connect one configured receiver at a time and click **Add connected receiver**. Connect them in the order you want to select them. Each receiver needs one free sender slot or a slot for the same handheld. The wizard renews the key if the handheld is already saved there and keeps the other sender slots.
3. Reconnect the same handheld and click **Save to master sender**. Its new list replaces its previous receiver list. Complete the wizard in the same tab. The website shows the final order using the receivers’ Wi-Fi names, and USB diagnostics shows the currently selected receiver.
4. Power the handheld and receivers. The handheld starts on receiver 1. With all other buttons released, tap **Power** for less than 0.3 seconds, then press it again within 0.6 seconds and hold it for two seconds. Release it. Count the flashes on the status LED to identify the new receiver. Wait for solid light, then give a fresh command. The sequence cycles back to receiver 1 after the last receiver.

The handheld sends a release to the previous receiver and waits one second before changing its target. The handset does not advance automatically if the selected receiver is out of range. A regular three-second hold of **Power** still toggles Power, and **Power + Emergency Stop** still resets a latched stop after release. Emergency Stop from the handheld addresses the selected receiver. Label each bench with its Wi-Fi name and check the selected receiver before pressing a direction.

## Handheld LED, buzzer and battery

| Function | Sender pin | Connection |
| --- | --- | --- |
| Status LED | GPIO23 | GPIO → 330 Ω to 1 kΩ resistor → LED anode. LED cathode → GND. |
| Sound | GPIO18 | Use a small passive piezo rated for 3.3 V drive. For a powered buzzer or magnetic speaker, use a driver and its rated supply. |
| Pack voltage | GPIO34 | Protected pack OUT+ → 10 kΩ → GPIO34 → 10 kΩ → OUT− / ESP32 GND. Use the pack side of the regulator. A 100 nF capacitor from GPIO34 to GND helps filter noise. |

| Indication | LED | Buzzer |
| --- | --- | --- |
| Receiver selected | One to four flashes, matching the configured order | Link tone follows when heard |
| Searching | Short flash about once a second | Periodic search tone |
| Receiver acknowledged commands | Solid | One short beep on connection |
| Link lost | Rapid flash | Two short beeps on loss |
| Low battery | A burst of rapid flashes over the link pattern | Three beeps every 30 seconds |

After wiring the divider, connect the sender by USB and enable **USB diagnostics → Sender battery monitoring**. Check the reported voltage against a multimeter. The warning starts at approximately 3.50 V under load and clears above about 3.65 V. A sender powered only by USB can show that it is waiting for valid pack voltage.

The pictured USB-C board appears to be a TP4056-type 1S charger. Confirm that your board has separate **B+**, **B−**, **OUT+**, and **OUT−** pads. Use a matched 1S parallel pack rated for 4.2 V charging. The pack connects to B+ and B−. The sender’s regulator and divider use the protected OUT+ and OUT−. Use this diagram with a suitable common-ground boost converter. [TP4056 datasheet](https://www.toppwr.com/uploadfile/file/20240130/65b892bb04a3c.pdf)

```text
Protected OUT+ ── sender power switch ──┬──── boost IN+
                                       │
                                      10 kΩ
                                       │
                                       ├──── GPIO34 (use the junction of the two resistors)
                                       │
                                      10 kΩ
                                       │
Protected OUT− ────────────────────────┴──── boost IN− / ESP32 GND

Boost OUT+ (set to 5.0 V) ── ESP32 development board 5V input
Boost OUT− ───────────────── ESP32 GND
GPIO23 ── 470 Ω resistor ──── LED anode (+) / LED cathode (−) ── GND
GPIO18 ───────────────────── passive 3.3 V piezo (+) / piezo (−) ── GND
```

1. Use two equal **10 kΩ** resistors. Add an optional **100 nF** ceramic capacitor from the GPIO34 junction to GND. With the sense wire disconnected from the ESP32, put the meter’s black probe on OUT− and red probe on the resistor junction. The junction should read half the voltage across protected OUT+ and OUT−. For a **4.20 V** pack, expect about **2.10 V** at the junction. [Espressif ADC hardware guide](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/schematic-checklist.html#adc)
2. Set and measure the boost output at **5.0 V** before attaching the ESP32. Choose a converter specified for the full 1S pack range, including below the 3.50 V warning threshold. If the pictured converter is stamped **XL6009**, its specified input starts at **5 V**, so use a 1S-rated converter. Switch the sender off while charging through the TP4056 board. Turn the battery switch off before powering the ESP32 through its USB port. [XL6009 datasheet](https://www.mantech.co.za/Datasheets/Products/xl6009.pdf) · [Espressif DevKitC power inputs](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html#power-supply-options)
3. The LED’s longer lead is normally the anode. The shorter lead or flat side is the cathode. Use a series resistor between GPIO23 and the anode. Connect the piezo’s marked positive lead to GPIO18 and its other lead to GND. A magnetic or powered buzzer, including a 5 V buzzer, needs a driver sized for its current and its own rated supply.
4. Power the sender by USB with the battery switch off to enable **USB diagnostics → Sender battery monitoring**. Then unplug USB and turn on the pack to check the light and sound. To view the pack voltage over USB during a bench check, disconnect the converter’s 5 V output from the ESP32 first. Keep the sense divider and common ground connected and compare the diagnostic reading with the multimeter.

The default Wi-Fi name is a 10-character code derived from the receiver’s MAC. To use a name such as `MH14AB4292`, open **Receiver Wi-Fi** in the installer, connect the receiver by USB, and disconnect the phone from receiver Wi-Fi. Enter a name and password, then click **Apply over USB**. The receiver restarts with the new Wi-Fi settings. Give each receiver a different name if you choose your own. Use a freshly generated password of 8–63 printable characters without spaces.

## Everyday use

1. Power both boards with the phone disconnected from the receiver’s Wi-Fi. Release all handheld buttons, reset Stop after receiver startup, then hold a direction. The phone page can stay closed.
2. For phone control, join the receiver’s Wi-Fi and open **http://192.168.4.1/**. Enable **Web Run** before jogging. When finished, disconnect from this Wi-Fi to return control to the handheld. Release any held button and press again.
3. Hold **Power On/Off** for three seconds and release. Hold it again for three seconds to turn it off. The Power relay also turns off after inactivity or if the controlling link drops.
4. Press **Emergency Stop** to turn off all eight outputs. To reset using the handheld, release all buttons, hold **Power On/Off + Emergency Stop** together for three seconds, then release both. Reset clears the latch with all outputs off. Press a direction again to jog. You can also use **Reset Emergency Stop** on the phone after releasing the handheld Stop button.

The sender listens for the receiver automatically after a signal loss. Motion and Power stop when command packets expire. Once the link returns, release all buttons before issuing a new command. The ESP32 Wi-Fi access point can take up to about ten seconds of inactivity to notice a phone that went out of range. The separate 650 ms command timeout stops the outputs while it waits. The Stop function is for the disconnected bench. Keep an independent, safety-rated emergency stop circuit for crane service.

### Long idle periods and out-of-range recovery

- Use **Update receiver firmware** first, then **Update handheld firmware**. For a blank replacement choose **Install on a new handheld**. These update buttons retain NVS pairing, Wi-Fi and relay settings; **Install and pair** replaces the pairing list.
- While linked, the handheld keeps Wi-Fi sleep disabled. It uses an 80 MHz CPU and sends idle keepalives every 125 ms, with changes sent immediately after input debounce and active commands refreshed every 65 ms.
- After 60 seconds without a receiver and no button activity, it turns the radio off for 2.2 seconds between listening windows of at least 0.8 seconds. The processor keeps checking buttons. It reconnects automatically when the selected receiver returns, normally within a few seconds. Any button wakes the search; use a **Power tap**, release every button, then make a fresh command. The waking press cannot start a relay command. A linked sender never enters this standby.
- Failed radio initialization is retried. Three immediate send API errors restart the radio. A subscribed 8-second task watchdog reboots a stalled main loop. These mechanisms cannot recover a battery cutoff or a failed supply.
- To open handheld setup without a power switch, release all buttons, hold **Emergency Stop alone for 8 seconds**, then release. This stops and latches the selected receiver. Exit with a **Power tap and release** or **Finish and return to radio**. After 15 minutes without setup activity it returns to radio automatically, keeping the selected target. Reset Stop with Power + Stop for 3 seconds, then release both. Holding Power for 3 seconds at startup remains supported.
- USB diagnostics now show sender uptime, last reset reason, radio starts and free heap. Repeated brownouts need a supply investigation. A two-cell parallel pack remains a 1S pack; an XL6009 needs at least 5 V input according to its datasheet and is unsuitable for this pack. Use a boost supply rated across the full 1S discharge range and measure regulated 5 V at the ESP32 under radio load.

Continuous operation does not itself imply ESP32 wear. Runtime recovery does not rewrite pairing flash. Battery endurance, supply stability and enclosure temperature still require a physical all-day test; neither battery runtime nor crane-service reliability is certified by a successful build.

Use **USB diagnostics → Check connected board** to check the boards after flashing. Power both boards while checking ESP-NOW. For the sender, the display shows the paired receiver address, whether it hears the receiver, and which inputs are low. For the receiver, it shows Wi-Fi and sender status, uptime and the last reset reason. A low input should appear only while you hold its button. If the receiver's Wi-Fi disappears and its uptime repeatedly resets, check power and read the last reset reason with the relay module disconnected.

## Flash the receiver manually

1. Open `CraneLink_PhoneBench.ino` from this folder in Arduino IDE. Select **Tools → Board → esp32 → ESP32 Dev Module** with the Espressif Arduino 3.x core. Set `CRANELINK_SENDER` to `0` in **BuildRole.h**, then upload to the receiver.
2. Open Serial Monitor at **115200 baud**. If you missed the startup output, press the board's **EN** or **RESET** button. The output shows the receiver’s Wi-Fi name and password. The password is kept across reboots.
3. Join that Wi-Fi on your phone. If prompted, choose to stay connected without internet. Type **http://192.168.4.1/** into the browser address bar.
4. Tap **Reset Emergency Stop** after receiver startup. Then tap **Web Run** and hold one direction. Its assigned relay and R8 should energize. Both should release when you lift your finger. **Emergency Stop** turns all eight outputs off and latches stop. **Reset Emergency Stop** clears the latch. Tap Web Run again before starting another jog.

Use the relay module's specified external supply. For a module with non-isolated logic inputs, connect the module's input ground to ESP32 GND and use inputs compatible with 3.3 V logic. Do not feed 5 V into the ESP32 GPIOs. The sketch defaults to active-low inputs, as in the original code. For active-high inputs, set `relayActiveLow` to `false` in `RelayController.h` before flashing.

| Relay input | ESP32 GPIO | Function |
| --- | ---: | --- |
| IN1 | 26 | Up |
| IN2 | 21 | Down |
| IN3 | 33 | Left |
| IN4 | 32 | Right |
| IN5 | 27 | Front |
| IN6 | 14 | Back |
| IN7 | 13 | Power On/Off latch |
| IN8 | 4 | Enable during a held directional jog |

If you wired an earlier revision, turn off the ESP32 and relay module before moving the IN2 signal wire from GPIO25 to GPIO21.

## Power and settings

- Power defaults to **60 minutes of inactivity**. Accepted directional jogs and Power holds count as activity. An open phone page and idle ESP-NOW packets do not count. The phone shows hold progress and the idle countdown.
- Open **Settings and sender pairing** to set a Power timeout from **1 to 720 minutes** and assign **Up, Down, Left, Right, Front, Back, and Power On/Off** to R1–R7. Use each relay once. R8 is reserved. Turn Web Run and Power off and release the sender before saving. Settings survive a reboot.
- **All directions together** is the default for the disconnected relay bench. You can hold multiple phone buttons with multiple fingers or hold all six physical direction switches. **NO/NC interlock** means one direction per axis in software. Up/Down, Left/Right and Front/Back are separate pairs. If one direction in a pair is already held, it takes priority. If both arrive together the pair stays off. Release both before reversing after a conflict. This setting does not change electrical trigger polarity or the physical NO and NC contacts.
- To move Up from R1 to R3, select **R3** beside **Up**. The page swaps the previous R3 assignment to R1 automatically. Click **Save relay settings**, then test Up. Relay assignments and sender pairing are saved separately.
- Only one source can jog at a time. A phone joined to the receiver’s Wi-Fi pauses the handheld. Both sources can stop the relays.

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
   | Power On/Off | 13 |
   | Emergency Stop | 14 |

2. In the sender's Serial Monitor, choose **115200 baud** and **New Line** or **Both NL & CR**. Send `info` to display the sender's station MAC address.
3. On the phone connected to the receiver's Wi-Fi, open **Settings and sender pairing** and choose an empty **Sender slot**. Enter that sender's station MAC, tap **Generate pairing key**, and copy the 32-character key privately. Click **Save sender slot**. Also copy the **receiver station MAC** shown on that page.
4. In the sender's Serial Monitor, enter `pair RECEIVER_MAC 32_HEX_KEY`, replacing both placeholders with the values from the page. Keep the spaces between the command, MAC, and key. The sender saves the pairing and prints **Receiver link found** when it hears the receiver. Both use Wi-Fi channel 6. Close and reopen Settings to refresh the link status.
5. Disconnect the phone from receiver Wi-Fi and release all sender buttons before testing. Hold Up and check its assigned relay plus R8, then release. Hold Power for three seconds, release, and repeat to toggle it off.
6. Repeat for additional senders, with a separate slot and key for each. The phone or one sender owns the jog at a time. A second sender whose first press is blocked must release and press again to take over.

## Replace a board

| Replacement | What to do |
| --- | --- |
| Sender | Flash the sender role. Get its new station MAC with `info`. On the receiver page, choose the old sender's slot, replace its MAC and generate a new key. Save the slot, then enter `pair RECEIVER_MAC 32_HEX_KEY` on the new sender. |
| Receiver | Flash the receiver role and join its Wi-Fi using Serial Monitor. Restore the relay settings. For each sender, get its station MAC with `info`, save its slot with a new key, and run `pair RECEIVER_MAC 32_HEX_KEY` on that sender. You can also restart the USB wizard with all boards. |

Record the relay assignments and Power timeout if you want to reproduce them on a replacement receiver.

| Bench check | Expected result |
| --- | --- |
| Hold a direction, then lift or slide the finger off it | The selected relay and R8 release. |
| Hold all six directions in default mode | All six direction relays and R8 energize. Releasing one drops its relay. |
| Enable NO/NC interlock. Hold Up, then touch Down | Up remains active. Lift both before starting a new hold for Down. Right and Back can run at the same time as Up. |
| Switch apps or turn off phone Wi-Fi during a jog | All relays release. Reopen or reconnect, then explicitly enable Web Run. |
| Open the page on a second tab or device | The second tab reports that Web Run is in use. |
| Press Emergency Stop during a jog, then reset using Power + Emergency Stop | All relays release. Reset leaves them off. |
| Set the Power timeout to one minute, then hold Power for three seconds and release | Power stays on without R8. It turns off after one idle minute. |
| Power off the sender while holding a direction or after latching Power | All outputs turn off. When it reconnects, release the controls before issuing a new command. |
| Join the receiver’s Wi-Fi during a handheld jog, then disconnect | The jog stops. After disconnecting, a fresh handheld press can jog again. |

## Findings in the supplied sketch

- The supplied receiver reopened pairing after a silent sender, which blocked phone jogs. The new phone access point stays available even if the handheld is absent or ESP-NOW fails to initialize.
- Presses, releases, and heartbeats can overlap. A delayed press can arrive after a release. Web Run off also leaves the old command mask intact, and the status poll can enable Web Run in a second browser. The phone bench uses one session per browser, numbered requests, and receiver-acknowledged state.
- Configuration reuses the same field for the current PIN and the replacement PIN, and the UI accepts longer PINs than firmware can store. Some status fields are also inserted as HTML. The phone bench has a generated Wi-Fi password and displays status as text.
- The original relay test path could hold a coil on independently of jog handling. The current relay driver uses the same timeout and stop behavior for every command.
- The original ESP-NOW callbacks modified shared state and saved settings directly in the Wi-Fi task. The current callback queues received frames. The receiver uses the configured sender MAC and key.
