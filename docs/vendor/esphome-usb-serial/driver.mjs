// ESPHome ESP Web Tools, commit 460229f. Apache-2.0. Transpiled from TypeScript.
const matchesFilter = (device, filter)=>{
    if (filter.vendorId !== undefined && filter.vendorId !== device.vendorId) {
        return false;
    }
    if (filter.productId !== undefined && filter.productId !== device.productId) {
        return false;
    }
    if (filter.classCode === undefined) {
        return true;
    }
    if (device.deviceClass === filter.classCode) {
        return true;
    }
    return device.configurations.some((config)=>config.interfaces.some((iface)=>iface.alternates.some((alt)=>alt.interfaceClass === filter.classCode)));
};
export const matchesDefinition = (device, definition)=>definition.filters.some((f)=>matchesFilter(device, f));
export const findInterface = (device, classCode)=>device.configuration?.interfaces.find((iface)=>iface.alternates[0]?.interfaceClass === classCode);
export const findBulkEndpoints = (iface)=>{
    const endpoints = iface.alternates[0].endpoints.filter((ep)=>ep.type === "bulk");
    const inEp = endpoints.find((ep)=>ep.direction === "in");
    const outEp = endpoints.find((ep)=>ep.direction === "out");
    if (!inEp || !outEp) {
        throw new Error(`Interface ${iface.interfaceNumber} has no bulk IN and OUT endpoints`);
    }
    return {
        in: inEp,
        out: outEp
    };
};
export const controlOut = async (device, setup, data)=>{
    const result = await device.controlTransferOut(setup, data);
    if (result.status !== "ok") {
        throw new Error(`USB control request 0x${setup.request.toString(16)} failed: ${result.status}`);
    }
};
export const controlIn = async (device, setup, length)=>{
    const result = await device.controlTransferIn(setup, length);
    if (result.status !== "ok" || !result.data) {
        throw new Error(`USB control request 0x${setup.request.toString(16)} failed: ${result.status}`);
    }
    return result.data;
};
