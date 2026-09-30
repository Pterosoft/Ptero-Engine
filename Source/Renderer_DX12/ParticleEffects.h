#pragma once
// .particle files: a particle effect saved on its own, so one authored fire can be placed
// many times and changed in one place. The file is JSON - the effect half of a
// ParticleSystemComponent (see ParticleEffectToJson) plus a small header - and a level
// refers to it by its Data-relative path.

#include "Components.h"

#include <string>
#include <vector>


namespace ParticleEffects
{
    // Where new effects go, and the extension that marks one.
    inline constexpr const char* kFolder = "Particles";
    inline constexpr const char* kExtension = ".particle";
    // The effect a newly placed Particle System entity starts with. Written from the
    // built-in defaults the first time it is needed, if it does not exist yet.
    inline constexpr const char* kDefaultEffect = "Particles/Fire.particle";

    // Absolute path of a Data-relative one, in whatever form DataFiles reads from (a
    // virtual path inside the archives in a packaged game).
    std::string AbsolutePath(const std::string& relativePath);

    // Reads the effect settings of a .particle file into `effect`, leaving its Enabled
    // flag and ParticlePath alone. False, with `error` set, when the file is missing or
    // is not valid JSON; `effect` is then untouched.
    bool Load(const std::string& relativePath, ParticleSystemComponent& effect, std::string* error = nullptr);

    // Writes the effect settings of `effect` to a .particle file, creating folders as
    // needed. Editor only - a packaged game's Data is read-only.
    bool Save(const std::string& relativePath, const ParticleSystemComponent& effect, std::string* error = nullptr);

    // Data-relative paths ("Particles/Fire.particle") of every .particle file under Data,
    // sorted. Scans the disk each call; callers cache.
    std::vector<std::string> List();

    // Loads the file behind every particle system in `entities` that names one. Each file
    // is read once however many entities share it. A file that cannot be read leaves its
    // emitters on their current settings and adds a line to `problems`, if given.
    // (No logging here: the editor executable compiles this without the engine log.)
    void LoadAll(std::vector<Entity>& entities, std::vector<std::string>* problems = nullptr);

    // Makes sure kDefaultEffect exists on disk, writing it from the built-in defaults if
    // not. Returns its Data-relative path, or empty (with `error` set) if it could not be
    // written.
    std::string EnsureDefaultEffect(std::string* error = nullptr);
}
