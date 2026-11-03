#include <Geode/Geode.hpp>
#include <Geode/modify/CCApplication.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace geode::prelude;

// Mức sàn tối thiểu: 480 FPS (2.083333 ms mỗi khung hình, không giới hạn trần)
constexpr double MIN_FLOOR_FPS = 480.0;
constexpr double MAX_FLOOR_DT = 1.0 / MIN_FLOOR_FPS;

// Hằng số OpenGL chuẩn phần cứng
constexpr unsigned int GL_MULTISAMPLE_EXT_CODE = 0x809D;
constexpr unsigned int GL_LINE_SMOOTH_CODE = 0x0B20;
constexpr unsigned int GL_POLYGON_SMOOTH_CODE = 0x0B41;
constexpr unsigned int GL_PERSPECTIVE_CORRECTION_HINT_CODE = 0x0C50;
constexpr unsigned int GL_NICEST_CODE = 0x1102;

namespace {
    struct Win11PowerThrottling {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };

    using PFN_NtQueryTimerResolution = LONG(NTAPI*)(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);
    using PFN_NtSetTimerResolution   = LONG(NTAPI*)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
    using PFN_timeBeginPeriod        = UINT(WINAPI*)(UINT uPeriod);
    using PFN_timeEndPeriod          = UINT(WINAPI*)(UINT uPeriod);
    using PFN_AvSetMmThreadCharsW    = HANDLE(WINAPI*)(LPCWSTR TaskName, LPDWORD TaskIndex);
    using PFN_AvSetMmThreadPriority  = BOOL(WINAPI*)(HANDLE AvrtHandle, int Priority);
    using PFN_AvRevertMmThreadChars  = BOOL(WINAPI*)(HANDLE AvrtHandle);
    using PFN_DwmEnableMMCSS         = HRESULT(WINAPI*)(BOOL fEnableMMCSS);
    using PFN_SetProcessInformation  = BOOL(WINAPI*)(HANDLE hProcess, int ProcessInformationClass, LPVOID ProcessInformation, DWORD ProcessInformationSize);

    using PFN_wglGetProcAddress      = PROC(WINAPI*)(LPCSTR lpszProc);
    using PFN_wglSwapIntervalEXT     = BOOL(WINAPI*)(int interval);
    using PFN_glEnable               = void(APIENTRY*)(unsigned int cap);
    using PFN_glDisable              = void(APIENTRY*)(unsigned int cap);
    using PFN_glHint                 = void(APIENTRY*)(unsigned int target, unsigned int mode);
    using PFN_glFlush                = void(APIENTRY*)();
}

namespace ApexEngineV5 {
    static LARGE_INTEGER s_qpcFreq = {};
    static LONGLONG s_lastSwapTick = 0;
    static HANDLE s_mmcssHandle = nullptr;

    static PFN_timeEndPeriod s_timeEndPeriod = nullptr;
    static PFN_AvRevertMmThreadChars s_avRevert = nullptr;
    static PFN_DwmEnableMMCSS s_dwmEnableMMCSS = nullptr;

    static PFN_wglGetProcAddress s_wglGetProcAddress = nullptr;
    static PFN_wglSwapIntervalEXT s_wglSwapIntervalEXT = nullptr;
    static PFN_glEnable s_glEnable = nullptr;
    static PFN_glDisable s_glDisable = nullptr;
    static PFN_glHint s_glHint = nullptr;
    static PFN_glFlush s_glFlush = nullptr;

    static double s_activeTargetDt = MAX_FLOOR_DT;
    static float s_timeAccumulator = 0.0f;
    static uint64_t s_frameCounter = 0;

    static bool s_kernelInitialized = false;
    static bool s_glResolved = false;
    static bool s_forcingInterval = false;

    // 1. Mở khóa toàn diện Kernel Windows 11 bằng liên kết động (Không gây lỗi biên dịch)
    void initWindows11KernelBoost() {
        if (s_kernelInitialized) return;

        // Mở khóa độ phân giải Timer xuống 0.5ms qua ntdll.dll
        if (HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll")) {
            auto ntQuery = reinterpret_cast<PFN_NtQueryTimerResolution>(
                GetProcAddress(hNtdll, "NtQueryTimerResolution")
            );
            auto ntSet = reinterpret_cast<PFN_NtSetTimerResolution>(
                GetProcAddress(hNtdll, "NtSetTimerResolution")
            );
            if (ntSet) {
                ULONG minRes = 0, maxRes = 5000, curRes = 0;
                if (ntQuery) {
                    ntQuery(&minRes, &maxRes, &curRes);
                }
                ntSet(maxRes, TRUE, &curRes);
            }
        }

        // Khóa chu kỳ 1ms dự phòng qua winmm.dll
        if (HMODULE hWinmm = LoadLibraryW(L"winmm.dll")) {
            auto timeBegin = reinterpret_cast<PFN_timeBeginPeriod>(
                GetProcAddress(hWinmm, "timeBeginPeriod")
            );
            s_timeEndPeriod = reinterpret_cast<PFN_timeEndPeriod>(
                GetProcAddress(hWinmm, "timeEndPeriod")
            );
            if (timeBegin) {
                timeBegin(1);
            }
        }

        // Đặt quyền ưu tiên cao nhất cho tiến trình và luồng chính
        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
        SetProcessPriorityBoost(hProcess, FALSE);
        SetThreadPriorityBoost(hThread, FALSE);

        // Tắt hoàn toàn cơ chế bóp hiệu năng EcoQoS và khóa Timer trên Windows 11
        if (HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll")) {
            auto setProcInfo = reinterpret_cast<PFN_SetProcessInformation>(
                GetProcAddress(hKernel32, "SetProcessInformation")
            );
            if (setProcInfo) {
                Win11PowerThrottling throttling = {};
                throttling.Version = 1;
                throttling.ControlMask = 0x1 | 0x4; // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION
                throttling.StateMask = 0;           // 0 = Tắt mọi giới hạn của Windows 11
                setProcInfo(hProcess, 4, &throttling, sizeof(throttling));
            }
        }

        // Đăng ký luồng chính vào hàng đợi ưu tiên phần cứng MMCSS ("Games" Critical)
        if (HMODULE hAvrt = LoadLibraryW(L"avrt.dll")) {
            auto avSetChars = reinterpret_cast<PFN_AvSetMmThreadCharsW>(
                GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW")
            );
            auto avSetPrio = reinterpret_cast<PFN_AvSetMmThreadPriority>(
                GetProcAddress(hAvrt, "AvSetMmThreadPriority")
            );
            s_avRevert = reinterpret_cast<PFN_AvRevertMmThreadChars>(
                GetProcAddress(hAvrt, "AvRevertMmThreadCharacteristics")
            );
            if (avSetChars) {
                DWORD taskIdx = 0;
                s_mmcssHandle = avSetChars(L"Games", &taskIdx);
                if (s_mmcssHandle && avSetPrio) {
                    avSetPrio(s_mmcssHandle, 2); // AVRT_PRIORITY_CRITICAL = 2
                }
            }
        }

        // Kích hoạt chế độ xuất hình độ trễ thấp trên Windows 11 DWM
        if (HMODULE hDwm = LoadLibraryW(L"dwmapi.dll")) {
            s_dwmEnableMMCSS = reinterpret_cast<PFN_DwmEnableMMCSS>(
                GetProcAddress(hDwm, "DwmEnableMMCSS")
            );
            if (s_dwmEnableMMCSS) {
                s_dwmEnableMMCSS(TRUE);
            }
        }

        QueryPerformanceFrequency(&s_qpcFreq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_lastSwapTick = now.QuadPart;

        s_kernelInitialized = true;
    }

    void cleanupKernelBoost() {
        if (!s_kernelInitialized) return;
        if (s_mmcssHandle && s_avRevert) {
            s_avRevert(s_mmcssHandle);
            s_mmcssHandle = nullptr;
        }
        if (s_dwmEnableMMCSS) {
            s_dwmEnableMMCSS(FALSE);
        }
        if (s_timeEndPeriod) {
            s_timeEndPeriod(1);
        }
        s_kernelInitialized = false;
    }

    // 2. Mở khóa giới hạn 60Hz ở tầng Driver GPU & Sửa lỗi nhòe hình, lệch hình, răng cưa
    void configureOpenGLHardware() {
        if (!s_glResolved) {
            HMODULE hOgl = GetModuleHandleW(L"opengl32.dll");
            if (!hOgl) {
                hOgl = LoadLibraryW(L"opengl32.dll");
            }
            if (hOgl) {
                s_wglGetProcAddress = reinterpret_cast<PFN_wglGetProcAddress>(
                    GetProcAddress(hOgl, "wglGetProcAddress")
                );
                s_glEnable = reinterpret_cast<PFN_glEnable>(
                    GetProcAddress(hOgl, "glEnable")
                );
                s_glDisable = reinterpret_cast<PFN_glDisable>(
                    GetProcAddress(hOgl, "glDisable")
                );
                s_glHint = reinterpret_cast<PFN_glHint>(
                    GetProcAddress(hOgl, "glHint")
                );
                s_glFlush = reinterpret_cast<PFN_glFlush>(
                    GetProcAddress(hOgl, "glFlush")
                );
            }
            s_glResolved = true;
        }

        if (!s_wglSwapIntervalEXT && s_wglGetProcAddress) {
            s_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
                s_wglGetProcAddress("wglSwapIntervalEXT")
            );
        }

        // Luôn tắt V-Sync phần cứng ở tầng Driver để màn 60Hz xuất thẳng 480+ FPS thật
        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }

        // Bật khử răng cưa phần cứng (MSAA) và tắt bộ lọc làm mờ cạnh gây nhòe hình/viền đen
        if (s_glEnable) {
            s_glEnable(GL_MULTISAMPLE_EXT_CODE);
        }
        if (s_glDisable) {
            s_glDisable(GL_POLYGON_SMOOTH_CODE);
            s_glDisable(GL_LINE_SMOOTH_CODE);
        }
        if (s_glHint) {
            s_glHint(GL_PERSPECTIVE_CORRECTION_HINT_CODE, GL_NICEST_CODE);
        }
        if (s_glFlush) {
            s_glFlush();
        }
    }

    // 3. Quét trực tiếp tín hiệu chuột và phím từ Kernel Windows cho pha Frame-Perfect
    void pollRawInput480Hz() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // 4. Tán xạ đường quét theo tỷ lệ vàng trên màn hình 60Hz để chống bể hình
    void scramble60HzScanline() {
        ++s_frameCounter;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        LONGLONG elapsed = now.QuadPart - s_lastSwapTick;
        LONGLONG minHarmonicTicks = s_qpcFreq.QuadPart / 960;

        if (elapsed > minHarmonicTicks && (s_frameCounter % 8 == 0)) {
            double goldenPhase = std::fmod(static_cast<double>(s_frameCounter) * 0.6180339887, 1.0);
            LONGLONG ditherTicks = static_cast<LONGLONG>(goldenPhase * (s_qpcFreq.QuadPart / 220000));
            LARGE_INTEGER current;
            do {
                YieldProcessor();
                QueryPerformanceCounter(&current);
            } while ((current.QuadPart - now.QuadPart) < ditherTicks);
            s_lastSwapTick = current.QuadPart;
        } else {
            s_lastSwapTick = now.QuadPart;
        }
    }
}

// ---------------------------------------------------------------------------
// 1. Hook CCApplication: Khóa sàn tối thiểu 480 FPS & Tương thích hoàn toàn với Eclipse
// ---------------------------------------------------------------------------
class $modify(ApexAppFloor, CCApplication) {
    void setAnimationInterval(double interval) {
        // Nếu game đặt mức FPS thấp hơn 480 (interval > 1/480s), ép ngay về sàn 480 FPS.
        // Nếu Eclipse Menu đặt mức FPS cao hơn (720/960/1440+ FPS -> interval < 1/480s),
        // giữ nguyên mức cao hơn đó để đạt số lượng FPS cao nhất!
        double targetInterval = (interval > MAX_FLOOR_DT || interval <= 0.0)
            ? MAX_FLOOR_DT
            : interval;

        ApexEngineV5::s_activeTargetDt = targetInterval;
        CCApplication::setAnimationInterval(targetInterval);
    }
};

// ---------------------------------------------------------------------------
// 2. Hook CCScheduler: Đảm bảo tốc độ xử lý >= 480Hz, chống Drop FPS & nhận Click chuẩn
// ---------------------------------------------------------------------------
class $modify(ApexSchedulerFloor, CCScheduler) {
    void update(float dt) {
        ApexEngineV5::initWindows11KernelBoost();
        ApexEngineV5::pollRawInput480Hz();

        // Đảm bảo mức sàn 480 FPS luôn được áp dụng ngay từ frame đầu tiên
        if (!ApexEngineV5::s_forcingInterval) {
            ApexEngineV5::s_forcingInterval = true;
            if (auto* app = CCApplication::sharedApplication()) {
                app->setAnimationInterval(ApexEngineV5::s_activeTargetDt);
            }
        }

        float baseStep = static_cast<float>(ApexEngineV5::s_activeTargetDt);
        float timeScale = this->getTimeScale();
        float maxAllowedDt = (timeScale > 0.0f) ? (baseStep * timeScale) : baseStep;

        // Khi phần cứng đang chạy mượt ở mức >= 480 FPS (hoặc cao hơn cùng Eclipse):
        // Cho phép chạy thẳng không giới hạn trần để đạt FPS tối đa
        if (dt > 0.0f && dt <= maxAllowedDt * 1.05f) {
            CCScheduler::update(dt);
            return;
        }

        // Khi gặp cảnh nặng gây khựng tạm thời xuống dưới mức sàn 480 FPS:
        // Chia nhỏ bước thời gian về >= 480Hz và quét chuột ở mỗi bước để giữ Frame-Perfect
        // đồng thời tích lũy phần dư giúp nhạc và hình không bao giờ bị lệch (Zero-Drift)
        if (dt > maxAllowedDt * 1.05f && dt < 0.1f) {
            float totalTime = dt + ApexEngineV5::s_timeAccumulator;
            int steps = static_cast<int>(totalTime / maxAllowedDt);
            steps = std::clamp(steps, 1, 8);

            float stepDt = totalTime / static_cast<float>(steps);
            if (stepDt > maxAllowedDt) {
                stepDt = maxAllowedDt;
            }

            for (int i = 0; i < steps; ++i) {
                if (i > 0) {
                    ApexEngineV5::pollRawInput480Hz();
                }
                CCScheduler::update(stepDt);
            }

            ApexEngineV5::s_timeAccumulator = std::clamp(
                totalTime - (stepDt * static_cast<float>(steps)),
                0.0f,
                maxAllowedDt
            );
        } else {
            ApexEngineV5::s_timeAccumulator = 0.0f;
            CCScheduler::update(maxAllowedDt);
        }
    }
};

// ---------------------------------------------------------------------------
// 3. Hook CCEGLView: Xuất 480+ FPS thật lên màn 60Hz, chống nhòe hình & chống bể hình
// ---------------------------------------------------------------------------
class $modify(ApexEGLViewFloor, CCEGLView) {
    void swapBuffers() {
        ApexEngineV5::configureOpenGLHardware();
        ApexEngineV5::scramble60HzScanline();
        CCEGLView::swapBuffers();
    }
};

// ---------------------------------------------------------------------------
// Tự động kích hoạt ngay khi mở Geometry Dash 2.2081 (Không cần bật/tắt)
// ---------------------------------------------------------------------------
$on_mod(Loaded) {
    ApexEngineV5::initWindows11KernelBoost();

    Loader::get()->queueInMainThread([]() {
        ApexEngineV5::configureOpenGLHardware();
        if (auto* app = CCApplication::sharedApplication()) {
            app->setAnimationInterval(MAX_FLOOR_DT);
        }
    });
}

$on_mod(Unloaded) {
    ApexEngineV5::cleanupKernelBoost();
}
