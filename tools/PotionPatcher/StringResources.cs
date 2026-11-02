using Mutagen.Bethesda;
using Mutagen.Bethesda.Archives;
using Mutagen.Bethesda.Plugins;

namespace Apex.PotionPatcher;

// Resolve only explicitly supplied resources. Mutagen's default BSA discovery also
// consults the machine's game profile, which is unsuitable for an explicit load order.
internal sealed class StringResources : IDisposable
{
    private readonly string directory;
    internal string StringsDirectory { get; }
    internal string EmptyArchiveDirectory { get; }

    internal StringResources(string dataDirectory, IEnumerable<ModKey> plugins)
    {
        directory = Path.Combine(Path.GetTempPath(), "apex-potion-strings-" + Guid.NewGuid().ToString("N"));
        StringsDirectory = Path.Combine(directory, "Strings");
        EmptyArchiveDirectory = Path.Combine(directory, "Archives");
        Directory.CreateDirectory(StringsDirectory);
        Directory.CreateDirectory(EmptyArchiveDirectory);
        try
        {
            var requested = plugins.SelectMany(p => new[] { ".STRINGS", ".DLSTRINGS", ".ILSTRINGS" }
                    .Select(extension => p.Name + "_English" + extension))
                .ToHashSet(StringComparer.OrdinalIgnoreCase);
            var loose = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var folder in Directory.EnumerateDirectories(dataDirectory)
                         .Where(p => StringComparer.OrdinalIgnoreCase.Equals(Path.GetFileName(p), "Strings")))
            foreach (var file in Directory.EnumerateFiles(folder))
            {
                var name = Path.GetFileName(file);
                if (!requested.TryGetValue(name, out var canonicalName)) continue;
                if (!loose.Add(canonicalName)) throw new IOException($"Conflicting loose string resources: {name}.");
                File.Copy(file, Path.Combine(StringsDirectory, canonicalName));
            }
            var archived = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            foreach (var path in Directory.EnumerateFiles(dataDirectory)
                         .Where(p => Path.GetExtension(p).Equals(".bsa", StringComparison.OrdinalIgnoreCase))
                         .OrderBy(p => p, StringComparer.Ordinal))
            {
                var archive = Archive.CreateReader(GameRelease.SkyrimSE, path);
                try
                {
                    if (!archive.TryGetFolder("strings", out var folder)) continue;
                    foreach (var file in folder.Files)
                    {
                        var name = file.Path.Replace('\\', '/').Split('/')[^1];
                        if (!requested.TryGetValue(name, out var canonicalName) || loose.Contains(name)) continue;
                        var bytes = file.GetBytes();
                        if (archived.TryGetValue(name, out var previous))
                        {
                            if (!previous.AsSpan().SequenceEqual(bytes))
                                throw new IOException($"Conflicting archive string resources for {name}; supply the winning loose file in Data/Strings.");
                        }
                        else archived.Add(canonicalName, bytes);
                    }
                }
                finally { if (archive is IDisposable disposable) disposable.Dispose(); }
            }
            foreach (var (name, bytes) in archived)
                File.WriteAllBytes(Path.Combine(StringsDirectory, name), bytes);
        }
        catch { Dispose(); throw; }
    }

    public void Dispose()
    {
        if (Directory.Exists(directory)) Directory.Delete(directory, true);
    }
}
