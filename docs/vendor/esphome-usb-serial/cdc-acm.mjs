// ESPHome ESP Web Tools, commit 460229f. Apache-2.0. Transpiled from TypeScript.
import { controlOut, findBulkEndpoints, findInterface } from "./driver.mjs";
const CDC_CLASS = 0x02;
const CDC_DATA_CLASS = 0x0a;
const SET_LINE_CODING = 0x20;
const SET_CONTROL_LINE_STATE = 0x22;
const SEND_BREAK = 0x23;
const PARITY_INDEX = {
    none: 0,
    odd: 1,
    even: 2
};
export const cdcLineCoding = (config)=>{
    const buffer = new Uint8Array(7);
    new DataView(buffer.buffer).setUint32(0, config.baudRate, true);
    buffer[4] = config.stopBits === 2 ? 2 : 0;
    buffer[5] = PARITY_INDEX[config.parity];
    buffer[6] = config.dataBits;
    return buffer;
};
class CdcAcmDriver {
    _device;
    _controlNumber = 0;
    constructor(_device){
        this._device = _device;
    }
    async open() {
        const control = findInterface(this._device, CDC_CLASS);
        if (!control) {
            throw new Error("No CDC control interface found");
        }
        const data = findInterface(this._device, CDC_DATA_CLASS) ?? control;
        this._controlNumber = control.interfaceNumber;
        await this._device.claimInterface(control.interfaceNumber);
        if (data !== control) {
            await this._device.claimInterface(data.interfaceNumber);
        }
        return findBulkEndpoints(data);
    }
    setLineConfig(config) {
        return controlOut(this._device, {
            requestType: "class",
            recipient: "interface",
            request: SET_LINE_CODING,
            value: 0,
            index: this._controlNumber
        }, cdcLineCoding(config));
    }
    setSignals(dataTerminalReady, requestToSend) {
        return controlOut(this._device, {
            requestType: "class",
            recipient: "interface",
            request: SET_CONTROL_LINE_STATE,
            value: (dataTerminalReady ? 1 : 0) | (requestToSend ? 2 : 0),
            index: this._controlNumber
        });
    }
    setBreak(enabled) {
        return controlOut(this._device, {
            requestType: "class",
            recipient: "interface",
            request: SEND_BREAK,
            value: enabled ? 0xffff : 0,
            index: this._controlNumber
        });
    }
}
export const cdcAcmDriver = {
    name: "CDC-ACM",
    filters: [
        {
            classCode: CDC_CLASS
        }
    ],
    create: (device)=>new CdcAcmDriver(device)
};
