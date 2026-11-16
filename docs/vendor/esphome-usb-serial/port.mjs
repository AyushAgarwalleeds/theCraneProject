// ESPHome ESP Web Tools, commit 460229f. Apache-2.0. Transpiled from TypeScript.
const DEFAULT_BUFFER_SIZE = 255;
export const toLineConfig = (options)=>{
    const { baudRate, dataBits = 8, stopBits = 1, parity = "none" } = options;
    if (!Number.isInteger(baudRate) || baudRate <= 0) {
        throw new RangeError(`invalid Baud Rate ${baudRate}`);
    }
    if (![
        5,
        6,
        7,
        8
    ].includes(dataBits)) {
        throw new RangeError(`invalid dataBits ${dataBits}`);
    }
    if (stopBits !== 1 && stopBits !== 2) {
        throw new RangeError(`invalid stopBits ${stopBits}`);
    }
    if (![
        "none",
        "even",
        "odd"
    ].includes(parity)) {
        throw new RangeError(`invalid parity ${parity}`);
    }
    return {
        baudRate,
        dataBits,
        stopBits,
        parity
    };
};
export class UsbSerialPort {
    _device;
    _definition;
    _driver;
    _endpoints;
    _options;
    _readable = null;
    _writable = null;
    _pendingRead;
    _signals = {
        dataTerminalReady: false,
        requestToSend: false
    };
    _break = false;
    constructor(_device, _definition){
        this._device = _device;
        this._definition = _definition;
    }
    get _bufferSize() {
        return this._options?.bufferSize ?? DEFAULT_BUFFER_SIZE;
    }
    get readable() {
        if (this._readable || !this._device.opened || !this._endpoints) {
            return this._readable;
        }
        const stream = new ReadableStream({
            pull: async (controller)=>{
                this._pendingRead ??= this._read(controller.desiredSize);
                this._pendingRead.catch(()=>undefined);
                let chunk;
                try {
                    chunk = await this._pendingRead;
                } catch (err) {
                    this._pendingRead = undefined;
                    if (this._readable === stream) {
                        this._readable = null;
                    }
                    controller.error(err);
                    return;
                }
                if (this._readable !== stream) {
                    return;
                }
                this._pendingRead = undefined;
                controller.enqueue(chunk);
            },
            cancel: ()=>{
                if (this._readable === stream) {
                    this._readable = null;
                }
            }
        }, new ByteLengthQueuingStrategy({
            highWaterMark: this._bufferSize
        }));
        this._readable = stream;
        return stream;
    }
    get writable() {
        if (this._writable || !this._device.opened || !this._endpoints) {
            return this._writable;
        }
        const stream = new WritableStream({
            write: async (chunk)=>{
                if (!chunk.byteLength) {
                    return;
                }
                try {
                    const result = await this._device.transferOut(this._endpoints.out.endpointNumber, chunk);
                    if (result.status !== "ok") {
                        throw new Error(`USB error: ${result.status}`);
                    }
                } catch (err) {
                    if (this._writable === stream) {
                        this._writable = null;
                    }
                    throw err;
                }
            }
        }, new ByteLengthQueuingStrategy({
            highWaterMark: this._bufferSize
        }));
        this._writable = stream;
        return stream;
    }
    async _read(desiredSize) {
        const endpoint = this._endpoints.in;
        const driver = this._driver;
        const packetSize = endpoint.packetSize;
        const length = Math.max(1, Math.ceil((desiredSize ?? packetSize) / packetSize)) * packetSize;
        for(;;){
            const result = await this._device.transferIn(endpoint.endpointNumber, length);
            if (result.status !== "ok") {
                throw new Error(`USB error: ${result.status}`);
            }
            if (!result.data?.byteLength) {
                continue;
            }
            let chunk = new Uint8Array(result.data.buffer, result.data.byteOffset, result.data.byteLength);
            if (driver.processRead) {
                chunk = driver.processRead(chunk);
            }
            if (chunk.byteLength) {
                return chunk;
            }
        }
    }
    async open(options) {
        const config = toLineConfig(options);
        this._options = options;
        try {
            await this._device.open();
            if (this._device.configuration === null) {
                await this._device.selectConfiguration(1);
            }
            const driver = this._definition.create(this._device);
            this._driver = driver;
            this._endpoints = await driver.open();
            await driver.setLineConfig(config);
            this._signals = {
                dataTerminalReady: false,
                requestToSend: false
            };
            this._break = false;
            await driver.setSignals(false, false);
        } catch (err) {
            this._driver = undefined;
            this._endpoints = undefined;
            if (this._device.opened) {
                await this._device.close().catch(()=>undefined);
            }
            throw new Error(`Error setting up device: ${err}`);
        }
    }
    async close() {
        await Promise.all([
            this._readable?.cancel().catch(()=>undefined),
            this._writable?.abort().catch(()=>undefined)
        ]);
        this._readable = null;
        this._writable = null;
        this._pendingRead = undefined;
        if (!this._device.opened) {
            return;
        }
        try {
            await this._driver?.setSignals(false, false);
            await this._driver?.close?.();
        } catch  {}
        this._driver = undefined;
        this._endpoints = undefined;
        await this._device.close();
    }
    forget() {
        return this._device.forget();
    }
    getInfo() {
        return {
            usbVendorId: this._device.vendorId,
            usbProductId: this._device.productId
        };
    }
    async reconfigure(options) {
        const merged = {
            ...this._options,
            ...options
        };
        const config = toLineConfig(merged);
        this._options = merged;
        await this._requireDriver().setLineConfig(config);
    }
    async setSignals(signals) {
        const driver = this._requireDriver();
        if (signals.dataTerminalReady !== undefined || signals.requestToSend !== undefined) {
            this._signals = {
                dataTerminalReady: signals.dataTerminalReady ?? this._signals.dataTerminalReady,
                requestToSend: signals.requestToSend ?? this._signals.requestToSend
            };
            await driver.setSignals(this._signals.dataTerminalReady, this._signals.requestToSend);
        }
        if (signals.break !== undefined && driver.setBreak) {
            this._break = signals.break;
            await driver.setBreak(this._break);
        }
    }
    _requireDriver() {
        if (!this._driver) {
            throw new DOMException("The port is not open.", "InvalidStateError");
        }
        return this._driver;
    }
}
