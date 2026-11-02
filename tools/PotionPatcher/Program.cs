using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Strings;
using Noggog;

namespace Apex.PotionPatcher;

internal static class Program
{
    internal const string Help = """
        Apex potion patcher
        --plugins-json <file> --data-dir <directory> --output <ApexCombat - Potions.esp> [--force]
        --self-test

        The JSON file is an array of physical plugin paths in load order (last wins).
        Relative paths resolve against that JSON file. Supply all active masters and plugins,
        excluding a previous Apex potion patch. Data must contain the English string resources.
        Existing output is preserved unless --force is supplied. Source plugins are never edited.
        """;

    public static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--self-test") return SelfTests.Run();
            if (args.Length == 1 && args[0] is "--help" or "-h") { Console.WriteLine(Help); return 0; }
            var report = Patcher.Generate(Options.Parse(args));
            Console.WriteLine(JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"Potion patch failed: {error.Message}");
            return 1;
        }
    }
}

internal sealed record Options(string PluginsJson, string DataDirectory, string Output, bool Force)
{
    internal static Options Parse(string[] args)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        bool force = false;
        for (int i = 0; i < args.Length; ++i)
        {
            if (args[i] == "--force")
            {
                if (force) throw new ArgumentException("Duplicate --force.");
                force = true;
                continue;
            }
            if (args[i] is not ("--plugins-json" or "--data-dir" or "--output") ||
                i + 1 == args.Length || args[i + 1].StartsWith("--", StringComparison.Ordinal))
                throw new ArgumentException(Program.Help);
            if (!values.TryAdd(args[i], args[++i])) throw new ArgumentException("Duplicate option.");
        }
        if (values.Count != 3) throw new ArgumentException(Program.Help);
        return new(Path.GetFullPath(values["--plugins-json"]), Path.GetFullPath(values["--data-dir"]),
            Path.GetFullPath(values["--output"]), force);
    }
}

internal sealed class PatchReport
{
    public int PluginsRead { get; set; }
    public int WinningIngestibles { get; set; }
    public int PatchedPotions { get; set; }
    public int RestoreEffectsRemoved { get; set; }
    public int SkippedDeleted { get; set; }
    public int SkippedFoodOrPoison { get; set; }
    public int SkippedWithoutCanonicalRestore { get; set; }
    public int SkippedBelowMinimum { get; set; }
    public int SkippedInvalidMagnitude { get; set; }
    public Dictionary<string, int> Tiers { get; } = Patcher.TierNames.ToDictionary(x => x, _ => 0);
}

internal static class Patcher
{
    internal const string OutputName = "ApexCombat - Potions.esp";
    internal const string KeywordPrefix = "ApexPotionStamina";
    internal static readonly ModKey OutputKey = ModKey.FromNameAndExtension(OutputName);
    internal static readonly FormKey RestoreStamina = new(ModKey.FromNameAndExtension("Skyrim.esm"), 0x3EB16);
    internal static readonly FormKey RestoreAllStamina = new(ModKey.FromNameAndExtension("Skyrim.esm"), 0xFFA05);
    internal static bool IsRestoreEffect(FormKey key) => key == RestoreStamina || key == RestoreAllStamina;
    internal static readonly string[] TierNames = ["Minor", "Standard", "Plentiful", "Vigorous", "Extreme", "Ultimate"];
    internal static readonly float[] Thresholds = [25, 50, 75, 100, 150, 9999];
    internal static readonly float[] DefaultDiscounts = [10, 20, 30, 50, 75, 100];
    internal static int DefaultDuration(int tier) => tier == 5 ? 20 : 60;

    internal static int TierFor(float magnitude)
    {
        if (!float.IsFinite(magnitude)) return -1;
        for (int i = Thresholds.Length - 1; i >= 0; --i)
            if (magnitude >= Thresholds[i]) return i;
        return -1;
    }

    internal static PatchReport Generate(Options options)
    {
        if (!Directory.Exists(options.DataDirectory)) throw new DirectoryNotFoundException("Data directory does not exist.");
        if (!StringComparer.OrdinalIgnoreCase.Equals(Path.GetFileName(options.Output), OutputName))
            throw new ArgumentException($"Output filename must be {OutputName}.");
        if (File.Exists(options.Output) && !options.Force) throw new IOException("Output exists; use --force to replace it.");
        var outputDirectory = Path.GetDirectoryName(options.Output)!;
        if (!Directory.Exists(outputDirectory)) throw new DirectoryNotFoundException("Output directory does not exist.");
        var paths = JsonSerializer.Deserialize<string[]>(File.ReadAllText(options.PluginsJson)) ??
            throw new ArgumentException("Plugin JSON must be a nonempty array of paths.");
        if (paths.Length == 0) throw new ArgumentException("Plugin list is empty.");
        var baseDirectory = Path.GetDirectoryName(options.PluginsJson)!;
        var seen = new HashSet<ModKey>();
        var winners = new Dictionary<FormKey, Ingestible>();
        var report = new PatchReport();
        using var strings = new StringResources(options.DataDirectory, paths.Select(p =>
        {
            if (string.IsNullOrWhiteSpace(p)) throw new ArgumentException("Plugin paths cannot be empty.");
            return ModKey.FromNameAndExtension(Path.GetFileName(p));
        }));
        var read = new BinaryReadParameters
        {
            StringsParam = new StringsReadParameters
            {
                BsaFolderOverride = new DirectoryPath(strings.EmptyArchiveDirectory),
                StringsFolderOverride = new DirectoryPath(strings.StringsDirectory),
                TargetLanguage = Language.English
            }
        };
        foreach (var suppliedPath in paths)
        {
            if (string.IsNullOrWhiteSpace(suppliedPath)) throw new ArgumentException("Plugin paths cannot be empty.");
            var path = Path.GetFullPath(suppliedPath, baseDirectory);
            if (!File.Exists(path)) throw new FileNotFoundException("Input plugin does not exist.", path);
            var key = ModKey.FromNameAndExtension(Path.GetFileName(path));
            if (key == OutputKey) throw new ArgumentException("Exclude the previous Apex potion patch from inputs.");
            if (!seen.Add(key)) throw new ArgumentException($"Duplicate input plugin: {key}.");
            using var mod = SkyrimMod.CreateFromBinaryOverlay(path, SkyrimRelease.SkyrimSE, read);
            foreach (var master in mod.ModHeader.MasterReferences)
                if (master.Master == key || !seen.Contains(master.Master))
                    throw new ArgumentException($"{key} has a missing or later master: {master.Master}.");
            foreach (var keyword in mod.Keywords)
                if (keyword.EditorID is string id && TierNames.Any(t => StringComparer.OrdinalIgnoreCase.Equals(id, KeywordPrefix + t)))
                    throw new ArgumentException($"Reserved Apex potion keyword already exists in {key}: {id}.");
            foreach (var ingestible in mod.Ingestibles)
            {
                if (mod.UsingLocalization && ((ingestible.Name is not null && ingestible.Name.String is null) ||
                    (ingestible.Description is not null && ingestible.Description.String is null)))
                    throw new IOException($"Missing English strings for {ingestible.FormKey} in {key}.");
                winners[ingestible.FormKey] = ingestible.DeepCopy();
            }
            ++report.PluginsRead;
        }
        if (!seen.Contains(RestoreStamina.ModKey)) throw new ArgumentException("Plugin list must include Skyrim.esm.");
        report.WinningIngestibles = winners.Count;
        var patch = new SkyrimMod(OutputKey, SkyrimRelease.SkyrimSE);
        patch.IsSmallMaster = true;
        var keywords = TierNames.Select(tier => patch.Keywords.AddNew(KeywordPrefix + tier)).ToArray();
        MagicEffect AddDisplayEffect(string editorId, string tier)
        {
            var marker = patch.MagicEffects.AddNew(editorId);
            marker.Name = tier + " Stamina Efficiency";
            marker.Description = "Reduces the stamina cost of attacks by <mag>% for <dur> seconds.";
            marker.Archetype = new MagicEffectArchetype
            {
                Type = MagicEffectArchetype.TypeEnum.Script,
                ActorValue = ActorValue.None
            };
            marker.CastType = CastType.FireAndForget;
            marker.TargetType = TargetType.Self;
            marker.MagicSkill = ActorValue.None;
            marker.ResistValue = ActorValue.None;
            marker.Flags = MagicEffect.Flag.NoArea |
                MagicEffect.Flag.NoHitEvent | MagicEffect.Flag.NoHitEffect;
            return marker;
        }
        // Allocate each complete group before the next to preserve the runtime's form IDs.
        var receipts = TierNames.Select(tier => AddDisplayEffect("ApexPotionDiscount" + tier, tier)).ToArray();
        var buffs = TierNames.Select(tier => AddDisplayEffect("ApexPotionBuff" + tier, tier)).ToArray();
        for (int tier = 0; tier < TierNames.Length; ++tier)
        {
            var spell = patch.Spells.AddNew("ApexPotionBuffSpell" + TierNames[tier]);
            spell.Name = TierNames[tier] + " Stamina Efficiency";
            spell.Type = SpellType.Spell;
            spell.CastType = CastType.FireAndForget;
            spell.TargetType = TargetType.Self;
            spell.BaseCost = 0;
            spell.Flags = SpellDataFlag.ManualCostCalc | SpellDataFlag.IgnoreResistance |
                SpellDataFlag.NoAbsorbOrReflect | SpellDataFlag.NoDualCastModification;
            spell.Effects.Add(new Effect
            {
                BaseEffect = new FormLinkNullable<IMagicEffectGetter>(buffs[tier].FormKey),
                Data = new EffectData { Magnitude = DefaultDiscounts[tier], Duration = DefaultDuration(tier) }
            });
        }
        foreach (var potion in winners.Values.OrderBy(p => p.FormKey.ToString(), StringComparer.Ordinal))
        {
            if (potion.IsDeleted) { ++report.SkippedDeleted; continue; }
            if ((potion.Flags & (Ingestible.Flag.FoodItem | Ingestible.Flag.Poison)) != 0)
                { ++report.SkippedFoodOrPoison; continue; }
            var restore = potion.Effects.Where(e => IsRestoreEffect(e.BaseEffect.FormKey)).ToArray();
            if (restore.Length == 0) { ++report.SkippedWithoutCanonicalRestore; continue; }
            if (restore.Any(e => e.Data is null || !float.IsFinite(e.Data.Magnitude) || e.Data.Magnitude < 0))
                { ++report.SkippedInvalidMagnitude; continue; }
            var tier = TierFor(restore.Max(e => e.Data!.Magnitude));
            if (tier < 0) { ++report.SkippedBelowMinimum; continue; }
            // DeepCopy preserved all winning fields. Replace the refill with an inert display effect.
            foreach (var effect in restore) potion.Effects.Remove(effect);
            potion.Effects.Add(new Effect
            {
                BaseEffect = new FormLinkNullable<IMagicEffectGetter>(receipts[tier].FormKey),
                Data = new EffectData { Magnitude = DefaultDiscounts[tier], Duration = DefaultDuration(tier) }
            });
            potion.Flags |= Ingestible.Flag.NoAutoCalc;
            potion.Keywords ??= [];
            potion.Keywords.Add(keywords[tier].ToLink());
            patch.Ingestibles.Add(potion);
            ++report.PatchedPotions;
            report.RestoreEffectsRemoved += restore.Length;
            ++report.Tiers[TierNames[tier]];
        }
        var temporaryDirectory = Path.Combine(outputDirectory, ".apex-potions-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporaryDirectory);
        try
        {
            var temporaryOutput = Path.Combine(temporaryDirectory, OutputName);
            patch.WriteToBinary(temporaryOutput, new BinaryWriteParameters
            {
                MastersListOrdering = new MastersListOrderingByLoadOrder(paths.Select(p =>
                    ModKey.FromNameAndExtension(Path.GetFileName(p)))) { Strict = true }
            });
            using (var verification = SkyrimMod.CreateFromBinaryOverlay(temporaryOutput, SkyrimRelease.SkyrimSE))
            {
                if (verification.Keywords.Count != 6 || verification.MagicEffects.Count != 12 ||
                    verification.Spells.Count != 6 || verification.Ingestibles.Count != report.PatchedPotions ||
                    verification.Ingestibles.Any(p => p.Effects.Any(e => IsRestoreEffect(e.BaseEffect.FormKey))))
                    throw new IOException("Written patch failed its validation.");
            }
            File.Move(temporaryOutput, options.Output, options.Force);
        }
        finally { Directory.Delete(temporaryDirectory, true); }
        return report;
    }
}
