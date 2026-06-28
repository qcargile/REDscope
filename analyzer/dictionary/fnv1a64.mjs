const OFFSET = 0xcbf29ce484222325n;
const PRIME = 0x100000001b3n;
const MASK = 0xffffffffffffffffn;
const MAX_PATH_LENGTH = 216;

export function sanitize(path) {
    const s = path;
    let i = 0;
    if (s[i] === '"' || s[i] === "'") i++;
    while (s[i] === '/' || s[i] === '\\') i++;
    let out = '';
    while (i < s.length && s[i] !== '"' && s[i] !== "'") {
        const c = s[i];
        if (c === '/' || c === '\\') {
            out += '\\';
            i++;
            while (i < s.length && (s[i] === '/' || s[i] === '\\')) i++;
        } else {
            out += (c >= 'A' && c <= 'Z') ? c.toLowerCase() : c;
            i++;
        }
        if (out.length === MAX_PATH_LENGTH) break;
    }
    return out;
}

export function fnv1a64(path) {
    const s = sanitize(path);
    if (s.length === 0) return 0n;
    let h = OFFSET;
    for (let k = 0; k < s.length; k++) {
        h ^= BigInt(s.charCodeAt(k) & 0xff);
        h = (h * PRIME) & MASK;
    }
    return h;
}
