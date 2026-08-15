// Browser firmware flasher: esp-web-tools drives esptool over Web Serial.
// The manifests and binaries under flash/<model>/ are assembled by the Pages
// workflow (tools/package-web-flash.sh) from the same commit as this page.
import 'esp-web-tools';

const install = document.getElementById('install')!;
for (const radio of document.querySelectorAll<HTMLInputElement>('input[name="device"]')) {
  radio.addEventListener('change', () => {
    if (radio.checked) install.setAttribute('manifest', `flash/${radio.value}/manifest.json`);
  });
}
