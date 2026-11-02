# Potion patcher

This tool creates `ApexCombat - Potions.esp` for an explicit Skyrim load order. Drinking a converted potion grants Apex's configured stamina-cost discount instead of an instant stamina refill. Apex's plugin supplies the discount and its timer; the generated ESP supplies the potion records, tier keywords, and display effects.

Requires the .NET 8 SDK. Mutagen.Bethesda.Skyrim is pinned to 0.53.1.

```sh
dotnet run --project tools/PotionPatcher -- --self-test
dotnet run --project tools/PotionPatcher -- \
  --plugins-json loadorder.json \
  --data-dir /path/to/Data \
  --output "output/ApexCombat - Potions.esp"
```

The output directory must already exist. Add `--force` to replace an existing output. Generation writes and checks a temporary plugin before replacing the destination; it never edits source plugins. Activate the output after every input plugin that changes these potions. Regenerate it after changing those mods or their order. The tool does not install or activate anything.

`loadorder.json` is an array of physical paths, from first loaded to last loaded. Include the active masters and the winning physical copy of each active plugin. Relative paths resolve against the JSON file's directory. Do not include an earlier generated potion patch.

```json
[
  "Data/Skyrim.esm",
  "Data/Update.esm",
  "mods/PotionChanges/PotionChanges.esp"
]
```

The last override of each ingestible wins. A missing or later master, duplicate plugin name, existing tier-keyword collision, invalid input, or unreadable record stops generation. The JSON report gives the number of winning records, conversions, removed effects, skipped records, and conversions per tier.

## Conversion rules

Only these vanilla effects qualify, identified by their original FormKeys:

- `Skyrim.esm:03EB16` — AlchRestoreStamina.
- `Skyrim.esm:0FFA05` — AlchRestoreStaminaAll, used by the ultimate potion.

Deleted records, food, poison, potions without either effect, and potions whose largest qualifying magnitude is below 25 remain unchanged. Missing, negative, NaN, or infinite qualifying magnitudes also leave the whole potion unchanged. The largest qualifying magnitude selects the highest reached threshold: 25 / 50 / 75 / 100 / 150 / 9999, for Minor / Standard / Plentiful / Vigorous / Extreme / Ultimate.

For a converted potion, all instances of those two restoring effects are removed, including smaller instances on a mixed potion. One tier keyword and one receipt effect for the inventory description are added. Other effects, their conditions and links, existing keywords, models, names, and other record fields are copied from the winning override through Mutagen. The `NoAutoCalc` flag is set to keep the winning stored numeric value as the fixed price; that stored value may differ from its former automatically calculated price.

Receipt effects provide the inventory description. Apex removes their active instances after consumption and casts a separate self-targeted spell whose active effect supplies the discount and its remaining duration. Each new dose replaces the current discount and starts its full duration, including when the new potion is weaker. Discounts never stack.

Both effect types use the Script archetype with no attached scripts, actor value, or spell payload. Their magnitude records the discount percentage rather than modifying an actor value. The generated defaults are 10% / 20% / 30% / 50% / 75% for 60 seconds and 100% for 20 seconds. The description uses the game's `<mag>` and `<dur>` substitutions: "Reduces the stamina cost of attacks by <mag>% for <dur> seconds." Apex synchronizes the receipt and accepted-dose spell values with its loaded INI. Receipts stay visible for inventory descriptions; removal targets their active instances rather than their UI flags. Behavior still requires game testing with the target load order.

Local form IDs are stable: tier keywords `800–805`, receipt effects `806–80B`, accepted-dose buff effects `80C–811`, and their spells `812–817`, each ordered Minor through Ultimate. The spells cost no magicka and cannot be absorbed or reflected; they are cast internally rather than added to the player's spellbook.

## String resources

Supply the merged game Data directory containing the English resources for the listed plugins. Loose files in `Data/Strings` take precedence over matching files in BSAs in that directory. The tool stages only the required English files, normalizes their casing, and reads them without consulting the machine's installed game profile. If two archives contain different copies of the same requested file, supply the intended winning loose file in `Data/Strings`; the tool will not guess their priority. Missing strings fail generation. The output embeds English strings and does not require separate string files.

## Scope

Player-crafted or other dynamically created potions are outside this static patch. Untagged potions keep their original effects and do not grant an Apex discount. Restore effects implemented with other FormKeys, scripts, or custom systems also remain unchanged. There is no fallback that adds a discount on top of an unconverted refill.

The generated patch requires Apex Combat for its cost-discount behavior. Removing or disabling Apex while retaining this patch leaves converted potions without their original stamina refill.
