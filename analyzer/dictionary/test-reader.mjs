import { openDict, lookup, closeDict } from "./rsdb-reader.mjs";

const SAMPLES = [
    ["17138494147559456028", "base\\anim_cooked.cookedanims", true],
    ["16239627860022410734", "(.wem johnny vo)", false],
    ["7867789368763836221", "(.wem merc vo)", false],
    ["1353663417162055546", "loc_q307_fia_hos_d511725c.envprobe", true],
    ["2429113337638151124", "hh_080_wa__buns_01.ent", true],
    ["988468502484951255", "decoset_shop_machine_b_v2_mproxy.mesh", true],
    ["10290667150920748779", "anton_kolev.app", true],
    ["11135250789206787704", "always_loaded_0.streamingsector", true],
    ["11296439083125339284", "anton_kolev_anton_kolev_default_d.xbm", true],
];

let fail = 0;

function run(dictPath, expectInStreaming) {
    console.log(`\n=== ${dictPath} ===`);
    const d = openDict(dictPath);
    console.log(`header ok: count=${d.count} pathsOffset=${d.pathsOffset}`);
    for (const [h, label, isStreamingType] of SAMPLES) {
        const got = lookup(d, h);
        const shouldFind = expectInStreaming ? isStreamingType : true;
        const ok = shouldFind ? (got !== null && got.includes(label.split("\\").pop().replace(/\(|\)/g, "").split(" ").pop()) || got !== null) : (got === null);
        // simpler verdict: shouldFind => non-null; else => null
        const verdict = shouldFind ? (got !== null) : (got === null);
        if (!verdict) fail++;
        console.log(`  ${verdict ? "OK " : "FAIL"}  ${h}  expect=${shouldFind ? "hit" : "miss"}  got=${got ? got : "(null)"}`);
    }
    closeDict(d);
}

run("D:/REDscope-dict-build/redscope-paths-all.rsdb", false);
run("D:/REDscope-dict-build/redscope-paths-streaming.rsdb", true);

console.log(fail === 0 ? "\nALL READER CHECKS PASSED" : `\n${fail} READER CHECKS FAILED`);
process.exit(fail === 0 ? 0 : 1);
