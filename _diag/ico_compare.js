// Compare two .ico files image by image.
//
// The two Apex icons happen to have the SAME byte length, which is what made this worth writing: a file
// size cannot tell "two different marks" from "one mark written twice". This parses the ICONDIR, extracts
// every embedded image and hashes it, so the answer is per-image and exact.
//
// Run: node _diag/ico_compare.js icon/apex-light.ico icon/apex-dark.ico
const fs = require("fs");
const crypto = require("crypto");

function parseIco(path) {
  const b = fs.readFileSync(path);
  const count = b.readUInt16LE(4);
  const images = [];
  for (let i = 0; i < count; i++) {
    const off = 6 + i * 16;
    const w = b[off] === 0 ? 256 : b[off];
    const h = b[off + 1] === 0 ? 256 : b[off + 1];
    const size = b.readUInt32LE(off + 8);
    const dataOff = b.readUInt32LE(off + 12);
    const data = b.subarray(dataOff, dataOff + size);
    const isPng =
      data.length > 8 && data[0] === 0x89 && data[1] === 0x50 && data[2] === 0x4e && data[3] === 0x47;
    images.push({ w, h, size, isPng, md5: crypto.createHash("md5").update(data).digest("hex") });
  }
  return { path, total: b.length, images };
}

const [a, b] = process.argv.slice(2);
const A = parseIco(a);
const B = parseIco(b);

console.log(`${A.path}  (${A.total} bytes, ${A.images.length} images)`);
for (const im of A.images) console.log(`   ${im.w}x${im.h}  ${im.isPng ? "PNG " : "BMP "} ${im.size} bytes  ${im.md5}`);
console.log(`${B.path}  (${B.total} bytes, ${B.images.length} images)`);
for (const im of B.images) console.log(`   ${im.w}x${im.h}  ${im.isPng ? "PNG " : "BMP "} ${im.size} bytes  ${im.md5}`);

console.log("\nper-size comparison:");
let identical = 0;
for (let i = 0; i < Math.min(A.images.length, B.images.length); i++) {
  const same = A.images[i].md5 === B.images[i].md5;
  if (same) identical++;
  console.log(`   ${A.images[i].w}x${A.images[i].h}: ${same ? "IDENTICAL" : "different"}`);
}
console.log(
  identical === A.images.length
    ? "\n=> THE TWO FILES ARE THE SAME ARTWORK (this alone explains 'the icon never changes')"
    : `\n=> ${A.images.length - identical} of ${A.images.length} sizes differ: two genuinely different marks`
);
