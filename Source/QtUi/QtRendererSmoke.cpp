// Integration smoke test: run from the repository root after a Debug/x64 build.
#include <windows.h>
#include <QtWidgets/QtWidgets>
#include <cstdio>
#include <cstdlib>

static bool stopping = false;
static void __stdcall progress(const wchar_t *message)
{
    if (stopping) { std::printf("%ls\n", message); std::fflush(stdout); }
}

int main()
{
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Pterosoft", "Ptero Editor");
    const QVariant savedLayout = settings.value("layout/qt-v1");
    wchar_t binaryDirectory[MAX_PATH]{};
    GetFullPathNameW(L"Binaries", MAX_PATH, binaryDirectory, nullptr);
    SetDllDirectoryW(binaryDirectory);
    HMODULE renderer = LoadLibraryW((std::wstring(binaryDirectory) + L"\\Renderer_DX12.dll").c_str());
    if (!renderer)
    {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    auto initialize = reinterpret_cast<bool(__stdcall *)(HWND)>(GetProcAddress(renderer, "RendererDX12_Initialize"));
    auto render = reinterpret_cast<bool(__stdcall *)()>(GetProcAddress(renderer, "RendererDX12_Render"));
    auto shutdown = reinterpret_cast<void(__stdcall *)()>(GetProcAddress(renderer, "RendererDX12_Shutdown"));
    auto error = reinterpret_cast<const char *(__stdcall *)()>(GetProcAddress(renderer, "RendererDX12_GetLastError"));
    if (!initialize || !render || !shutdown || !error)
        return 2;
    auto setProgress = reinterpret_cast<void(__stdcall*)(void(__stdcall*)(const wchar_t*))>(
        GetProcAddress(renderer,"RendererDX12_SetProgressCallback"));
    if (setProgress) setProgress(progress);
    HWND host = CreateWindowExW(0, L"STATIC", L"Ptero Qt renderer smoke", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0,
                                1440, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::puts("Initializing renderer...");
    std::fflush(stdout);
    if (!initialize(host))
    {
        std::printf("Initialization failed: %s\n", error());
        FreeLibrary(renderer);
        DestroyWindow(host);
        return 3;
    }
    bool passed = true;
    QMainWindow *shell = nullptr;
    for (int i = 0; i < 80; ++i)
    {
        if (!render())
        {
            std::printf("Frame %d failed: %s\n", i, error());
            passed = false;
            break;
        }
        if (i == 20)
        {
            for (auto *w : QApplication::allWidgets())
                if (auto *m = qobject_cast<QMainWindow *>(w))
                    shell = m;
            if (!shell)
            {
                passed = false;
                break;
            }
            shell->resize(1100, 720);
        }
        QThread::msleep(16);
    }
    if (shell)
    {
        // Exercise the editor's GPU screenshot path as well as the native chrome.
        QTemporaryDir captureDirectory("Cache/qt-viewport-smoke-XXXXXX");
        for (auto *action : shell->findChildren<QAction *>())
            if (action->text() == "Screenshot...") action->trigger();
        for (int i=0;i<3;++i) { passed=render()&&passed; QThread::msleep(16); }
        for (auto *edit : shell->findChildren<QLineEdit *>())
            if (edit->accessibleName() == "Output Folder")
            {
                edit->setText(captureDirectory.path());
                QMetaObject::invokeMethod(edit,"textEdited",Qt::DirectConnection,Q_ARG(QString,captureDirectory.path()));
            }
        for (auto *button : shell->findChildren<QPushButton *>())
            if (button->text() == "Capture") button->click();
        for (int i=0;i<3;++i) { passed=render()&&passed; QThread::msleep(16); }
        const auto captures=QDir(captureDirectory.path()).entryList({"*.png"},QDir::Files);
        if (captures.isEmpty()) passed=false;
        else {
            QImage image(QDir(captureDirectory.path()).filePath(captures.front()));
            passed=!image.isNull()&&image.width()>16&&image.height()>16&&image.save("Cache/qt-viewport-runtime.png")&&passed;
        }
        // Show auxiliary native controls without changing scene data.
        for (auto *action : shell->findChildren<QAction *>())
            if (action->text() == "Scene Settings...")
                action->trigger();
        for (int i = 0; i < 5; ++i)
        {
            passed = render() && passed;
            QThread::msleep(16);
        }
        passed = shell->grab().save("Cache/qt-editor-runtime.png") && passed;
        for (auto *dialog : shell->findChildren<QDialog *>())
            if (dialog->windowTitle() == "Scene Settings")
                passed = dialog->grab().save("Cache/qt-scene-settings.png") && passed;
    }
    std::puts("Shutting down renderer...");
    std::fflush(stdout);
    stopping = true;
    shutdown();
    if (savedLayout.isValid()) settings.setValue("layout/qt-v1", savedLayout);
    else settings.remove("layout/qt-v1");
    settings.sync();
    std::puts("Unloading renderer...");std::fflush(stdout);
    FreeLibrary(renderer);
    std::puts("Destroying host window...");std::fflush(stdout);
    DestroyWindow(host);
    std::puts(passed ? "Qt renderer smoke: passed" : "Qt renderer smoke: failed");
    return passed ? 0 : 4;
}
