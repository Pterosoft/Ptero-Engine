#include "System/DataFiles.h"
#include "System/PackagedDataApi.h"

#include <windows.h>

#include <cwctype>
#include <fstream>
#include <mutex>
#include <streambuf>

namespace fs = std::filesystem;

namespace
{
    struct PackagedApi
    {
        PteroDataFileSizeFn FileSize = nullptr;
        PteroDataReadFileFn ReadFile = nullptr;
        PteroDataIsDirectoryFn IsDirectory = nullptr;
        PteroDataListFn List = nullptr;
        fs::path Root;
        bool Available = false;
    };

    // Resolved once per module. The exe's export table cannot change while it runs.
    const PackagedApi& Api()
    {
        static PackagedApi api;
        static std::once_flag resolved;
        std::call_once(resolved, []
        {
            HMODULE exe = ::GetModuleHandleW(nullptr);
            if (exe == nullptr)
                return;

            api.FileSize = reinterpret_cast<PteroDataFileSizeFn>(::GetProcAddress(exe, PTERO_DATA_FILE_SIZE_EXPORT));
            api.ReadFile = reinterpret_cast<PteroDataReadFileFn>(::GetProcAddress(exe, PTERO_DATA_READ_FILE_EXPORT));
            api.IsDirectory = reinterpret_cast<PteroDataIsDirectoryFn>(::GetProcAddress(exe, PTERO_DATA_IS_DIRECTORY_EXPORT));
            api.List = reinterpret_cast<PteroDataListFn>(::GetProcAddress(exe, PTERO_DATA_LIST_EXPORT));
            if (api.FileSize == nullptr || api.ReadFile == nullptr || api.IsDirectory == nullptr || api.List == nullptr)
                return;

            wchar_t exePath[MAX_PATH * 4] = {};
            const DWORD length = ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
            if (length == 0 || length >= std::size(exePath))
                return;

            api.Root = fs::path(exePath).parent_path() / L"Data";
            api.Available = true;
        });
        return api;
    }

    std::wstring Lower(std::wstring text)
    {
        for (wchar_t& c : text)
            c = static_cast<wchar_t>(std::towlower(c));
        return text;
    }

    std::string Utf8(const std::wstring& text)
    {
        if (text.empty())
            return {};
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    fs::path FromUtf8(const std::string& text)
    {
        if (text.empty())
            return {};
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
        return fs::path(out);
    }

    // Returns true (and the archive path) when `path` is the packaged root or below it.
    bool ToPackaged(const fs::path& path, std::string& outRelative)
    {
        const PackagedApi& api = Api();
        if (!api.Available || path.empty())
            return false;

        std::error_code error;
        fs::path absolute = path.is_absolute() ? path : fs::absolute(path, error);
        if (error)
            return false;
        absolute = absolute.lexically_normal();

        const std::wstring root = Lower(api.Root.lexically_normal().wstring());
        std::wstring candidate = Lower(absolute.wstring());
        while (!candidate.empty() && (candidate.back() == L'\\' || candidate.back() == L'/'))
            candidate.pop_back();

        if (candidate.size() < root.size() || candidate.compare(0, root.size(), root) != 0)
            return false;
        if (candidate.size() > root.size() && candidate[root.size()] != L'\\' && candidate[root.size()] != L'/')
            return false;   // "<exe>\DataOther" is not under "<exe>\Data"

        // Keep the caller's casing for the relative part; the archive matches it
        // case-insensitively anyway.
        std::wstring relative = absolute.wstring().substr((std::min)(absolute.wstring().size(), root.size()));
        while (!relative.empty() && (relative.front() == L'\\' || relative.front() == L'/'))
            relative.erase(relative.begin());
        for (wchar_t& c : relative)
            if (c == L'\\')
                c = L'/';
        while (!relative.empty() && relative.back() == L'/')
            relative.pop_back();

        outRelative = Utf8(relative);
        return true;
    }

    // An istream over bytes it owns, so a decrypted archive entry can stand in for the
    // ifstream a loader would otherwise have opened.
    class OwnedMemoryBuffer : public std::streambuf
    {
    public:
        explicit OwnedMemoryBuffer(std::vector<std::uint8_t>&& bytes) : mBytes(std::move(bytes))
        {
            char* begin = reinterpret_cast<char*>(mBytes.data());
            setg(begin, begin, begin + mBytes.size());
        }

    protected:
        pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override
        {
            char* target = nullptr;
            if (direction == std::ios_base::beg) target = eback() + offset;
            else if (direction == std::ios_base::cur) target = gptr() + offset;
            else target = egptr() + offset;
            if (target < eback() || target > egptr())
                return pos_type(off_type(-1));
            setg(eback(), target, egptr());
            return pos_type(target - eback());
        }

        pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
        {
            return seekoff(off_type(position), std::ios_base::beg, mode);
        }

    private:
        std::vector<std::uint8_t> mBytes;
    };

    class OwnedMemoryStream : public std::istream
    {
    public:
        explicit OwnedMemoryStream(std::vector<std::uint8_t>&& bytes)
            : std::istream(nullptr), mBuffer(std::move(bytes))
        {
            rdbuf(&mBuffer);
        }

    private:
        OwnedMemoryBuffer mBuffer;
    };
}

namespace DataFiles
{
    bool IsPackaged()
    {
        return Api().Available;
    }

    fs::path PackagedRoot()
    {
        return Api().Root;
    }

    fs::path FindDataDirectory()
    {
        if (IsPackaged())
            return PackagedRoot();

        wchar_t exePath[MAX_PATH * 4] = {};
        const DWORD length = ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (length == 0 || length >= std::size(exePath))
            return {};

        fs::path current = fs::path(exePath).parent_path();
        while (!current.empty())
        {
            std::error_code error;
            const fs::path candidate = current / L"Data";
            if (fs::is_directory(candidate, error))
                return candidate;
            const fs::path parent = current.parent_path();
            if (parent == current)
                break;
            current = parent;
        }
        return {};
    }

    std::string PackagedRelativePath(const fs::path& path)
    {
        std::string relative;
        return ToPackaged(path, relative) ? relative : std::string();
    }

    bool IsFile(const fs::path& path)
    {
        std::string relative;
        if (ToPackaged(path, relative))
            return !relative.empty() && Api().FileSize(relative.c_str()) >= 0;

        std::error_code error;
        return fs::is_regular_file(path, error);
    }

    bool IsDirectory(const fs::path& path)
    {
        std::string relative;
        if (ToPackaged(path, relative))
            return Api().IsDirectory(relative.c_str());

        std::error_code error;
        return fs::is_directory(path, error);
    }

    bool Exists(const fs::path& path)
    {
        std::string relative;
        if (ToPackaged(path, relative))
            return (!relative.empty() && Api().FileSize(relative.c_str()) >= 0) || Api().IsDirectory(relative.c_str());

        std::error_code error;
        return fs::exists(path, error);
    }

    bool ReadBytes(const fs::path& path, std::vector<std::uint8_t>& out)
    {
        out.clear();
        std::string relative;
        if (ToPackaged(path, relative))
        {
            const long long size = relative.empty() ? -1 : Api().FileSize(relative.c_str());
            if (size < 0)
                return false;
            out.resize(static_cast<size_t>(size));
            if (size > 0 && !Api().ReadFile(relative.c_str(), out.data(), static_cast<unsigned long long>(size)))
            {
                out.clear();
                return false;
            }
            return true;
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        const std::streamsize size = file.tellg();
        if (size < 0)
            return false;
        file.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(size));
        if (size > 0 && !file.read(reinterpret_cast<char*>(out.data()), size))
        {
            out.clear();
            return false;
        }
        return true;
    }

    bool ReadText(const fs::path& path, std::string& out)
    {
        std::vector<std::uint8_t> bytes;
        if (!ReadBytes(path, bytes))
        {
            out.clear();
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    std::unique_ptr<std::istream> OpenStream(const fs::path& path)
    {
        std::string relative;
        if (ToPackaged(path, relative))
        {
            std::vector<std::uint8_t> bytes;
            if (!ReadBytes(path, bytes))
                return nullptr;
            return std::make_unique<OwnedMemoryStream>(std::move(bytes));
        }

        auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
        if (!*file)
            return nullptr;
        return file;
    }

    InputFile::InputFile(const fs::path& path, std::ios_base::openmode mode)
        : std::istream(nullptr)
    {
        std::string relative;
        if (ToPackaged(path, relative))
        {
            std::vector<std::uint8_t> bytes;
            if (ReadBytes(path, bytes))
            {
                mBuffer = std::make_unique<OwnedMemoryBuffer>(std::move(bytes));
                mOpen = true;
            }
        }
        else
        {
            auto file = std::make_unique<std::filebuf>();
            if (file->open(path, mode | std::ios_base::in) != nullptr)
            {
                mBuffer = std::move(file);
                mOpen = true;
            }
        }

        if (mOpen)
            rdbuf(mBuffer.get());
        else
            setstate(std::ios_base::failbit);
    }

    InputFile::~InputFile()
    {
        rdbuf(nullptr);
    }

    std::vector<fs::path> ListFiles(const fs::path& directory, bool recursive)
    {
        std::vector<fs::path> files;
        std::string relative;
        if (ToPackaged(directory, relative))
        {
            struct Context
            {
                std::vector<fs::path>* Files;
                fs::path Root;
            } context{ &files, PackagedRoot() };

            Api().List(relative.c_str(), recursive, [](const char* entry, void* opaque)
            {
                auto* ctx = static_cast<Context*>(opaque);
                ctx->Files->push_back((ctx->Root / FromUtf8(entry)).make_preferred());
            }, &context);
            return files;
        }

        std::error_code error;
        if (!fs::is_directory(directory, error))
            return files;

        if (recursive)
        {
            for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error))
            {
                std::error_code statusError;
                if (it->is_regular_file(statusError) && !statusError)
                    files.push_back(it->path());
            }
        }
        else
        {
            for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error))
            {
                std::error_code statusError;
                if (it->is_regular_file(statusError) && !statusError)
                    files.push_back(it->path());
            }
        }
        return files;
    }
}
