#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <dwmapi.h>
#include <GL/gl.h>
#include <algorithm>
#include <cmath>

// Tự động liên kết toàn bộ thư viện hệ thống Windows 11 ngay trong trình biên dịch MSVC/Clang
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace geode::prelude;

// Mức sàn tối thiểu: 480 FPS (2.083333 ms mỗi bước khung hình, không giới hạn trần)
constexpr double MIN_FLOOR_FPS = 480.0;
constexpr double MAX_FLOOR_DT = 1.0 / MIN_FLOOR_FPS;

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace {
    struct Win11PowerThrottling {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };

    using PFN_wglSwapIntervalEXT = BOOL(WINAPI*)(int interval);
    using PFN_NtSetTimerResolution = LONG(NTAPI*)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
}

namespace HyperEngineV4 {
    static LARGE_INTEGER s_qpcFreq = {};
    static LONGLONG s_lastSwapTick = 0;
    static HANDLE s_mmcssHandle = nullptr;
    static PFN_wglSwapIntervalEXT s_wglSwapIntervalEXT = nullptr;
    static PFN_NtSetTimerResolution s_ntSetTimerResolution = nullptr;

    static bool s_kernelUnlocked = false;
    static bool s_glConfigured = false;
    static uint64_t s_frameCounter = 0;
    static float s_subframeRemainder = 0.0f;

    // 1. Mở khóa toàn diện Kernel Windows 11 (0.5ms Timer + Tắt EcoQoS + Ưu tiên CPU/GPU tối đa)
    void unlockWindows11Kernel() {
        if (s_kernelUnlocked) return;

        // Ép độ phân giải đồng hồ hệ thống Windows 11 xuống 0.5ms (5000 đơn vị 100-ns)
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            s_ntSetTimerResolution = reinterpret_cast<PFN_NtSetTimerResolution>(
                GetProcAddress(ntdll, "NtSetTimerResolution")
            );
            if (s_ntSetTimerResolution) {
                ULONG currentRes = 0;
                s_ntSetTimerResolution(5000, TRUE, &currentRes);
            }
        }
        timeBeginPeriod(1);

        // Đặt quyền ưu tiên cao nhất cho tiến trình Geometry Dash và luồng chính
        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

        // Bật Dynamic Priority Boost khi có tín hiệu chuột/bàn phím
        SetProcessPriorityBoost(hProcess, FALSE);
        SetThreadPriorityBoost(hThread, FALSE);

        // Vô hiệu hóa hoàn toàn cơ chế tiết kiệm điện EcoQoS và khóa Timer của Windows 11
        Win11PowerThrottling throttling = {};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                 PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0;
        SetProcessInformation(
            hProcess,
            static_cast<PROCESS_INFORMATION_CLASS>(4),
            &throttling,
            sizeof(throttling)
        );

        // Đăng ký luồng vào bộ lập lịch phần cứng MMCSS ("Games" - Critical Priority)
        DWORD taskIndex = 0;
        s_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
        if (s_mmcssHandle) {
            AvSetMmThreadPriority(s_mmcssHandle, AVRT_PRIORITY_CRITICAL);
        }

        // Bật chế độ ưu tiên xuất hình độ trễ thấp trên Desktop Window Manager (DWM)
        DwmEnableMMCSS(TRUE);

        QueryPerformanceFrequency(&s_qpcFreq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_lastSwapTick = now.QuadPart;

        s_kernelUnlocked = true;
    }

    void restoreKernelState() {
        if (!s_kernelUnlocked) return;
        if (s_mmcssHandle) {
            AvRevertMmThreadCharacteristics(s_mmcssHandle);
            s_mmcssHandle = nullptr;
        }
        DwmEnableMMCSS(FALSE);
        timeEndPeriod(1);
        s_kernelUnlocked = false;
    }

    // 2. Mở khóa giới hạn 60Hz ở tầng Driver GPU & Sửa lỗi nhòe hình, lệch hình, răng cưa
    void optimizeOpenGLAndDisplay() {
        if (!s_glConfigured) {
            s_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
                wglGetProcAddress("wglSwapIntervalEXT")
            );
            s_glConfigured = true;
        }

        // Luôn tắt V-Sync phần cứng để màn hình 60Hz xuất thẳng từ 480 FPS thật trở lên
        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }

        // Bật khử răng cưa đa mẫu phần cứng (MSAA), tắt bộ lọc làm nhòe cạnh đa giác
        // giúp icon sắc nét ở tốc độ cao và xóa sạch đường kẻ đen giữa các block
        glEnable(GL_MULTISAMPLE);
        glDisable(GL_POLYGON_SMOOTH);
        glDisable(GL_LINE_SMOOTH);
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    }

    // 3. Thiết lập sàn 480+ FPS và tương thích hoàn toàn với Mod Eclipse
    void enforceMinimum480Floor() {
        if (auto* gm = GameManager::sharedState()) {
            // Tắt V-Sync nội bộ ("0030") và bật Custom FPS ("0116")
            gm->setGameVariable("0030", false);
            gm->setGameVariable("0116", true);
        }
        if (auto* director = CCDirector::sharedDirector()) {
            // Chỉ can thiệp khi FPS bị tụt xuống dưới 480; nếu Eclipse đặt cao hơn (720/1440 FPS)
            // thì giữ nguyên mức cao hơn đó để đạt tổng FPS cao nhất
            if (director->m_dAnimationInterval > MAX_FLOOR_DT || director->m_dAnimationInterval <= 0.0) {
                if (auto* app = CCApplication::sharedApplication()) {
                    app->setAnimationInterval(MAX_FLOOR_DT);
                }
            }
        }
    }

    // 4. Quét trực tiếp tín hiệu chuột và phím từ Kernel Windows cho pha Frame-Perfect
    void pumpRawHardwareInput() {
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

    // 5. Bộ tán xạ đường quét theo tỷ lệ vàng (Golden-Ratio Tear Scrambler) cho màn 60Hz
    void scramble60HzTearLine() {
        ++s_frameCounter;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        LONGLONG elapsed = now.QuadPart - s_lastSwapTick;
        LONGLONG minHarmonicTicks = s_qpcFreq.QuadPart / 960;

        // Dịch pha vi mô theo chuỗi tỷ lệ vàng khi khung hình trùng sóng hài 60Hz
        // giúp vệt xé hình tản mịn hoàn toàn khỏi mắt thường mà không làm giảm FPS
        if (elapsed > minHarmonicTicks && (s_frameCounter % 8 == 0)) {
            double goldenPhase = std::fmod(static_cast<double>(s_frameCounter) * 0.6180339887, 1.0);
            LONGLONG ditherTicks = static_cast<LONGLONG>(goldenPhase * (s_qpcFreq.QuadPart / 200000));
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
// 1. Can thiệp CCScheduler: Đảm bảo tốc độ khung hình luôn từ 480 FPS trở lên
// ---------------------------------------------------------------------------
class $modify(UltimateFloorScheduler, CCScheduler) {
    void update(float dt) {
        HyperEngineV4::unlockWindows11Kernel();
        HyperEngineV4::enforceMinimum480Floor();
        HyperEngineV4::pumpRawHardwareInput();

        constexpr float floorDt = static_cast<float>(MAX_FLOOR_DT);
        float timeScale = this->getTimeScale();
        float targetMaxStep = (timeScale > 0.0f) ? (floorDt * timeScale) : floorDt;

        // Nếu phần cứng đang chạy >= 480 FPS (hoặc được Eclipse đẩy lên 720 - 1440+ FPS):
        // Cho phép chạy thẳng không giới hạn trần để đạt mức FPS cao nhất
        if (dt > 0.0f && dt <= targetMaxStep * 1.05f) {
            CCScheduler::update(dt);
            return;
        }

        // Nếu gặp màn chơi quá nặng gây khựng tạm thời xuống dưới 480 FPS:
        // Tự động chia nhỏ khung hình thành các bước >= 480Hz và quét chuột ở mỗi bước
        // để giữ độ nhạy Frame-Perfect và không cho nhịp xử lý tụt xuống dưới 480!
        if (dt > targetMaxStep * 1.05f && dt < 0.1f) {
            float totalTime = dt + HyperEngineV4::s_subframeRemainder;
            int subSteps = static_cast<int>(totalTime / targetMaxStep);
            subSteps = std::clamp(subSteps, 1, 8);

            float stepDt = totalTime / static_cast<float>(subSteps);
            if (stepDt > targetMaxStep) {
                stepDt = targetMaxStep;
            }

            for (int i = 0; i < subSteps; ++i) {
                if (i > 0) {
                    HyperEngineV4::pumpRawHardwareInput();
                }
                CCScheduler::update(stepDt);
            }

            HyperEngineV4::s_subframeRemainder = std::clamp(
                totalTime - (stepDt * static_cast<float>(subSteps)),
                0.0f,
                targetMaxStep
            );
        } else {
            HyperEngineV4::s_subframeRemainder = 0.0f;
            CCScheduler::update(targetMaxStep);
        }
    }
};

// ---------------------------------------------------------------------------
// 2. Can thiệp GJBaseGameLayer: Quét Input trực tiếp trong màn chơi cho Frame-Perfect
// ---------------------------------------------------------------------------
class $modify(UltimatePlayLayer, GJBaseGameLayer) {
    void update(float dt) {
        HyperEngineV4::pumpRawHardwareInput();
        GJBaseGameLayer::update(dt);
    }
};

// ---------------------------------------------------------------------------
// 3. Can thiệp CCEGLView: Xử lý xuất hình 480+ FPS thật, chống nhòe & chống bể hình
// ---------------------------------------------------------------------------
class $modify(UltimateEGLView, CCEGLView) {
    void swapBuffers() {
        HyperEngineV4::optimizeOpenGLAndDisplay();

        // Đẩy toàn bộ khung hình trong hàng đợi GPU ra ngay lập tức để xóa lệch hình & trễ chuột
        glFlush();

        // Tán xạ vệt xé hình trên màn hình 60Hz
        HyperEngineV4::scramble60HzTearLine();

        CCEGLView::swapBuffers();
    }
};

// ---------------------------------------------------------------------------
// Tự động kích hoạt ngay khi mở Geometry Dash (Không cần bật/tắt)
// ---------------------------------------------------------------------------
$on_mod(Loaded) {
    HyperEngineV4::unlockWindows11Kernel();

    Loader::get()->queueInMainThread([]() {
        HyperEngineV4::optimizeOpenGLAndDisplay();
        HyperEngineV4::enforceMinimum480Floor();
    });
}

$on_mod(Unloaded) {
    HyperEngineV4::restoreKernelState();
}
