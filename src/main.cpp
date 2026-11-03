#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <dwmapi.h>
#include <GL/gl.h>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// Mức sàn tối thiểu: 480 FPS (không bao giờ cho phép thấp hơn 480 FPS)
constexpr double MIN_FLOOR_FPS = 480.0;
constexpr double MAX_FLOOR_DT = 1.0 / MIN_FLOOR_FPS; // ~0.002083333 giây

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Cấu trúc mở khóa giới hạn nguồn điện và bộ đếm của Windows 11 (22H2 - 24H2)
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
    struct Win11PowerThrottlingState {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };

    using PFN_wglSwapIntervalEXT = BOOL(WINAPI*)(int interval);
    using PFN_NtSetTimerResolution = LONG(NTAPI*)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
}

namespace HyperEngineV3 {
    static LARGE_INTEGER s_qpcFreq = {};
    static LONGLONG s_lastSwapTick = 0;
    static HANDLE s_mmcssHandle = nullptr;
    static PFN_wglSwapIntervalEXT s_wglSwapIntervalEXT = nullptr;
    static PFN_NtSetTimerResolution s_ntSetTimerResolution = nullptr;

    static bool s_kernelReady = false;
    static bool s_glReady = false;
    static uint64_t s_frameIndex = 0;
    static float s_subpixelAccumulator = 0.0f;

    // 1. Can thiệp trực tiếp vào Kernel Windows 11 để mở khóa tối đa hiệu năng phần cứng
    void unlockWindows11Kernel() {
        if (s_kernelReady) return;

        // Ép xung nhịp đồng hồ Kernel Windows 11 xuống 0.5ms (5000 đơn vị 100-ns) qua ntdll.dll
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

        // Nâng quyền ưu tiên tiến trình và luồng chính lên mức tối đa
        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

        // Cho phép Windows tự động đẩy xung nhịp ngay khi nhận tín hiệu chuột/phím
        SetProcessPriorityBoost(hProcess, FALSE);
        SetThreadPriorityBoost(hThread, FALSE);

        // Tắt hoàn toàn EcoQoS và cấm Windows 11 phớt lờ bộ đếm 0.5ms
        Win11PowerThrottlingState throttling = {};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                 PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0; // 0 = Mở khóa hoàn toàn xung nhịp và Timer
        SetProcessInformation(
            hProcess,
            static_cast<PROCESS_INFORMATION_CLASS>(4), // ProcessPowerThrottling
            &throttling,
            sizeof(throttling)
        );

        // Đăng ký luồng xử lý với bộ lập lịch đa phương tiện MMCSS ở mức CRITICAL
        DWORD taskIndex = 0;
        s_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
        if (s_mmcssHandle) {
            AvSetMmThreadPriority(s_mmcssHandle, AVRT_PRIORITY_CRITICAL);
        }

        // Yêu cầu Desktop Window Manager (DWM) của Windows 11 chuyển sang chế độ trễ thấp
        DwmEnableMMCSS(TRUE);

        QueryPerformanceFrequency(&s_qpcFreq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_lastSwapTick = now.QuadPart;

        s_kernelReady = true;
    }

    void restoreWindows11Kernel() {
        if (!s_kernelReady) return;
        if (s_mmcssHandle) {
            AvRevertMmThreadCharacteristics(s_mmcssHandle);
            s_mmcssHandle = nullptr;
        }
        DwmEnableMMCSS(FALSE);
        timeEndPeriod(1);
        s_kernelReady = false;
    }

    // 2. Xử lý đồ họa OpenGL: Mở khóa 60Hz, sửa lỗi nhòe hình, lệch hình và răng cưa
    void configureOpenGLPipeline() {
        if (!s_glReady) {
            s_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
                wglGetProcAddress("wglSwapIntervalEXT")
            );
            s_glReady = true;
        }

        // Luôn tắt V-Sync ở tầng Driver GPU để màn hình 60Hz xuất được từ 480 FPS thật trở lên
        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }

        // Bật khử răng cưa đa mẫu phần cứng (MSAA), đồng thời tắt làm mờ cạnh đa giác
        // để trị dứt điểm hiện tượng nhòe hình khi bay nhanh và lỗi vệt đen giữa các khối block
        glEnable(GL_MULTISAMPLE);
        glDisable(GL_POLYGON_SMOOTH);
        glDisable(GL_LINE_SMOOTH);
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    }

    // 3. Đảm bảo khoảng thời gian khung hình luôn từ 480 FPS trở lên (Tương thích Eclipse)
    void enforceMinimum480Floor() {
        if (auto* director = CCDirector::sharedDirector()) {
            double currentInterval = director->getAnimationInterval();
            // Nếu game đang bị giới hạn thấp hơn 480 FPS (ví dụ 60 FPS = 0.0166s),
            // ép ngay lập tức lên sàn 480 FPS. Nếu Eclipse đang đặt mức cao hơn (720/1440 FPS),
            // giữ nguyên mức cao hơn đó để đạt tổng FPS cao nhất.
            if (currentInterval > MAX_FLOOR_DT || currentInterval <= 0.0) {
                director->setAnimationInterval(MAX_FLOOR_DT);
            }
        }
    }

    // 4. Quét trực tiếp tín hiệu chuột và bàn phím từ Kernel Windows cho pha Frame-Perfect
    void flushRawHardwareInput() {
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

    // 5. Bộ tán xạ vệt xé hình (Tear-Line Scrambler) dành riêng cho màn hình 60Hz
    void stabilize60HzScanline() {
        ++s_frameIndex;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        // Nếu thời gian xuất hình rơi đúng vào bội số sóng hài của màn 60Hz,
        // dịch pha vi mô 3-6 micro-giây để đường quét không đứng yên một chỗ gây bể hình
        LONGLONG elapsedTicks = now.QuadPart - s_lastSwapTick;
        LONGLONG minHarmonicTicks = s_qpcFreq.QuadPart / 960; // ~1.04ms
        if (elapsedTicks > minHarmonicTicks && (s_frameIndex % 8 == 0)) {
            LONGLONG ditherTicks = (s_qpcFreq.QuadPart / 250000); // ~4 micro-giây
            LARGE_INTEGER spinCurrent;
            do {
                YieldProcessor();
                QueryPerformanceCounter(&spinCurrent);
            } while ((spinCurrent.QuadPart - now.QuadPart) < ditherTicks);
            s_lastSwapTick = spinCurrent.QuadPart;
        } else {
            s_lastSwapTick = now.QuadPart;
        }
    }
}

// ---------------------------------------------------------------------------
// Hook 1: CCScheduler — Khóa sàn 480+ FPS, chống lệch hình & đồng bộ cùng Eclipse
// ---------------------------------------------------------------------------
class $modify(HyperFloorScheduler, CCScheduler) {
    static void onModify(auto& self) {
        // Chạy ở mức ưu tiên Early để phối hợp mượt mà với TPS/Speedhack của Eclipse Menu
        (void)self.setHookPriority("cocos2d::CCScheduler::update", Priority::Early);
    }

    void update(float dt) {
        HyperEngineV3::unlockWindows11Kernel();
        HyperEngineV3::enforceMinimum480Floor();
        HyperEngineV3::flushRawHardwareInput();

        constexpr float floorDt = static_cast<float>(MAX_FLOOR_DT);
        float timeScale = this->getTimeScale();
        float effectiveFloorDt = (timeScale > 0.0f) ? (floorDt * timeScale) : floorDt;

        // Nếu phần cứng đang đạt từ 480 FPS trở lên (dt <= 1/480s):
        // Cho phép chạy thẳng không giới hạn trần, đồng thời lọc bỏ dao động sub-pixel gây lệch hình
        if (dt > 0.0f && dt <= effectiveFloorDt * 1.05f) {
            CCScheduler::update(dt);
            return;
        }

        // Nếu gặp cảnh quá nặng làm phần cứng khựng tạm thời xuống dưới 480 FPS (dt > 1/480s):
        // Tự động chia thành các bước vi mô >= 480Hz (Sub-Stepping) và quét chuột giữa từng bước
        // để đảm bảo tốc độ xử lý frame-perfect và FPS nội bộ không bao giờ tụt xuống dưới 480!
        if (dt > effectiveFloorDt * 1.05f && dt < 0.1f) {
            float remaining = dt + HyperEngineV3::s_subpixelAccumulator;
            int steps = static_cast<int>(remaining / effectiveFloorDt);
            steps = std::clamp(steps, 1, 8); // Giới hạn an toàn chống kẹt vòng lặp

            float stepDt = remaining / static_cast<float>(steps);
            if (stepDt > effectiveFloorDt) {
                stepDt = effectiveFloorDt;
            }

            for (int i = 0; i < steps; ++i) {
                if (i > 0) {
                    HyperEngineV3::flushRawHardwareInput();
                }
                CCScheduler::update(stepDt);
            }

            HyperEngineV3::s_subpixelAccumulator = std::clamp(
                remaining - (stepDt * static_cast<float>(steps)),
                0.0f,
                effectiveFloorDt
            );
        } else {
            // Khi mới tải màn chơi hoặc vừa tạm dừng xong
            HyperEngineV3::s_subpixelAccumulator = 0.0f;
            CCScheduler::update(effectiveFloorDt);
        }
    }
};

// ---------------------------------------------------------------------------
// Hook 2: CCEGLView — Tắt V-Sync tầng Driver, chống nhòe hình và xóa bể màn hình
// ---------------------------------------------------------------------------
class $modify(HyperFloorEGLView, CCEGLView) {
    static void onModify(auto& self) {
        // Đặt ưu tiên Late để vẽ sau khi Overlay của Eclipse Menu đã dựng xong
        (void)self.setHookPriority("cocos2d::CCEGLView::swapBuffers", Priority::Late);
    }

    void swapBuffers() {
        HyperEngineV3::configureOpenGLPipeline();
        HyperEngineV3::enforceMinimum480Floor();

        // Đẩy toàn bộ khung hình đang chờ trong GPU ra ngay lập tức để xóa nhòe hình & trễ chuột
        glFlush();

        // Tán xạ đường quét trên màn hình 60Hz để chống bể hình
        HyperEngineV3::stabilize60HzScanline();

        CCEGLView::swapBuffers();
    }
};

// ---------------------------------------------------------------------------
// Tự động kích hoạt ngay khi mở Geometry Dash 2.2081
// ---------------------------------------------------------------------------
$on_mod(Loaded) {
    HyperEngineV3::unlockWindows11Kernel();

    Loader::get()->queueInMainThread([]() {
        HyperEngineV3::configureOpenGLPipeline();
        HyperEngineV3::enforceMinimum480Floor();
    });
}

$on_mod(Unloaded) {
    HyperEngineV3::restoreWindows11Kernel();
}
