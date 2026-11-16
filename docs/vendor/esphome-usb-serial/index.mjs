// ESPHome ESP Web Tools, commit 460229f. Apache-2.0. Transpiled from TypeScript.
import { cdcAcmDriver } from "./cdc-acm.mjs";
import { ch34xDriver } from "./ch34x.mjs";
import { cp210xDriver } from "./cp210x.mjs";
import { matchesDefinition } from "./driver.mjs";
import { ftdiDriver } from "./ftdi.mjs";
export const usbSerialDrivers = [
    cp210xDriver,
    ch34xDriver,
    ftdiDriver,
    cdcAcmDriver
];
export const usbSerialFilters = ()=>usbSerialDrivers.flatMap((driver)=>driver.filters);
export const findUsbSerialDriver = (device)=>usbSerialDrivers.find((driver)=>matchesDefinition(device, driver));
