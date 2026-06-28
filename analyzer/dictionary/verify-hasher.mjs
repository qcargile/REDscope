import { fnv1a64, sanitize } from "./fnv1a64.mjs";

const cases = [
    "test",
    "base\\characters\\base_entities\\player_base\\player_base.ent",
    "Base/Characters/Base_Entities/Player_Base/player_base.ent",
    "base\\\\characters//base_entities\\\\player_base\\player_base.ent",
    "\\\\base\\test.mesh",
    "base\\fx\\path\\to\\FILE.effect"
];

for (const c of cases) {
    console.log(`${fnv1a64(c).toString()}\t[${sanitize(c)}]\t<- ${JSON.stringify(c)}`);
}
