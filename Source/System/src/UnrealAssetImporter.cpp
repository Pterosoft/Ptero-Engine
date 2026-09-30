#include "System/UnrealAssetImporter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "System/CollisionGenerator.h"
#include "System/FbxCompiler.h"
#include "System/TextureImporter.h"

#include "Unreal/UnrealMaterial.h"
#include "Unreal/UnrealOodle.h"
#include "Unreal/UnrealPackage.h"
#include "Unreal/UnrealStaticMesh.h"
#include "Unreal/UnrealTexture.h"

#include "..\SDKs\DirectXTex\DirectXTex\DirectXTex.h"
#include "..\SDKs\nlohmann\json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <semaphore>
#include <set>
#include <thread>

namespace
{
    namespace fs = std::filesystem;
    using namespace Ptero::Unreal;
    using Role = TextureImporter::Role;

    std::string Lower(std::string value)
    {
        for (char& c : value)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }

    // Lower-case letters and digits only: "Base Color" and "base_color" compare equal.
    std::string Simplify(const std::string& value)
    {
        std::string result;
        for (const char c : value)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
            {
                result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        return result;
    }

    bool IsInside(const fs::path& root, const fs::path& path)
    {
        const std::string rootText = Lower(fs::weakly_canonical(root).generic_string());
        const std::string pathText = Lower(fs::weakly_canonical(path).generic_string());
        return pathText.size() > rootText.size() && pathText.compare(0, rootText.size(), rootText) == 0 && pathText[rootText.size()] == '/';
    }

    std::string DataRelative(const fs::path& dataDirectory, const fs::path& file)
    {
        return fs::weakly_canonical(file).lexically_relative(fs::weakly_canonical(dataDirectory)).generic_string();
    }

    // Outputs older than this were written by an importer with a decoding bug and are
    // redone even when newer than their source. Bump it whenever a fix changes what the
    // importer produces for the same input. (2026-09-26: PNG-compressed BGRA8 sources
    // came out with red and blue swapped.)
    fs::file_time_type ImporterRevisionTime()
    {
        using namespace std::chrono;
        const sys_seconds revision = sys_days{ year{ 2026 } / September / 26 } + hours{ 19 } + minutes{ 25 };
        return clock_cast<file_clock>(revision);
    }

    bool IsUpToDate(const fs::path& output, const fs::path& source)
    {
        std::error_code ec;
        if (!fs::exists(output, ec))
        {
            return false;
        }
        const fs::file_time_type written = fs::last_write_time(output, ec);
        return written >= fs::last_write_time(source, ec) && written >= ImporterRevisionTime();
    }

    std::string ObjectName(const std::string& objectPath)
    {
        const std::size_t dot = objectPath.rfind('.');
        const std::size_t colon = objectPath.rfind(':');
        const std::size_t start = (std::max)(dot == std::string::npos ? 0 : dot + 1, colon == std::string::npos ? 0 : colon + 1);
        std::string name = objectPath.substr(start);
        if (dot == std::string::npos && colon == std::string::npos)
        {
            name = objectPath.substr(objectPath.rfind('/') + 1);
        }
        return name;
    }

    std::string PackageNameOf(const std::string& objectPath)
    {
        return objectPath.substr(0, objectPath.find('.'));
    }

    // What a texture feeds, guessed from a parameter or asset name when the material graph
    // could not be read. Packed textures come back as their layout ("ORM", "ARM", "MRA",
    // "RMA").
    std::string ClassifyTextureName(const std::string& name)
    {
        std::vector<std::string> tokens;
        std::string token;
        for (const char c : name)
        {
            if (c == '_' || c == ' ' || c == '-' || c == '.')
            {
                if (!token.empty()) tokens.push_back(Lower(token));
                token.clear();
            }
            else
            {
                token.push_back(c);
            }
        }
        if (!token.empty()) tokens.push_back(Lower(token));

        static const std::map<std::string, std::string> kSuffixes = {
            { "bc", "BaseColor" }, { "d", "BaseColor" }, { "diff", "BaseColor" }, { "diffuse", "BaseColor" },
            { "albedo", "BaseColor" }, { "basecolor", "BaseColor" }, { "color", "BaseColor" }, { "col", "BaseColor" },
            { "n", "Normal" }, { "nrm", "Normal" }, { "nor", "Normal" }, { "nm", "Normal" }, { "normal", "Normal" },
            { "r", "Roughness" }, { "rough", "Roughness" }, { "roughness", "Roughness" },
            { "m", "Metallic" }, { "metal", "Metallic" }, { "metallic", "Metallic" }, { "metalness", "Metallic" },
            { "ao", "AmbientOcclusion" }, { "occlusion", "AmbientOcclusion" }, { "ambientocclusion", "AmbientOcclusion" },
            { "orm", "ORM" }, { "arm", "ARM" }, { "mra", "MRA" }, { "rma", "RMA" }, { "mrao", "MRA" },
            { "e", "EmissiveColor" }, { "emissive", "EmissiveColor" }, { "emission", "EmissiveColor" },
            { "h", "Displacement" }, { "height", "Displacement" }, { "disp", "Displacement" }, { "displacement", "Displacement" },
            { "o", "Opacity" }, { "opacity", "Opacity" }, { "alpha", "Opacity" },
        };
        for (auto it = tokens.rbegin(); it != tokens.rend(); ++it)
        {
            const auto found = kSuffixes.find(*it);
            if (found != kSuffixes.end())
            {
                return found->second;
            }
        }

        const std::string simple = Simplify(name);
        if (simple.find("normal") != std::string::npos) return "Normal";
        if (simple.find("rough") != std::string::npos) return "Roughness";
        if (simple.find("metal") != std::string::npos) return "Metallic";
        if (simple.find("occlusion") != std::string::npos) return "AmbientOcclusion";
        if (simple.find("emissi") != std::string::npos) return "EmissiveColor";
        if (simple.find("height") != std::string::npos || simple.find("displace") != std::string::npos) return "Displacement";
        if (simple.find("opacity") != std::string::npos) return "Opacity";
        if (simple.find("albedo") != std::string::npos || simple.find("basecolo") != std::string::npos ||
            simple.find("diffuse") != std::string::npos) return "BaseColor";
        return {};
    }

    // Channel of each packed component (roughness, metallic, occlusion) for a packed layout.
    bool PackedLayout(const std::string& layout, int& roughness, int& metallic, int& occlusion)
    {
        if (layout == "ORM" || layout == "ARM") { occlusion = 0; roughness = 1; metallic = 2; return true; }
        if (layout == "MRA") { metallic = 0; roughness = 1; occlusion = 2; return true; }
        if (layout == "RMA") { roughness = 0; metallic = 1; occlusion = 2; return true; }
        return false;
    }

    // One channel of a packed output texture: where it comes from, or a constant.
    struct ChannelSource
    {
        std::string TexturePath;
        int Channel = 0;
        std::uint8_t Constant = 0;
    };

    // Shared by every import session in the process: the editor runs several .uasset
    // imports at once, and they must neither exceed one memory budget for encodes nor
    // write the same .dds concurrently (two meshes sharing a texture).
    struct EncodeRegistry
    {
        // Each 4K encode holds a few hundred MB, so concurrency is capped well below the
        // core count.
        static std::ptrdiff_t ConcurrentEncodes()
        {
            const unsigned cores = std::thread::hardware_concurrency();
            return (std::clamp)(static_cast<std::ptrdiff_t>(cores / 2), std::ptrdiff_t{ 1 }, std::ptrdiff_t{ 6 });
        }

        std::mutex Mutex;
        std::condition_variable Released;
        std::set<std::string> InFlight;
        std::counting_semaphore<64> Slots{ ConcurrentEncodes() };
    };

    // Never destroyed: workers may still be finishing when the DLL unloads.
    EncodeRegistry& Registry()
    {
        static EncodeRegistry* registry = new EncodeRegistry();
        return *registry;
    }

    class ImportSession
    {
    public:
        ImportSession(fs::path source, fs::path dataDirectory, fs::path targetDirectory, UnrealAssetImporter::Result& result)
            : m_Source(std::move(source)), m_Data(std::move(dataDirectory)), m_Target(std::move(targetDirectory)), m_Result(result)
        {
        }

        ~ImportSession()
        {
            WaitForBackgroundWork();
        }

        bool Run(std::string& error)
        {
            const Package* package = LoadFile(m_Source, error);
            if (package == nullptr)
            {
                return false;
            }

            const ObjectExport* main = package->MainExport();
            const std::string assetClass = main != nullptr ? package->ExportClassName(*main) : std::string{};
            bool imported = false;
            if (assetClass == "StaticMesh") imported = ImportStaticMesh(*package, error);
            else if (assetClass == "Texture2D" || assetClass == "TextureCube") imported = ImportStandaloneTexture(*package, error);
            else if (assetClass == "MaterialInstanceConstant" || assetClass == "Material") imported = ImportStandaloneMaterial(*package, error);
            else
            {
                if (assetClass == "SkeletalMesh") error = "skeletal meshes are not supported yet; only static meshes can be imported";
                else if (assetClass == "World") error = "levels (.umap) cannot be imported; import the meshes they use instead";
                else error = "'" + (assetClass.empty() ? std::string("unknown") : assetClass) + "' assets are not supported";
                return false;
            }
            WaitForBackgroundWork();
            return imported;

        }

    private:
        // ---- Package lookup --------------------------------------------------------

        const Package* LoadFile(const fs::path& file, std::string& error)
        {
            const std::string key = Lower(fs::weakly_canonical(file).generic_string());
            if (const auto cached = m_Packages.find(key); cached != m_Packages.end())
            {
                return cached->second.get();
            }

            auto package = std::make_unique<Package>();
            if (!package->Load(file, error))
            {
                error = file.filename().string() + ": " + error;
                return nullptr;
            }
            RegisterMount(*package);
            const Package* result = package.get();
            m_Packages.emplace(key, std::move(package));
            return result;
        }

        // "/Game/Props/Mesh/SM_Box" saved at ".../Content/Props/Mesh/SM_Box.uasset" tells
        // us /Game maps to ".../Content".
        void RegisterMount(const Package& package)
        {
            const std::string& name = package.PackageName();
            if (name.size() < 2 || name[0] != '/')
            {
                return;
            }
            const std::size_t mountEnd = name.find('/', 1);
            if (mountEnd == std::string::npos)
            {
                return;
            }
            const std::string mount = name.substr(1, mountEnd - 1);
            if (m_MountRoots.count(mount) != 0)
            {
                return;
            }

            std::vector<std::string> segments;
            std::size_t start = mountEnd + 1;
            while (start < name.size())
            {
                const std::size_t end = name.find('/', start);
                segments.push_back(name.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            if (segments.empty() || Lower(package.Path().stem().string()) != Lower(segments.back()))
            {
                return;
            }

            fs::path root = package.Path().parent_path();
            for (std::size_t index = segments.size() - 1; index-- > 0;)
            {
                if (Lower(root.filename().string()) != Lower(segments[index]))
                {
                    return;
                }
                root = root.parent_path();
            }
            m_MountRoots[mount] = root;
        }

        void BuildFileIndex()
        {
            if (m_Indexed)
            {
                return;
            }
            m_Indexed = true;

            std::vector<fs::path> roots;
            if (const auto game = m_MountRoots.find("Game"); game != m_MountRoots.end())
            {
                roots.push_back(game->second);
            }
            if (IsInside(m_Data, m_Source))
            {
                // Content copied into Data: search the top-level folder it was dropped into.
                const fs::path relative = fs::weakly_canonical(m_Source).lexically_relative(fs::weakly_canonical(m_Data));
                roots.push_back(m_Data / *relative.begin());
            }
            else
            {
                roots.push_back(m_Source.parent_path().parent_path());
            }

            for (const fs::path& root : roots)
            {
                std::error_code ec;
                for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
                {
                    if (ec) break;
                    if (it->is_regular_file(ec) && _stricmp(it->path().extension().string().c_str(), ".uasset") == 0)
                    {
                        m_FileIndex.emplace(Lower(it->path().stem().string()), it->path());
                    }
                }
            }
        }

        fs::path ResolvePackageFile(const std::string& packageName)
        {
            if (packageName.size() < 2 || packageName[0] != '/')
            {
                return {};
            }
            const std::size_t mountEnd = packageName.find('/', 1);
            if (mountEnd == std::string::npos)
            {
                return {};
            }
            const std::string mount = packageName.substr(1, mountEnd - 1);
            if (mount == "Engine" || mount == "Script")
            {
                return {};   // engine content is not part of the purchased asset
            }
            const std::string relative = packageName.substr(mountEnd + 1);

            std::error_code ec;
            if (const auto root = m_MountRoots.find(mount); root != m_MountRoots.end())
            {
                const fs::path candidate = root->second / (relative + ".uasset");
                if (fs::exists(candidate, ec)) return candidate;
            }

            // A plugin's content: <Project>/Plugins/.../<Mount>/Content.
            if (const auto game = m_MountRoots.find("Game"); game != m_MountRoots.end())
            {
                const fs::path plugins = game->second.parent_path() / "Plugins";
                for (fs::recursive_directory_iterator it(plugins, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
                {
                    if (ec) break;
                    if (it.depth() > 3)
                    {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (it->is_directory(ec) && _stricmp(it->path().filename().string().c_str(), mount.c_str()) == 0)
                    {
                        const fs::path candidate = it->path() / "Content" / (relative + ".uasset");
                        if (fs::exists(candidate, ec))
                        {
                            m_MountRoots[mount] = it->path() / "Content";
                            return candidate;
                        }
                    }
                }
            }

            // Last resort: the files were moved; find the asset by name.
            BuildFileIndex();
            const std::string assetName = Lower(packageName.substr(packageName.rfind('/') + 1));
            if (const auto found = m_FileIndex.find(assetName); found != m_FileIndex.end())
            {
                return found->second;
            }
            return {};
        }

        const Package* LoadObject(const std::string& objectPath)
        {
            const fs::path file = ResolvePackageFile(PackageNameOf(objectPath));
            if (file.empty())
            {
                return nullptr;
            }
            std::string error;
            const Package* package = LoadFile(file, error);
            if (package == nullptr)
            {
                Warn(error);
            }
            return package;
        }

        void Warn(const std::string& message)
        {
            std::lock_guard<std::mutex> lock(m_ResultMutex);
            if (std::find(m_Result.Warnings.begin(), m_Result.Warnings.end(), message) == m_Result.Warnings.end())
            {
                m_Result.Warnings.push_back(message);
            }
        }

        // Outputs of assets inside Data go next to them; everything else goes to the
        // chosen target folder (textures in a Textures subfolder).
        fs::path OutputPath(const fs::path& sourceAsset, const std::string& fileName, const bool isTexture) const
        {
            if (IsInside(m_Data, sourceAsset))
            {
                return sourceAsset.parent_path() / fileName;
            }
            return isTexture ? (m_Target / "Textures" / fileName) : (m_Target / fileName);
        }

        void Written(const fs::path& file)
        {
            std::lock_guard<std::mutex> lock(m_ResultMutex);
            m_Result.WrittenFiles.push_back(file);
        }

        // ---- Textures ----------------------------------------------------------------

        struct TextureSummary
        {
            bool Valid = false;
            TextureInfo Info;
            const Package* Owner = nullptr;
        };

        TextureSummary DescribeTexture(const std::string& objectPath)
        {
            TextureSummary summary;
            summary.Owner = LoadObject(objectPath);
            if (summary.Owner == nullptr)
            {
                if (objectPath.rfind("/Engine/", 0) != 0)
                {
                    Warn("texture not found: " + objectPath);
                }
                return summary;
            }
            std::string error;
            summary.Valid = ReadTextureInfo(*summary.Owner, summary.Info, error);
            if (!summary.Valid)
            {
                Warn(ObjectName(objectPath) + ": " + error);
            }
            return summary;
        }

        static bool IsPlaceholder(const TextureInfo& info)
        {
            return info.Width <= 4 && info.Height <= 4;
        }

        // Average colour of a (placeholder) texture, 0-1 per channel.
        std::optional<std::array<float, 4>> AverageColor(const Package& owner)
        {
            TextureInfo info;
            DirectX::ScratchImage image;
            std::string error;
            if (!ReadTextureSource(owner, info, image, error))
            {
                return std::nullopt;
            }
            DirectX::ScratchImage floats;
            if (FAILED(DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, floats)))
            {
                return std::nullopt;
            }
            const DirectX::Image* pixels = floats.GetImage(0, 0, 0);
            std::array<float, 4> sum{ 0, 0, 0, 0 };
            const std::size_t count = pixels->width * pixels->height;
            const float* data = reinterpret_cast<const float*>(pixels->pixels);
            for (std::size_t index = 0; index < count; ++index)
            {
                for (int channel = 0; channel < 4; ++channel) sum[channel] += data[index * 4 + channel];
            }
            for (float& channel : sum) channel /= static_cast<float>((std::max<std::size_t>)(count, 1));
            return sum;
        }

        // ---- Background encoding -----------------------------------------------------
        //
        // Decoding and BC-compressing a 4K texture takes seconds, and a Fab pack has dozens,
        // so conversions run on worker threads while the session carries on resolving
        // materials. Only the workers touch pixels; package loading (which mutates the
        // session's caches) stays on the calling thread, and packages stay alive until the
        // session ends, so workers may read them freely.

        // Writes `output` by running `job` on a worker. If another session is already writing the
        // same file, waits for it instead, then encodes only if still out of date.
        void RunInBackground(const fs::path& output, std::vector<fs::path> sources, std::function<void()> job)
        {
            m_Workers.emplace_back([output, sources = std::move(sources), job = std::move(job)]()
            {
                EncodeRegistry& registry = Registry();
                const std::string key = Lower(output.lexically_normal().generic_string());
                {
                    std::unique_lock<std::mutex> lock(registry.Mutex);
                    registry.Released.wait(lock, [&] { return registry.InFlight.count(key) == 0; });
                    const bool current = std::all_of(sources.begin(), sources.end(), [&](const fs::path& source) { return IsUpToDate(output, source); });
                    if (current && !sources.empty())
                    {
                        return;
                    }
                    registry.InFlight.insert(key);
                }

                registry.Slots.acquire();
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC for PNG/JPEG sources
                job();
                CoUninitialize();
                registry.Slots.release();

                {
                    std::lock_guard<std::mutex> lock(registry.Mutex);
                    registry.InFlight.erase(key);
                }
                registry.Released.notify_all();
            });
        }

        void WaitForBackgroundWork()
        {
            for (std::thread& worker : m_Workers)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
            m_Workers.clear();
        }

        // Converts one texture to .dds (in the background) and returns its Data-relative
        // path ("" when it cannot be imported).
        std::string ImportTexture(const std::string& objectPath, std::optional<Role> roleOverride)
        {
            TextureSummary summary = DescribeTexture(objectPath);
            if (!summary.Valid)
            {
                return {};
            }

            const Role role = roleOverride.has_value()
                ? *roleOverride
                : summary.Info.IsNormalMap() ? Role::Normal : summary.Info.IsLinear() ? Role::Linear : Role::Color;

            const fs::path output = OutputPath(summary.Owner->Path(), summary.Info.Name + ".dds", true);
            const std::string key = Lower(output.generic_string());
            if (const auto done = m_Textures.find(key); done != m_Textures.end())
            {
                return done->second;
            }

            if (!IsUpToDate(output, summary.Owner->Path()))
            {
                const Package* owner = summary.Owner;
                const std::string name = summary.Info.Name;
                RunInBackground(output, { owner->Path() }, [this, owner, name, output, role]()
                {
                    TextureInfo info;
                    DirectX::ScratchImage image;
                    std::string error;
                    if (!ReadTextureSource(*owner, info, image, error))
                    {
                        Warn(name + ": " + error);
                        return;
                    }
                    TextureImporter importer;
                    if (!importer.ImportImage(image, output.string(), role))
                    {
                        Warn(name + ": " + importer.LastError());
                        return;
                    }
                    Written(output);
                });
            }

            const std::string relative = DataRelative(m_Data, output);
            m_Textures[key] = relative;
            return relative;
        }

        // Decodes a texture into 8-bit BGRA with its stored values untouched (no sRGB
        // conversion), for channel packing. Thread-safe.
        bool DecodeRaw8(const Package& owner, DirectX::ScratchImage& out)
        {
            TextureInfo info;
            DirectX::ScratchImage image;
            std::string error;
            if (!ReadTextureSource(owner, info, image, error))
            {
                Warn(owner.Path().stem().string() + ": " + error);
                return false;
            }
            if (image.GetMetadata().format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            {
                image.OverrideFormat(DXGI_FORMAT_B8G8R8A8_UNORM);
            }
            if (image.GetMetadata().format == DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                out = std::move(image);
                return true;
            }
            return SUCCEEDED(DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_B8G8R8A8_UNORM,
                DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, out));
        }

        // Builds a linear texture whose R, G and B come from the given sources. Used for
        // Ptero's packed roughness/metallic/occlusion map and for pulling a single channel
        // out of a packed Unreal texture.
        std::string ImportPacked(const fs::path& outputNear, const std::string& outputName, const std::array<ChannelSource, 3>& sources)
        {
            const fs::path output = OutputPath(outputNear, outputName + ".dds", true);
            const std::string key = Lower(output.generic_string());
            if (const auto done = m_Textures.find(key); done != m_Textures.end())
            {
                return done->second;
            }

            // Load every source package here, on the session thread.
            std::map<std::string, const Package*> owners;
            bool upToDate = fs::exists(output);
            for (const ChannelSource& source : sources)
            {
                if (source.TexturePath.empty() || owners.count(source.TexturePath) != 0)
                {
                    continue;
                }
                const Package* owner = LoadObject(source.TexturePath);
                if (owner == nullptr)
                {
                    Warn("texture not found: " + source.TexturePath);
                    return {};
                }
                owners[source.TexturePath] = owner;
                upToDate = upToDate && IsUpToDate(output, owner->Path());
            }

            if (!upToDate)
            {
                std::vector<fs::path> sourceFiles;
                for (const auto& [path, owner] : owners)
                {
                    sourceFiles.push_back(owner->Path());
                }
                RunInBackground(output, std::move(sourceFiles), [this, owners, sources, output, outputName]()
                {
                    BuildPacked(owners, sources, output, outputName);
                });
            }

            const std::string relative = DataRelative(m_Data, output);
            m_Textures[key] = relative;
            return relative;
        }

        void BuildPacked(const std::map<std::string, const Package*>& owners, const std::array<ChannelSource, 3>& sources,
            const fs::path& output, const std::string& outputName)
        {
            {
                std::map<std::string, DirectX::ScratchImage> decoded;
                std::size_t width = 1, height = 1;
                for (const auto& [path, owner] : owners)
                {
                    DirectX::ScratchImage image;
                    if (!DecodeRaw8(*owner, image))
                    {
                        return;
                    }
                    width = (std::max)(width, image.GetMetadata().width);
                    height = (std::max)(height, image.GetMetadata().height);
                    decoded.emplace(path, std::move(image));
                }
                for (auto& [path, image] : decoded)
                {
                    if (image.GetMetadata().width != width || image.GetMetadata().height != height)
                    {
                        DirectX::ScratchImage resized;
                        if (FAILED(DirectX::Resize(*image.GetImage(0, 0, 0), width, height, DirectX::TEX_FILTER_DEFAULT | DirectX::TEX_FILTER_FORCE_NON_WIC, resized)))
                        {
                            Warn("could not resize " + ObjectName(path) + " for packing");
                            return;
                        }
                        image = std::move(resized);
                    }
                }

                DirectX::ScratchImage packed;
                if (FAILED(packed.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, 1)))
                {
                    Warn(outputName + ": out of memory");
                    return;
                }
                const DirectX::Image* target = packed.GetImage(0, 0, 0);
                static constexpr int kByteOfChannel[4] = { 2, 1, 0, 3 };   // RGBA -> BGRA offset
                for (int outChannel = 0; outChannel < 3; ++outChannel)
                {
                    const ChannelSource& source = sources[static_cast<std::size_t>(outChannel)];
                    const DirectX::Image* input = source.TexturePath.empty() ? nullptr : decoded.at(source.TexturePath).GetImage(0, 0, 0);
                    const int inByte = kByteOfChannel[(std::clamp)(source.Channel, 0, 3)];
                    const int outByte = kByteOfChannel[outChannel];
                    for (std::size_t y = 0; y < height; ++y)
                    {
                        std::uint8_t* dst = target->pixels + y * target->rowPitch;
                        const std::uint8_t* src = input != nullptr ? input->pixels + y * input->rowPitch : nullptr;
                        for (std::size_t x = 0; x < width; ++x)
                        {
                            dst[x * 4 + outByte] = src != nullptr ? src[x * 4 + inByte] : source.Constant;
                        }
                    }
                }
                for (std::size_t y = 0; y < height; ++y)
                {
                    std::uint8_t* dst = target->pixels + y * target->rowPitch;
                    for (std::size_t x = 0; x < width; ++x) dst[x * 4 + 3] = 255;
                }

                TextureImporter importer;
                if (!importer.ImportImage(packed, output.string(), Role::Linear))
                {
                    Warn(outputName + ": " + importer.LastError());
                    return;
                }
                Written(output);
            }
        }

        // ---- Materials ---------------------------------------------------------------

        nlohmann::json DefaultMaterialJson(const std::string& name) const
        {
            return nlohmann::json{
                { "name", name },
                { "baseColorTint", { 1.0f, 1.0f, 1.0f, 1.0f } },
                { "emissiveColor", { 0.0f, 0.0f, 0.0f } },
                { "metallicFactor", 0.0f },
                { "roughnessFactor", 0.5f },
                { "specularFactor", 0.5f },
                { "normalScale", 1.0f },
                { "normalFlipGreen", false },
                { "ambientOcclusionStrength", 1.0f },
                { "heightScale", 0.05f },
                { "heightReference", 1.0f },
                { "opacity", 1.0f },
                { "alphaCutoff", 0.333f },
                { "doubleSided", false },
                { "useAlphaCutout", false },
                { "useTransparentBlend", false },
                { "textures", {
                    { "baseColor", "" }, { "normal", "" }, { "metallic", "" }, { "roughness", "" },
                    { "metallicRoughness", "" }, { "ambientOcclusion", "" }, { "emissive", "" },
                    { "height", "" }, { "opacity", "" },
                } },
            };
        }

        static std::optional<float> FindScalar(const ResolvedMaterial& material, std::initializer_list<const char*> names)
        {
            for (const auto& [name, value] : material.Scalars)
            {
                const std::string simple = Simplify(name);
                for (const char* candidate : names)
                {
                    if (simple == candidate) return value;
                }
            }
            return std::nullopt;
        }

        nlohmann::json BuildMaterialJson(const ResolvedMaterial& material, const fs::path& outputNear)
        {
            nlohmann::json json = DefaultMaterialJson(material.Name);
            nlohmann::json& textures = json["textures"];

            // Start from the inputs the graph tied textures to, then fill gaps from names.
            std::map<std::string, TextureBinding> inputs = material.Inputs;
            for (const TextureBinding& loose : material.LooseTextures)
            {
                std::string kind = ClassifyTextureName(loose.ParameterName);
                if (kind.empty()) kind = ClassifyTextureName(ObjectName(loose.TexturePath));
                int roughness = -1, metallic = -1, occlusion = -1;
                if (PackedLayout(kind, roughness, metallic, occlusion))
                {
                    if (!inputs.count("Roughness")) inputs["Roughness"] = TextureBinding{ loose.TexturePath, roughness, loose.ParameterName };
                    if (!inputs.count("Metallic")) inputs["Metallic"] = TextureBinding{ loose.TexturePath, metallic, loose.ParameterName };
                    if (!inputs.count("AmbientOcclusion")) inputs["AmbientOcclusion"] = TextureBinding{ loose.TexturePath, occlusion, loose.ParameterName };
                }
                else if (!kind.empty() && !inputs.count(kind))
                {
                    inputs[kind] = loose;
                }
            }
            if (!inputs.count("Opacity") && inputs.count("OpacityMask"))
            {
                inputs["Opacity"] = inputs["OpacityMask"];
            }

            // Placeholder textures (1x1 defaults of master materials) become constants.
            std::map<std::string, TextureSummary> summaries;
            for (auto it = inputs.begin(); it != inputs.end();)
            {
                TextureSummary summary = DescribeTexture(it->second.TexturePath);
                if (!summary.Valid)
                {
                    it = inputs.erase(it);
                    continue;
                }
                if (IsPlaceholder(summary.Info))
                {
                    if (const auto color = AverageColor(*summary.Owner))
                    {
                        const int channel = (std::max)(it->second.Channel, 0);
                        if (it->first == "BaseColor") json["baseColorTint"] = { (*color)[0], (*color)[1], (*color)[2], 1.0f };
                        else if (it->first == "Roughness") json["roughnessFactor"] = (*color)[static_cast<std::size_t>(channel)];
                        else if (it->first == "Metallic") json["metallicFactor"] = (*color)[static_cast<std::size_t>(channel)];
                    }
                    it = inputs.erase(it);
                    continue;
                }
                summaries[it->first] = summary;
                ++it;
            }

            auto binding = [&inputs](const char* input) -> const TextureBinding*
            {
                const auto found = inputs.find(input);
                return found != inputs.end() ? &found->second : nullptr;
            };

            if (const TextureBinding* baseColor = binding("BaseColor"))
            {
                textures["baseColor"] = ImportTexture(baseColor->TexturePath, Role::Color);
            }
            if (const TextureBinding* normal = binding("Normal"))
            {
                textures["normal"] = ImportTexture(normal->TexturePath, Role::Normal);
            }
            if (const TextureBinding* emissive = binding("EmissiveColor"))
            {
                textures["emissive"] = ImportTexture(emissive->TexturePath, Role::Color);
                const float intensity = FindScalar(material, { "emissiveintensity", "emissivestrength", "emissive", "emissivemultiplier" }).value_or(1.0f);
                json["emissiveColor"] = { intensity, intensity, intensity };
            }

            // Roughness / metallic / occlusion: separate greyscale maps are used as they are;
            // anything packed or read from a non-red channel is repacked into Ptero's layout
            // (R roughness, G metallic, B occlusion).
            const TextureBinding* roughness = binding("Roughness");
            const TextureBinding* metallic = binding("Metallic");
            const TextureBinding* occlusion = binding("AmbientOcclusion");
            auto singleChannelOk = [](const TextureBinding* b) { return b == nullptr || b->Channel <= 0; };
            auto same = [](const TextureBinding* a, const TextureBinding* b) { return a != nullptr && b != nullptr && a->TexturePath == b->TexturePath; };
            const bool needsPacking = !singleChannelOk(roughness) || !singleChannelOk(metallic) || !singleChannelOk(occlusion) ||
                same(roughness, metallic) || same(roughness, occlusion) || same(metallic, occlusion);

            const float roughnessConstant = FindScalar(material, { "roughness", "roughnessvalue" }).value_or(json["roughnessFactor"].get<float>());
            const float metallicConstant = FindScalar(material, { "metallic", "metalness", "metallicvalue" }).value_or(json["metallicFactor"].get<float>());

            if (needsPacking)
            {
                auto toSource = [](const TextureBinding* b, const std::uint8_t constant)
                {
                    return b != nullptr ? ChannelSource{ b->TexturePath, (std::max)(b->Channel, 0), 0 } : ChannelSource{ {}, 0, constant };
                };
                const std::array<ChannelSource, 3> sources = {
                    toSource(roughness, static_cast<std::uint8_t>((std::clamp)(roughnessConstant, 0.0f, 1.0f) * 255.0f + 0.5f)),
                    toSource(metallic, static_cast<std::uint8_t>((std::clamp)(metallicConstant, 0.0f, 1.0f) * 255.0f + 0.5f)),
                    toSource(occlusion, 255),
                };
                const TextureBinding* first = roughness != nullptr ? roughness : metallic != nullptr ? metallic : occlusion;
                const bool oneSource = (!roughness || roughness->TexturePath == first->TexturePath) &&
                    (!metallic || metallic->TexturePath == first->TexturePath) &&
                    (!occlusion || occlusion->TexturePath == first->TexturePath);
                const std::string name = (oneSource ? ObjectName(first->TexturePath) : material.Name) + "_RMA";
                const Package* owner = LoadObject(first->TexturePath);
                const std::string packed = ImportPacked(owner != nullptr ? owner->Path() : outputNear, name, sources);
                if (!packed.empty())
                {
                    textures["metallicRoughness"] = packed;
                    json["roughnessFactor"] = 1.0f;
                    json["metallicFactor"] = 1.0f;
                }
            }
            else
            {
                if (roughness != nullptr)
                {
                    textures["roughness"] = ImportTexture(roughness->TexturePath, Role::Linear);
                    json["roughnessFactor"] = 1.0f;
                }
                else
                {
                    json["roughnessFactor"] = roughnessConstant;
                }
                if (metallic != nullptr)
                {
                    textures["metallic"] = ImportTexture(metallic->TexturePath, Role::Linear);
                    json["metallicFactor"] = 1.0f;
                }
                else
                {
                    json["metallicFactor"] = metallicConstant;
                }
                if (occlusion != nullptr)
                {
                    textures["ambientOcclusion"] = ImportTexture(occlusion->TexturePath, Role::Linear);
                }
            }

            // Opacity read from the base colour's alpha is what Ptero does already.
            if (const TextureBinding* opacity = binding("Opacity"))
            {
                const TextureBinding* baseColor = binding("BaseColor");
                const bool fromBaseAlpha = baseColor != nullptr && baseColor->TexturePath == opacity->TexturePath && opacity->Channel == 3;
                if (!fromBaseAlpha)
                {
                    if (opacity->Channel <= 0)
                    {
                        textures["opacity"] = ImportTexture(opacity->TexturePath, Role::Linear);
                    }
                    else
                    {
                        const Package* owner = LoadObject(opacity->TexturePath);
                        const ChannelSource channel{ opacity->TexturePath, opacity->Channel, 0 };
                        textures["opacity"] = ImportPacked(owner != nullptr ? owner->Path() : outputNear,
                            ObjectName(opacity->TexturePath) + "_Opacity", { channel, channel, channel });
                    }
                }
            }

            // Height maps are imported but parallax stays off until enabled in the editor:
            // Unreal content often feeds them to Nanite displacement at a scale that means
            // nothing to Ptero's parallax.
            if (const TextureBinding* height = binding("Displacement"))
            {
                textures["height"] = ImportTexture(height->TexturePath, Role::Linear);
            }

            // A tint multiplies the texture; without a texture a plain colour parameter (the
            // Interchange FBX fallback materials use DiffuseColor) is the albedo itself.
            static const std::set<std::string> kColorParameters = {
                "basecolor", "basecolour", "color", "colour", "diffusecolor", "diffuse", "albedo", "albedocolor",
            };
            const std::array<float, 4>* tint = nullptr;
            for (const auto& [name, value] : material.Vectors)
            {
                if (Simplify(name).find("tint") != std::string::npos)
                {
                    tint = &value;
                    break;
                }
            }
            if (tint == nullptr && !binding("BaseColor"))
            {
                for (const auto& [name, value] : material.Vectors)
                {
                    if (kColorParameters.count(Simplify(name)) != 0)
                    {
                        tint = &value;
                        break;
                    }
                }
            }
            if (tint != nullptr)
            {
                json["baseColorTint"] = { (*tint)[0], (*tint)[1], (*tint)[2], 1.0f };
            }
            if (const auto normalIntensity = FindScalar(material, { "normalintensity", "normalstrength", "normalscale", "normalamount" }))
            {
                json["normalScale"] = *normalIntensity;
            }

            json["doubleSided"] = material.TwoSided;
            if (material.BlendMode == "BLEND_Masked")
            {
                json["useAlphaCutout"] = true;
            }
            else if (material.BlendMode == "BLEND_Translucent" || material.BlendMode == "BLEND_Additive" ||
                material.BlendMode == "BLEND_AlphaComposite" || material.BlendMode == "BLEND_AlphaHoldout")
            {
                json["useTransparentBlend"] = true;
                if (const auto opacity = FindScalar(material, { "opacity" }))
                {
                    json["opacity"] = *opacity;
                }
            }
            return json;
        }

        nlohmann::json ResolveMaterialJson(const std::string& materialPath, const fs::path& outputNear, const std::string& fallbackName)
        {
            if (materialPath.empty() || materialPath.rfind("/Engine/", 0) == 0)
            {
                return DefaultMaterialJson(fallbackName);
            }

            ResolvedMaterial material;
            std::string error;
            const PackageLoader loader = [this](const std::string& objectPath) { return LoadObject(objectPath); };
            if (!ResolveMaterial(materialPath, loader, material, error))
            {
                Warn(error);
                return DefaultMaterialJson(ObjectName(materialPath));
            }
            return BuildMaterialJson(material, outputNear);
        }

        bool WriteJson(const fs::path& path, const nlohmann::json& json, std::string& error)
        {
            fs::create_directories(path.parent_path());
            std::ofstream stream(path);
            if (!stream)
            {
                error = "cannot write " + path.string();
                return false;
            }
            stream << json.dump(4);
            Written(path);
            return true;
        }

        // ---- Asset kinds -------------------------------------------------------------

        bool ImportStaticMesh(const Package& package, std::string& error)
        {
            StaticMeshData mesh;
            if (!ReadStaticMesh(package, mesh, error))
            {
                return false;
            }

            const fs::path pteroPath = OutputPath(package.Path(), mesh.Name + ".ptero", false);
            fs::create_directories(pteroPath.parent_path());
            const std::size_t triangleCount = mesh.Indices.size() / 3;
            if (!FbxCompiler::WritePteroMesh(pteroPath.string(), std::move(mesh.Vertices), std::move(mesh.Indices), std::move(mesh.SubMeshes)))
            {
                error = "could not write " + pteroPath.string();
                return false;
            }
            Written(pteroPath);

            // One sub-material per slot, in slot order: the .ptero's material ids index it.
            // Saved as <mesh>.json beside the geometry, which the editor picks up as the
            // mesh's default material.
            nlohmann::json subMaterials = nlohmann::json::array();
            for (const MeshMaterialSlot& slot : mesh.Slots)
            {
                subMaterials.push_back(ResolveMaterialJson(slot.MaterialPath, package.Path(), slot.SlotName));
            }
            const nlohmann::json multiMaterial{
                { "name", mesh.Name },
                { "type", "MultiMaterial" },
                { "subMaterials", subMaterials },
            };
            const fs::path materialPath = pteroPath.parent_path() / (mesh.Name + ".json");
            if (!WriteJson(materialPath, multiMaterial, error))
            {
                return false;
            }

            // Non-fatal, as for FBX: collisions can be regenerated from the asset browser.
            CollisionGenerator::GenerateCollisions(pteroPath.string());

            m_Result.Summary = "static mesh " + mesh.Name + ": " + std::to_string(triangleCount) + " triangles, " +
                std::to_string(mesh.Slots.size()) + (mesh.Slots.size() == 1 ? " material, " : " materials, ") +
                std::to_string(m_Textures.size()) + (m_Textures.size() == 1 ? " texture" : " textures");
            return true;
        }

        bool ImportStandaloneTexture(const Package& package, std::string& error)
        {
            TextureInfo info;
            if (!ReadTextureInfo(package, info, error))
            {
                return false;
            }
            const std::string relative = ImportTexture(package.PackageName() + "." + info.Name, std::nullopt);
            WaitForBackgroundWork();
            if (relative.empty() || !fs::exists(m_Data / relative))
            {
                error = m_Result.Warnings.empty() ? "texture conversion failed" : m_Result.Warnings.back();
                return false;
            }
            m_Result.Summary = "texture " + info.Name + " (" + std::to_string(info.Width) + "x" + std::to_string(info.Height) + ")";
            return true;
        }

        bool ImportStandaloneMaterial(const Package& package, std::string& error)
        {
            const ObjectExport* main = package.MainExport();
            const std::string objectPath = package.PackageName() + "." + main->ObjectName;
            const nlohmann::json material = ResolveMaterialJson(objectPath, package.Path(), main->ObjectName);
            const fs::path output = OutputPath(package.Path(), main->ObjectName + ".json", false);
            if (!WriteJson(output, material, error))
            {
                return false;
            }
            m_Result.Summary = "material " + main->ObjectName + ": " + std::to_string(m_Textures.size()) +
                (m_Textures.size() == 1 ? " texture" : " textures");
            return true;
        }

        fs::path m_Source;
        fs::path m_Data;
        fs::path m_Target;
        UnrealAssetImporter::Result& m_Result;

        std::map<std::string, std::unique_ptr<Package>> m_Packages;
        std::map<std::string, fs::path> m_MountRoots;
        std::map<std::string, fs::path> m_FileIndex;
        bool m_Indexed = false;
        std::map<std::string, std::string> m_Textures;   // output path -> Data-relative

        std::vector<std::thread> m_Workers;
        std::mutex m_ResultMutex;
    };
}

namespace UnrealAssetImporter
{
    bool Import(
        const std::filesystem::path& uassetPath,
        const std::filesystem::path& dataDirectory,
        const std::filesystem::path& targetDirectory,
        Result& result,
        std::string& error)
    {
        // Embedded PNG/JPEG source art is decoded through WIC. Keep this thread in COM for
        // its lifetime rather than pairing an uninitialize here: DirectXTex caches its WIC
        // factory process-wide, and tearing the apartment down under it breaks later imports.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        try
        {
            ImportSession session(uassetPath, dataDirectory, targetDirectory, result);
            return session.Run(error);
        }
        catch (const std::exception& exception)
        {
            error = std::string("import failed: ") + exception.what();
            return false;
        }
    }

    std::string PeekAssetClass(const std::filesystem::path& uassetPath)
    {
        Package package;
        std::string error;
        if (!package.Load(uassetPath, error, true))
        {
            return {};
        }
        const ObjectExport* main = package.MainExport();
        return main != nullptr ? package.ExportClassName(*main) : std::string{};
    }

    bool IsImportableClass(const std::string& assetClass)
    {
        return assetClass == "StaticMesh" || assetClass == "Texture2D" || assetClass == "TextureCube" ||
            assetClass == "MaterialInstanceConstant" || assetClass == "Material";
    }

    bool HasImportedOutput(const std::filesystem::path& uassetPath)
    {
        const std::string assetClass = PeekAssetClass(uassetPath);
        const char* extension = assetClass == "StaticMesh" ? ".ptero"
            : (assetClass == "Texture2D" || assetClass == "TextureCube") ? ".dds"
            : (assetClass == "Material" || assetClass == "MaterialInstanceConstant") ? ".json"
            : nullptr;
        if (extension == nullptr)
        {
            return true;   // nothing to import, so nothing is pending
        }
        std::filesystem::path output = uassetPath;
        output.replace_extension(extension);
        return IsUpToDate(output, uassetPath);
    }

    bool IsDecompressorAvailable(std::string* reason)
    {
        const bool available = Oodle::IsAvailable();
        if (!available && reason != nullptr)
        {
            *reason = Oodle::UnavailableReason();
        }
        return available;
    }
}
