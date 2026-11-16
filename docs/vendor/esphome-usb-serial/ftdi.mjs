// ESPHome ESP Web Tools, commit 460229f. Apache-2.0. Transpiled from TypeScript.
import { controlOut, findBulkEndpoints, findInterface } from "./driver.mjs";
const VENDOR_CLASS = 0xff;
const REQ_SET_MODEM_CTRL = 0x01;
const REQ_SET_FLOW_CTRL = 0x02;
const REQ_SET_BAUDRATE = 0x03;
const REQ_SET_DATA = 0x04;
const MODEM_DTR_HIGH = 0x0001;
const MODEM_RTS_HIGH = 0x0002;
const MODEM_DTR_MASK = 0x0100;
const MODEM_RTS_MASK = 0x0200;
const DATA_BREAK = 1 << 14;
const STATUS_BYTES = 2;
export const ftdiChipFromVersion = (bcdDevice, multiInterface)=>{
    if (bcdDevice < 0x400) {
        return undefined;
    }
    if (multiInterface) {
        const highSpeed = bcdDevice === 0x700 || bcdDevice === 0x800;
        return {
            highSpeed,
            shiftBaudIndex: true,
            maxBaud: highSpeed ? 12e6 : 3e6
        };
    }
    switch(bcdDevice){
        case 0x400:
        case 0x600:
            return {
                highSpeed: false,
                shiftBaudIndex: false,
                maxBaud: 3e6
            };
        case 0x500:
        case 0x1000:
            return {
                highSpeed: false,
                shiftBaudIndex: true,
                maxBaud: 3e6
            };
        default:
            return {
                highSpeed: true,
                shiftBaudIndex: true,
                maxBaud: 12e6
            };
    }
};
const DIV_FRAC = [
    0,
    3,
    2,
    4,
    1,
    5,
    6,
    7
];
const divRoundClosest = (n, d)=>Math.floor((n + Math.floor(d / 2)) / d);
export const ftdiBaudDivisor = (baudRate, chip)=>{
    const baud = Math.min(Math.max(baudRate, 1), chip.maxBaud);
    const divisor3 = chip.highSpeed ? divRoundClosest(8 * 120_000_000, 10 * baud) : divRoundClosest(48_000_000, 2 * baud);
    let divisor = divisor3 >> 3 | DIV_FRAC[divisor3 & 7] << 14;
    if (divisor === 1) {
        divisor = 0;
    } else if (divisor === 0x4001) {
        divisor = 1;
    }
    if (chip.highSpeed) {
        divisor |= 0x20000;
    }
    return divisor;
};
export const ftdiDataBits = (config, brk = false)=>{
    const parity = {
        none: 0,
        odd: 1,
        even: 2
    }[config.parity];
    const stop = config.stopBits === 2 ? 2 : 0;
    return config.dataBits | parity << 8 | stop << 11 | (brk ? DATA_BREAK : 0);
};
export const ftdiModemControl = (dtr, rts)=>MODEM_DTR_MASK | MODEM_RTS_MASK | (dtr ? MODEM_DTR_HIGH : 0) | (rts ? MODEM_RTS_HIGH : 0);
class FtdiDriver {
    _device;
    _chip;
    _index = 0;
    _packetSize = 64;
    _config;
    constructor(_device){
        this._device = _device;
    }
    _out(request, value, index) {
        return controlOut(this._device, {
            requestType: "vendor",
            recipient: "device",
            request,
            value,
            index
        });
    }
    async open() {
        const iface = findInterface(this._device, VENDOR_CLASS);
        if (!iface) {
            throw new Error("No vendor specific interface found");
        }
        const multiInterface = this._device.configuration.interfaces.length > 1;
        const bcdDevice = this._device.deviceVersionMajor << 8 | this._device.deviceVersionMinor << 4 | this._device.deviceVersionSubminor;
        const chip = ftdiChipFromVersion(bcdDevice, multiInterface);
        if (!chip) {
            throw new Error("Unsupported FTDI chip");
        }
        this._chip = chip;
        this._index = multiInterface ? iface.interfaceNumber + 1 : 0;
        await this._device.claimInterface(iface.interfaceNumber);
        const endpoints = findBulkEndpoints(iface);
        this._packetSize = endpoints.in.packetSize;
        await this._out(REQ_SET_FLOW_CTRL, 0, this._index);
        return endpoints;
    }
    async setLineConfig(config) {
        this._config = config;
        const divisor = ftdiBaudDivisor(config.baudRate, this._chip);
        const value = divisor & 0xffff;
        let index = divisor >>> 16 & 0xffff;
        if (this._chip.shiftBaudIndex) {
            index = index << 8 | this._index;
        }
        await this._out(REQ_SET_BAUDRATE, value, index);
        await this._out(REQ_SET_DATA, ftdiDataBits(config), this._index);
    }
    setSignals(dataTerminalReady, requestToSend) {
        return this._out(REQ_SET_MODEM_CTRL, ftdiModemControl(dataTerminalReady, requestToSend), this._index);
    }
    setBreak(enabled) {
        return this._out(REQ_SET_DATA, ftdiDataBits(this._config, enabled), this._index);
    }
    processRead(data) {
        return ftdiStripStatus(data, this._packetSize);
    }
}
export const ftdiStripStatus = (data, packetSize)=>{
    const out = new Uint8Array(data.byteLength);
    let length = 0;
    for(let start = 0; start < data.byteLength; start += packetSize){
        const payload = data.subarray(start + STATUS_BYTES, Math.min(start + packetSize, data.byteLength));
        out.set(payload, length);
        length += payload.byteLength;
    }
    return out.subarray(0, length);
};
export const ftdiDriver = {
    name: "FTDI",
    filters: [
        0x6001,
        0x6010,
        0x6011,
        0x6014,
        0x6015
    ].map((productId)=>({
            vendorId: 0x0403,
            productId
        })),
    create: (device)=>new FtdiDriver(device)
};
