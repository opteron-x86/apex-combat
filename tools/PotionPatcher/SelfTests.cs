using System.Security.Cryptography;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Skyrim;

namespace Apex.PotionPatcher;

internal static class SelfTests
{
    private static int checks;
    private static void Check(bool value, string message)
    {
        ++checks;
        if (!value) throw new InvalidOperationException("Self-test: " + message);
    }
    private static void Reject(Action action, string message)
    {
        bool rejected = false;
        try { action(); }
        catch (Exception) { rejected = true; }
        Check(rejected, message);
    }
    private static Effect Restore(float magnitude) => new()
    {
        BaseEffect = new FormLinkNullable<IMagicEffectGetter>(Patcher.RestoreStamina),
        Data = new EffectData { Magnitude = magnitude }
    };
    private static Ingestible Potion(SkyrimMod mod, string id, float magnitude)
    {
        var potion = mod.Ingestibles.AddNew(id);
        potion.Name = id;
        potion.Flags = Ingestible.Flag.Medicine | Ingestible.Flag.NoAutoCalc;
        potion.Weight = 0.5F;
        potion.Value = 42;
        potion.Effects.Add(Restore(magnitude));
        return potion;
    }
    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));

    internal static int Run()
    {
        checks = 0;
        var folder = Path.Combine(Path.GetTempPath(), "apex-potion-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var sky = new SkyrimMod(Patcher.RestoreStamina.ModKey, SkyrimRelease.SkyrimSE);
            sky.UsingLocalization = true;
            sky.MagicEffects.AddNew(Patcher.RestoreStamina).EditorID = "RestoreStamina";
            sky.MagicEffects.AddNew(Patcher.RestoreAllStamina).EditorID = "RestoreAllStamina";
            var otherEffect = sky.MagicEffects.AddNew("UnrelatedEffect");
            var existingKeyword = sky.Keywords.AddNew("PreservedKeyword");
            float[] magnitudes = [0, 24.99F, 25, 49, 50, 74, 75, 99, 100, 149, 150, 9998, 9999, 15000];
            var tierPotions = magnitudes.Select((m, i) => Potion(sky, "Tier" + i, m)).ToArray();
            foreach (var ultimate in tierPotions.Skip(12))
                ultimate.Effects[0].BaseEffect.SetTo(Patcher.RestoreAllStamina);
            var mixedBase = Potion(sky, "MixedPotion", 25);
            mixedBase.Keywords = [existingKeyword.ToLink()];
            var removedBase = Potion(sky, "RestoreRemovedByWinner", 100);
            var reducedBase = Potion(sky, "BelowThresholdInWinner", 50);
            var deletedBase = Potion(sky, "DeletedByWinner", 50);
            var food = Potion(sky, "Food", 100);
            food.Flags |= Ingestible.Flag.FoodItem;
            var poison = Potion(sky, "Poison", 100);
            poison.Flags |= Ingestible.Flag.Poison;
            var invalid = Potion(sky, "InvalidMagnitude", float.NaN);
            var noRestore = sky.Ingestibles.AddNew("NoRestore");
            noRestore.Name = "No restore";
            var skyPath = Path.Combine(folder, "Skyrim.esm");
            sky.WriteToBinary(skyPath);

            var later = new SkyrimMod(ModKey.FromNameAndExtension("Later.esp"), SkyrimRelease.SkyrimSE);
            var mixed = mixedBase.DeepCopy();
            mixed.Name = "Winning mixed potion";
            mixed.Description = "Preserved description";
            mixed.Weight = 0.75F;
            mixed.Value = 777;
            mixed.Flags &= ~Ingestible.Flag.NoAutoCalc;
            mixed.Model = new Model { File = "Clutter/Potions/Winning.nif" };
            mixed.Effects.Clear();
            mixed.Effects.Add(Restore(75));
            mixed.Effects.Add(Restore(10));
            var retainedEffect = new Effect
            {
                BaseEffect = new FormLinkNullable<IMagicEffectGetter>(otherEffect.FormKey),
                Data = new EffectData { Magnitude = 13, Duration = 42, Area = 7 }
            };
            retainedEffect.Conditions.Add(new ConditionFloat { Data = new GetDeadConditionData(), ComparisonValue = 0 });
            mixed.Effects.Add(retainedEffect);
            later.Ingestibles.Add(mixed);
            var removed = removedBase.DeepCopy();
            removed.Effects.Clear();
            removed.Effects.Add(retainedEffect.DeepCopy());
            later.Ingestibles.Add(removed);
            var reduced = reducedBase.DeepCopy();
            reduced.Effects[0].Data!.Magnitude = 20;
            later.Ingestibles.Add(reduced);
            var deleted = deletedBase.DeepCopy();
            deleted.IsDeleted = true;
            later.Ingestibles.Add(deleted);
            var laterPath = Path.Combine(folder, "Later.esp");
            later.UsingLocalization = true;
            later.WriteToBinary(laterPath);
            // The resource stage normalizes filename casing for case-sensitive filesystems.
            var stringsFolder = Path.Combine(folder, "Strings");
            foreach (var path in Directory.EnumerateFiles(stringsFolder))
            {
                var normalized = Path.Combine(stringsFolder, Path.GetFileName(path).ToLowerInvariant());
                if (path != normalized) File.Move(path, normalized);
            }
            var hashes = new[] { Hash(skyPath), Hash(laterPath) };
            var manifest = Path.Combine(folder, "plugins.json");
            File.WriteAllText(manifest, JsonSerializer.Serialize(new[] { "Skyrim.esm", "Later.esp" }));
            var output = Path.Combine(folder, Patcher.OutputName);
            var options = new Options(manifest, folder, output, false);
            var report = Patcher.Generate(options);
            Check(report.PluginsRead == 2, "Both source plugins read");
            Check(report.WinningIngestibles == sky.Ingestibles.Count, "Overrides do not duplicate winning records");
            Check(report.PatchedPotions == 13, "Twelve threshold potions plus mixed winner patched");
            Check(report.RestoreEffectsRemoved == 14, "Every canonical effect removed, including below-tier effect on mixed potion");
            Check(report.SkippedFoodOrPoison == 2, "Food and poison excluded");
            Check(report.SkippedBelowMinimum == 3, "Below-minimum winners preserved");
            Check(report.SkippedWithoutCanonicalRestore == 2, "Removed and unrelated-only winners preserved");
            Check(report.SkippedInvalidMagnitude == 1 && report.SkippedDeleted == 1, "Malformed and deleted winners excluded");
            Check(report.Tiers.Values.Sum() == report.PatchedPotions, "Tier count agrees with output");
            Check(Hash(skyPath) == hashes[0] && Hash(laterPath) == hashes[1], "Source files unchanged");
            using (var patch = SkyrimMod.CreateFromBinaryOverlay(output, SkyrimRelease.SkyrimSE))
            {
                Check(patch.IsSmallMaster && patch.Keywords.Count == 6, "Six new ESL-compatible keywords");
                Check(patch.MagicEffects.Count == 12, "Six receipt and six accepted-dose effects");
                Check(patch.Spells.Count == 6, "Six accepted-dose spells");
                foreach (var marker in patch.MagicEffects)
                {
                    Check(marker.Archetype.Type == MagicEffectArchetype.TypeEnum.Script && marker.VirtualMachineAdapter is null,
                        "Marker is an inert script archetype with no scripts");
                    Check(marker.Archetype.ActorValue == ActorValue.None && marker.BaseCost == 0 &&
                        marker.EquipAbility.IsNull && marker.PerkToApply.IsNull,
                        "Marker has no actor-value, spell, perk or cost payload");
                    Check(marker.TargetType == TargetType.Self && marker.CastType == CastType.FireAndForget,
                        "Marker targets the consumer");
                    Check(marker.Description?.String == "Reduces the stamina cost of attacks by <mag>% for <dur> seconds.",
                        "Marker description uses magnitude and duration substitutions");
                    Check((marker.Flags & (MagicEffect.Flag.HideInUI | MagicEffect.Flag.NoMagnitude |
                        MagicEffect.Flag.NoDuration)) == 0,
                        "Marker is visible and retains magnitude and duration for display");
                }
                foreach (var potion in patch.Ingestibles)
                {
                    var marker = potion.Effects.Single(e => e.BaseEffect.FormKey.ModKey == Patcher.OutputKey);
                    var tier = checked((int)marker.BaseEffect.FormKey.ID - 0x806);
                    Check(tier >= 0 && tier < 6, "Marker form IDs remain stable");
                    float[] percentages = [10, 20, 30, 50, 75, 100];
                    Check(marker.Data!.Duration == (tier == 5 ? 20 : 60) &&
                        marker.Data.Magnitude == percentages[tier] && marker.Data.Area == 0,
                        "Marker shows the default discount and lasts for its full duration");
                }
                for (int tier = 0; tier < Patcher.TierNames.Length; ++tier)
                {
                    var keyword = patch.Keywords.Single(k => k.EditorID == Patcher.KeywordPrefix + Patcher.TierNames[tier]);
                    Check(keyword.FormKey.ID == 0x800 + tier, "Each tier keyword retains its form ID");
                    var marker = patch.MagicEffects.Single(m => m.EditorID == "ApexPotionDiscount" + Patcher.TierNames[tier]);
                    Check(marker.FormKey.ID == 0x806 + tier, "Each tier receipt effect retains its form ID");
                    var buff = patch.MagicEffects.Single(m => m.EditorID == "ApexPotionBuff" + Patcher.TierNames[tier]);
                    Check(buff.FormKey.ID == 0x80C + tier, "Each tier buff effect has its fixed form ID");
                    var spell = patch.Spells.Single(m => m.EditorID == "ApexPotionBuffSpell" + Patcher.TierNames[tier]);
                    Check(spell.FormKey.ID == 0x812 + tier, "Each tier accepted-dose spell has its fixed form ID");
                    Check(spell.Type == SpellType.Spell && spell.CastType == CastType.FireAndForget &&
                        spell.TargetType == TargetType.Self, "Accepted-dose spell targets only the consumer");
                    Check(spell.BaseCost == 0 && spell.Flags.HasFlag(SpellDataFlag.ManualCostCalc),
                        "Accepted-dose spell has no automatic or manual magicka cost");
                    Check(spell.Flags.HasFlag(SpellDataFlag.NoAbsorbOrReflect) &&
                        spell.Flags.HasFlag(SpellDataFlag.IgnoreResistance) &&
                        !spell.Flags.HasFlag(SpellDataFlag.PCStartSpell),
                        "Buff cannot be absorbed or resisted and is not a starting spell");
                    Check(spell.Effects.Count == 1 && spell.Effects[0].BaseEffect.FormKey == buff.FormKey,
                        "Accepted-dose spell applies only its own buff, never a receipt");
                    float[] percentages = [10, 20, 30, 50, 75, 100];
                    Check(spell.Effects[0].Data!.Magnitude == percentages[tier] &&
                        spell.Effects[0].Data!.Duration == (tier == 5 ? 20 : 60),
                        "Accepted-dose spell matches the receipt percentage and duration");
                    Check(spell.Effects[0].Conditions.Count == 0 && marker.Conditions.Count == 0 &&
                        buff.Conditions.Count == 0, "Display effects do not depend on application-condition tricks");
                }
                for (int i = 0; i < magnitudes.Length; ++i)
                {
                    int tier = Patcher.TierFor(magnitudes[i]);
                    var matching = patch.Ingestibles.FirstOrDefault(p => p.FormKey == tierPotions[i].FormKey);
                    Check((matching is not null) == (tier >= 0), "Threshold inclusion " + magnitudes[i]);
                    if (matching is null) continue;
                    var keyword = patch.Keywords.Single(k => k.EditorID == Patcher.KeywordPrefix + Patcher.TierNames[tier]);
                    Check(matching.Effects.Count == 1 && matching.Keywords!.Any(k => k.FormKey == keyword.FormKey), "Correct tier and no refill");
                }
                var result = patch.Ingestibles.Single(p => p.FormKey == mixed.FormKey);
                Check(result.Name?.String == "Winning mixed potion" && result.Description?.String == "Preserved description", "Winning strings preserved");
                Check(result.Value == 777 && result.Weight == 0.75F && result.Flags == (mixed.Flags | Ingestible.Flag.NoAutoCalc), "Stored value fixed, weight and other flags preserved");
                Check(result.Model?.File.GivenPath == "Clutter/Potions/Winning.nif", "Model preserved");
                Check(result.Keywords!.Any(k => k.FormKey == existingKeyword.FormKey), "Existing keyword preserved");
                Check(result.Effects.Count == 2 && result.Effects[0].BaseEffect.FormKey == otherEffect.FormKey, "Unrelated effect and remapped link preserved");
                Check(result.Effects[0].Data!.Magnitude == 13 && result.Effects[0].Data!.Duration == 42 && result.Effects[0].Data!.Area == 7, "Mixed effect data preserved");
                Check(result.Effects[0].Conditions.Count == 1 && result.Effects[0].Conditions[0].Data is IGetDeadConditionDataGetter, "Effect condition preserved");
                Check(!patch.Ingestibles.Any(p => new[] { removed.FormKey, reduced.FormKey, deleted.FormKey, food.FormKey, poison.FormKey, invalid.FormKey, noRestore.FormKey }.Contains(p.FormKey)), "Excluded winning forms absent");
            }
            string originalOutput = Hash(output);
            Directory.Move(stringsFolder, stringsFolder + "-hidden");
            try
            {
                Reject(() => Patcher.Generate(options with { Force = true }), "Missing localized strings rejected");
                Check(Hash(output) == originalOutput, "String resolution failure leaves output intact");
            }
            finally { Directory.Move(stringsFolder + "-hidden", stringsFolder); }
            Reject(() => Patcher.Generate(options), "Existing output needs explicit force");
            Check(Hash(output) == originalOutput, "Rejected overwrite preserves output");
            Patcher.Generate(options with { Force = true });
            Check(Hash(output) == originalOutput, "Repeated generation is deterministic");
            File.WriteAllText(manifest, JsonSerializer.Serialize(new[] { "Later.esp", "Skyrim.esm" }));
            Reject(() => Patcher.Generate(options with { Force = true }), "Later masters rejected");
            Check(Hash(output) == originalOutput, "Failure leaves existing output intact");
            File.WriteAllText(manifest, JsonSerializer.Serialize(new[] { "Skyrim.esm", "Skyrim.esm" }));
            Reject(() => Patcher.Generate(options with { Force = true }), "Duplicate mod keys rejected");
            File.WriteAllText(manifest, JsonSerializer.Serialize(new[] { "Skyrim.esm", Patcher.OutputName }));
            Reject(() => Patcher.Generate(options with { Force = true }), "Prior patch rejected as input");
            Check(Patcher.TierFor(float.PositiveInfinity) == -1 && Patcher.TierFor(float.NaN) == -1 && Patcher.TierFor(-1) == -1, "Nonfinite and negative tier rejected");
            Reject(() => Options.Parse(["--plugins-json", manifest, "--data-dir", folder, "--output", output, "--force", "--force"]), "Duplicate flags rejected");
            Console.WriteLine($"Potion patcher: {checks} checks passed");
            return 0;
        }
        finally { Directory.Delete(folder, true); }
    }
}
