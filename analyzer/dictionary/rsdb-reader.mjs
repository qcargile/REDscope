import fs from "fs";

export function openDict(path) {
    const fd = fs.openSync(path, "r");
    const header = Buffer.alloc(16);
    fs.readSync(fd, header, 0, 16, 0);
    if (header.toString("ascii", 0, 4) !== "RSD1") { fs.closeSync(fd); throw new Error("bad magic (not an RSD1 dictionary)"); }
    return {
        fd,
        count: header.readUInt32LE(4),
        indexOffset: header.readUInt32LE(8),
        pathsOffset: header.readUInt32LE(12),
    };
}

export function lookup(dict, hash) {
    const target = Buffer.alloc(8);
    target.writeBigUInt64BE(typeof hash === "bigint" ? hash : BigInt(hash));
    const entry = Buffer.alloc(16);
    let lo = 0, hi = dict.count - 1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        fs.readSync(dict.fd, entry, 0, 16, dict.indexOffset + mid * 16);
        const cmp = entry.compare(target, 0, 8, 0, 8);
        if (cmp === 0) {
            const pathOff = entry.readUInt32LE(8);
            const pathLen = entry.readUInt32LE(12);
            const pb = Buffer.alloc(pathLen);
            fs.readSync(dict.fd, pb, 0, pathLen, dict.pathsOffset + pathOff);
            return pb.toString("utf8");
        }
        if (cmp < 0) lo = mid + 1; else hi = mid - 1;
    }
    return null;
}

export function closeDict(dict) { fs.closeSync(dict.fd); }
