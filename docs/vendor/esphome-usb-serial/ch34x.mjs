// ESPHome ESP Web Tools, commit 460229f. Apache-2.0. Transpiled from TypeScript.
import { controlIn, controlOut, findBulkEndpoints, findInterface } from "./driver.mjs";
const VENDOR_CLASS = 0xff;
const REQ_READ_VERSION = 0x5f;
const REQ_WRITE_REG = 0x9a;
const REQ_SERIAL_INIT = 0xa1;
const REQ_MODEM_CTRL = 0xa4;
const BIT_DTR = 1 << 5;
const BIT_RTS = 1 << 6;
const LCR_ENABLE_RX = 0x80;
const LCR_ENABLE_TX = 0x40;
const LCR_PAR_EVEN = 0x10;
const LCR_ENABLE_PAR = 0x08;
const LCR_STOP_BITS_2 = 0x04;
const CLOCK_RATE = 48_000_000;
const MIN_BPS = 46;
const MAX_BPS = 3_000_000;
const clockDivisor = (ps, fact)=>1 << 12 - 3 * ps - fact;
const MIN_RATES = [
    0,
    1,
    2,
    3
].map((ps)=>Math.floor(CLOCK_RATE / (clockDivisor(ps, 1) * 512)));
export const ch34xBaudDivisor = (baudRate)=>{
    const speed = Math.min(Math.max(baudRate, MIN_BPS), MAX_BPS);
    let fact = 1;
    let ps = 3;
    while(ps > 0 && speed <= MIN_RATES[ps]){
        ps--;
    }
    let clkDiv = clockDivisor(ps, fact);
    let div = Math.floor(CLOCK_RATE / (clkDiv * speed));
    if (div < 9 || div > 255) {
        div = Math.floor(div / 2);
        clkDiv *= 2;
        fact = 0;
    }
    if (16 * CLOCK_RATE / (clkDiv * div) - 16 * speed >= 16 * speed - 16 * CLOCK_RATE / (clkDiv * (div + 1))) {
        div++;
    }
    if (fact === 1 && div % 2 === 0) {
        div /= 2;
        fact = 0;
    }
    return 0x100 - div << 8 | fact << 2 | ps;
};
export const ch34xLineControl = (config)=>{
    let lcr = LCR_ENABLE_RX | LCR_ENABLE_TX | config.dataBits - 5;
    if (config.parity !== "none") {
        lcr |= LCR_ENABLE_PAR;
        if (config.parity === "even") {
            lcr |= LCR_PAR_EVEN;
        }
    }
    if (config.stopBits === 2) {
        lcr |= LCR_STOP_BITS_2;
    }
    return lcr;
};
export const ch34xModemControl = (dtr, rts)=>~((dtr ? BIT_DTR : 0) | (rts ? BIT_RTS : 0)) & 0xffff;
class Ch34xDriver {
    _device;
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
        await this._device.claimInterface(iface.interfaceNumber);
        await controlIn(this._device, {
            requestType: "vendor",
            recipient: "device",
            request: REQ_READ_VERSION,
            value: 0,
            index: 0
        }, 2);
        await this._out(REQ_SERIAL_INIT, 0, 0);
        return findBulkEndpoints(iface);
    }
    async setLineConfig(config) {
        await this._out(REQ_WRITE_REG, 0x1312, ch34xBaudDivisor(config.baudRate) | 0x80);
        await this._out(REQ_WRITE_REG, 0x2518, ch34xLineControl(config));
    }
    setSignals(dataTerminalReady, requestToSend) {
        return this._out(REQ_MODEM_CTRL, ch34xModemControl(dataTerminalReady, requestToSend), 0);
    }
}
export const ch34xDriver = {
    name: "CH340/CH341",
    filters: [
        {
            vendorId: 0x1a86,
            productId: 0x7523
        },
        {
            vendorId: 0x1a86,
            productId: 0x7522
        },
        {
            vendorId: 0x1a86,
            productId: 0x5523
        },
        {
            vendorId: 0x4348,
            productId: 0x5523
        }
    ],
    create: (device)=>new Ch34xDriver(device)
};
