// ESPHome ESP Web Tools, commit 460229f. Apache-2.0. Transpiled from TypeScript.
import { controlOut, findBulkEndpoints, findInterface } from "./driver.mjs";
const VENDOR_ID = 0x10c4;
const VENDOR_CLASS = 0xff;
const IFC_ENABLE = 0x00;
const SET_LINE_CTL = 0x03;
const SET_BREAK = 0x05;
const SET_MHS = 0x07;
const SET_BAUDRATE = 0x1e;
const UART_ENABLE = 0x0001;
const UART_DISABLE = 0x0000;
const MHS_DTR = 0x0001;
const MHS_RTS = 0x0002;
const MHS_WRITE_DTR = 0x0100;
const MHS_WRITE_RTS = 0x0200;
export const cp210xLineControl = (config)=>{
    const parity = {
        none: 0x00,
        odd: 0x10,
        even: 0x20
    }[config.parity];
    const stop = config.stopBits === 2 ? 0x02 : 0x00;
    return config.dataBits << 8 | parity | stop;
};
export const cp210xModemControl = (dtr, rts)=>MHS_WRITE_DTR | MHS_WRITE_RTS | (dtr ? MHS_DTR : 0) | (rts ? MHS_RTS : 0);
class Cp210xDriver {
    _device;
    _interfaceNumber = 0;
    constructor(_device){
        this._device = _device;
    }
    _out(request, value, data) {
        return controlOut(this._device, {
            requestType: "vendor",
            recipient: "interface",
            request,
            value,
            index: this._interfaceNumber
        }, data);
    }
    async open() {
        const iface = findInterface(this._device, VENDOR_CLASS);
        if (!iface) {
            throw new Error("No vendor specific interface found");
        }
        this._interfaceNumber = iface.interfaceNumber;
        await this._device.claimInterface(iface.interfaceNumber);
        await this._out(IFC_ENABLE, UART_ENABLE);
        return findBulkEndpoints(iface);
    }
    async setLineConfig(config) {
        const baud = new Uint8Array(4);
        new DataView(baud.buffer).setUint32(0, config.baudRate, true);
        await this._out(SET_BAUDRATE, 0, baud);
        await this._out(SET_LINE_CTL, cp210xLineControl(config));
    }
    setSignals(dataTerminalReady, requestToSend) {
        return this._out(SET_MHS, cp210xModemControl(dataTerminalReady, requestToSend));
    }
    setBreak(enabled) {
        return this._out(SET_BREAK, enabled ? 1 : 0);
    }
    async close() {
        await this._out(IFC_ENABLE, UART_DISABLE);
    }
}
export const cp210xDriver = {
    name: "CP210x",
    filters: [
        0xea60,
        0xea61,
        0xea63,
        0xea70,
        0xea71
    ].map((productId)=>({
            vendorId: VENDOR_ID,
            productId
        })),
    create: (device)=>new Cp210xDriver(device)
};
