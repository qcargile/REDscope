import fs from "fs";
import { fnv1a64 } from "./fnv1a64.mjs";

const AUDIO_EXT = new Set([".wem", ".opuspak", ".opusinfo", ".bnk", ".bk2"]);

function parseArgs(argv) {
    const a = { input: null, output: null, mode: "no-audio" };
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === "--input") a.input = argv[++i];
        else if (argv[i] === "--output") a.output = argv[++i];
        else if (argv[i] === "--mode") a.mode = argv[++i];
    }
    return a;
}

function keepPath(path, mode) {
    if (mode === "all") return true;
    const dot = path.lastIndexOf(".");
    const ext = dot >= 0 ? path.slice(dot).toLowerCase() : "";
    if (mode === "no-audio") return !AUDIO_EXT.has(ext);
    if (mode === "streaming") {
        return [".mesh", ".ent", ".app", ".scene", ".streamingsector", ".streamingsector_inplace",
                ".xbm", ".mi", ".mt", ".morphtarget", ".physicsdata", ".mlsetup", ".mlmask",
                ".envprobe", ".gidata", ".anims", ".cookedanims", ".rig", ".w2mesh", ".sectorcustomdata",
                ".inkatlas", ".inkwidget", ".effect", ".particle"].includes(ext);
    }
    return true;
}

const args = parseArgs(process.argv.slice(2));
if (!args.input || !args.output) {
    console.error("usage: node build-dictionary.mjs --input <paths.txt> --output <dict.rsdb> [--mode all|no-audio|streaming]");
    process.exit(2);
}

const t0 = Date.now();
const data = fs.readFileSync(args.input, "utf8");
const lines = data.split(/\r?\n/);

const byHash = new Map();
let scanned = 0, kept = 0, collisions = 0;
for (const raw of lines) {
    const path = raw.trim();
    if (!path) continue;
    scanned++;
    if (!keepPath(path, args.mode)) continue;
    const h = fnv1a64(path).toString();
    if (byHash.has(h)) { collisions++; continue; }
    byHash.set(h, path);
    kept++;
}

const entries = [...byHash.entries()].map(([h, path]) => ({ hash: BigInt(h), path }));
entries.sort((a, b) => (a.hash < b.hash ? -1 : a.hash > b.hash ? 1 : 0));

const count = entries.length;
const indexOffset = 16;
const pathsOffset = indexOffset + count * 16;

const pathBuffers = [];
let blobLen = 0;
const indexBuf = Buffer.alloc(count * 16);
for (let i = 0; i < count; i++) {
    const e = entries[i];
    const pb = Buffer.from(e.path, "utf8");
    const o = i * 16;
    indexBuf.writeBigUInt64BE(e.hash, o);
    indexBuf.writeUInt32LE(blobLen, o + 8);
    indexBuf.writeUInt32LE(pb.length, o + 12);
    pathBuffers.push(pb);
    blobLen += pb.length;
}

const header = Buffer.alloc(16);
header.write("RSD1", 0, "ascii");
header.writeUInt32LE(count, 4);
header.writeUInt32LE(indexOffset, 8);
header.writeUInt32LE(pathsOffset, 12);

const out = fs.openSync(args.output, "w");
fs.writeSync(out, header);
fs.writeSync(out, indexBuf);
for (const pb of pathBuffers) fs.writeSync(out, pb);
fs.closeSync(out);

const totalBytes = pathsOffset + blobLen;
console.log(`mode=${args.mode}  scanned=${scanned}  kept=${kept}  collisions=${collisions}`);
console.log(`entries=${count}  index=${(count * 16 / 1048576).toFixed(1)}MB  blob=${(blobLen / 1048576).toFixed(1)}MB  total=${(totalBytes / 1048576).toFixed(1)}MB`);
console.log(`wrote ${args.output} in ${((Date.now() - t0) / 1000).toFixed(1)}s`);
