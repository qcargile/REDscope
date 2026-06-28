import fs from "fs";

// Mirror of rsdbReader.lua's exact approach: hex "0x..%016X" -> 8 BE bytes,
// byte-string compare, manual little-endian u32. Validates the Lua logic + the
// real .crash.json hash input format before in-game.
function u32le(buf, i) { return buf[i] + buf[i + 1] * 256 + buf[i + 2] * 65536 + buf[i + 3] * 16777216; }
function hexToBE(hex) {
    let h = hex.replace(/^0[xX]/, "");
    h = h.padStart(16, "0");
    const b = Buffer.alloc(8);
    for (let i = 0; i < 8; i++) b[i] = parseInt(h.substr(i * 2, 2), 16);
    return b;
}

const file = "D:/REDscope-dict-build/redscope-paths-all.rsdb";
const fd = fs.openSync(file, "r");
const header = Buffer.alloc(16); fs.readSync(fd, header, 0, 16, 0);
const count = u32le(header, 4), indexOffset = u32le(header, 8), pathsOffset = u32le(header, 12);

function lookup(hexHash) {
    const target = hexToBE(hexHash);
    const entry = Buffer.alloc(16);
    let lo = 0, hi = count - 1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        fs.readSync(fd, entry, 0, 16, indexOffset + mid * 16);
        const cmp = Buffer.compare(entry.subarray(0, 8), target);
        if (cmp === 0) {
            const pathOff = u32le(entry, 8), pathLen = u32le(entry, 12);
            const pb = Buffer.alloc(pathLen); fs.readSync(fd, pb, 0, pathLen, pathsOffset + pathOff);
            return pb.toString("utf8");
        }
        if (cmp < 0) lo = mid + 1; else hi = mid - 1;
    }
    return null;
}

const expect = {
    "17138494147559456028": "base\\anim_cooked.cookedanims",
    "2429113337638151124": "hh_080_wa__buns_01.ent",
    "988468502484951255": "decoset_shop_machine_b_v2_mproxy.mesh",
    "10290667150920748779": "anton_kolev.app",
    "11135250789206787704": "always_loaded_0.streamingsector",
};
let fail = 0;
for (const [dec, frag] of Object.entries(expect)) {
    const hex = "0x" + BigInt(dec).toString(16).toUpperCase().padStart(16, "0");
    const got = lookup(hex);
    const ok = got && got.includes(frag);
    if (!ok) fail++;
    console.log(`${ok ? "OK " : "FAIL"}  ${hex}  ->  ${got || "(null)"}`);
}
fs.closeSync(fd);
console.log(fail === 0 ? "\nLUA ALGORITHM (hex->path via sidecar format) VALIDATED" : `\n${fail} FAILED`);
process.exit(fail === 0 ? 0 : 1);
