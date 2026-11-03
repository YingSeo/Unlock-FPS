#include <Geode/Geode.hpp>
#include <Geode/modify/CCApplication.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <cmath>
#include <algorithm>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <mmsystem.h>
#include <GL/gl.h>

extern "C" NTSTATUS NTAPI NtSetTimerResolution(
    ULONG DesiredResolution,
    BOOLEAN SetResolution,
    PULONG CurrentResolution
);
#endif

using namespace geode::prelude;

namespace {
    constexpr double TARGET_FPS = 480.0;
    constexpr double FIXED_DT = 1.0 / TARGET_FPS; // ~0.002083333s
    constexpr unsigned int SAFE_PARTICLE_LIMIT = 100;

    bool g_isInitialized = false;
    bool g_inLevelPlay = false;

#ifdef GEODE_IS_WINDOWS
    struct MEMORY_PRIORITY_INFO_CUSTOM {
        ULONG MemoryPriority;
    };
    constexpr ULONG MEMORY_PRIORITY_NORMAL_HIGH = 5;
#endif

    void setupSystemEnvironment() {
        if (g_isInitialized) return;
        g_isInitialized = true;

#ifdef GEODE_IS_WINDOWS
        // 1. Khoa bo dinh thoi gian nhan Windows 0.5ms chong lech khung hinh
        timeBeginPeriod(1);
        ULONG currentRes = 0;
        NtSetTimerResolution(5000, TRUE, &currentRes);

        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();

        // 2. Uu tien CPU va luong xu ly toi da cho Geometry Dash
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

        // 3. Vo hieu hoa tiet kiem nang luong CPU (EcoQoS) va tang uu tien RAM
        PROCESS_POWER_THROTTLING_STATE powerState{};
        powerState.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        powerState.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        powerState.StateMask = 0;
        SetProcessInformation(hProcess, ProcessPowerThrottling, &powerState, sizeof(powerState));

        MEMORY_PRIORITY_INFO_CUSTOM memInfo{ MEMORY_PRIORITY_NORMAL_HIGH };
        SetProcessInformation(hProcess, ProcessMemoryPriority, &memInfo, sizeof(memInfo));

        SetProcessPriorityBoost(hProcess, FALSE);
        SetThreadPriorityBoost(hThread, FALSE);
#endif
    }

    // Ap dung thiet lap FPS 480+ truc tiep vao GameManager va Director ma khong gay lap vo tan
    void applyStrictFPSLock() {
        auto* gm = GameManager::sharedState();
        auto* director = CCDirector::sharedDirector();
        if (!gm || !director) return;

        // Tat VSync (0030)
        if (gm->getGameVariable("0030")) {
            gm->setGameVariable("0030", false);
        }
        // Tat Smooth Fix (0023) & Force Smooth Fix (0125) de tranh tua cham vat ly
        if (gm->getGameVariable("0023")) gm->setGameVariable("0023", false);
        if (gm->getGameVariable("0125")) gm->setGameVariable("0125", false);
        // Tat glFinish (0166) tranh nghen GPU
        if (gm->getGameVariable("0166")) gm->setGameVariable("0166", false);

        // Bat Custom FPS (0116)
        if (!gm->getGameVariable("0116")) gm->setGameVariable("0116", true);

        // Khoa cung muc tieu FPS
        if (gm->m_customFPSTarget < static_cast<float>(TARGET_FPS)) {
            gm->m_customFPSTarget = static_cast<float>(TARGET_FPS);
        }

        // Ep truc tiep animation interval xuong chuan 480 FPS
        if (std::abs(director->getAnimationInterval() - FIXED_DT) > 0.00001) {
            director->setAnimationInterval(FIXED_DT);
        }
    }
}

$on_mod(Loaded) {
    setupSystemEnvironment();
}

// 1. Chan chan moi ham co y dinh thay doi interval ve duoi 480 FPS
class $modify(UltraAppPatch, CCApplication) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCApplication::setAnimationInterval", Priority::Last);
        (void)self.setHookPriority("cocos2d::CCApplication::toggleVerticalSync", Priority::Last);
    }

    void setAnimationInterval(double interval) {
        if (interval > FIXED_DT || interval <= 0.0) {
            interval = FIXED_DT;
        }
        CCApplication::setAnimationInterval(interval);
    }

    void toggleVerticalSync(bool enabled) {
        CCApplication::toggleVerticalSync(false);
    }
};

// 2. Scheduler toi uu: Khong gay lag, khong lap lai feedback, chong spike delta-time tuyet doi
class $modify(UltraSchedulerPatch, CCScheduler) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCScheduler::update", Priority::Normal);
    }

    void update(float dt) {
        // Chi thuc hien ep FPS o man hinh choi de tranh anh huong toi UI menu chinh
        if (g_inLevelPlay) {
            // Chuan hoa Delta Time: Khi choi o 480 FPS, dt luon phai khop tuyet doi de chuan Frame Perfect
            if (!std::isfinite(dt) || dt <= 0.0f || dt > 0.033f) {
                dt = static_cast<float>(FIXED_DT);
            } else {
                // khoa cung dt o tan so 480 FPS giup tau/song khong bi dao dong duoc vat ly
                dt = static_cast<float>(FIXED_DT);
            }
        } else {
            if (!std::isfinite(dt) || dt > 0.05f) {
                dt = static_cast<float>(FIXED_DT);
            }
        }

        CCScheduler::update(dt);
    }
};

// 3. Dong bo vat ly trong PlayLayer
class $modify(UltraBaseGamePatch, GJBaseGameLayer) {
    void update(float dt) {
        applyStrictFPSLock();
        GJBaseGameLayer::update(dt);
    }
};

// 4. Quan ly vong doi PlayLayer
class $modify(UltraPlayLayerPatch, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        setupSystemEnvironment();
        applyStrictFPSLock();

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_inLevelPlay = true;

#ifdef GEODE_IS_WINDOWS
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
        glDisable(GL_DITHER);
#endif
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_inLevelPlay = true;
        applyStrictFPSLock();
    }

    void onQuit() {
        g_inLevelPlay = false;
        PlayLayer::onQuit();
        applyStrictFPSLock();
    }
};

// 5. Dong bo khi ve Menu
class $modify(UltraMenuPatch, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        g_inLevelPlay = false;
        setupSystemEnvironment();
        applyStrictFPSLock();
        return true;
    }
};

// 6. Toi ưu hoa hat Particle an toan tuyet doi, chong tran bo nhieu / crash map nang
class $modify(UltraParticlePatch, CCParticleSystemQuad) {
    bool initWithTotalParticles(unsigned int numberOfParticles) {
        if (numberOfParticles > SAFE_PARTICLE_LIMIT) {
            numberOfParticles = SAFE_PARTICLE_LIMIT;
        }
        return CCParticleSystemQuad::initWithTotalParticles(numberOfParticles);
    }

    void update(float dt) {
        if (!this->isVisible() || this->getOpacity() == 0) {
            return;
        }
        if (this->m_uParticleCount > SAFE_PARTICLE_LIMIT) {
            this->m_uParticleCount = SAFE_PARTICLE_LIMIT;
        }
        CCParticleSystemQuad::update(dt);
    }
};
