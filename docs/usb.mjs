import {findUsbSerialDriver, usbSerialFilters} from './vendor/esphome-usb-serial/index.mjs';
import {UsbSerialPort} from './vendor/esphome-usb-serial/port.mjs';

export const useAndroidUsb = /Android/i.test(navigator.userAgent) && !!navigator.usb;
export const supported = window.isSecureContext && (useAndroidUsb || !!navigator.serial);

export async function requestPort() {
  if (!useAndroidUsb) return navigator.serial.requestPort();
  const device = await navigator.usb.requestDevice({filters:usbSerialFilters()});
  const driver = findUsbSerialDriver(device);
  if (!driver) throw Error('This Android USB adapter is unsupported. Try a different cable or desktop Chrome or Edge.');
  return new UsbSerialPort(device, driver);
}
