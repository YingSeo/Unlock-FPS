#include <Geode/Geode.hpp>
#include <Geode/modify/CCApplication.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>

using namespace geode::prelude;

constexpr double MIN_FLOOR_FPS = 480.0;
constexpr double TARGET_FRAME_TIME = 1.0 / MIN_FLOOR_FPS;

// 1. Hook CCApplication: Khóa sàn tối thiểu 480 FPS, tương thích hoàn toàn với Eclipse Menu
class $modify(ZeroApp, CCApplication) {
    void setAnimationInterval(double interval) {
        double safeInterval = (interval > TARGET_FRAME_TIME || interval <= 0.0)
            ? TARGET_FRAME_TIME
            : interval;
        CCApplication::setAnimationInterval(safeInterval);
    }
};

// 2. Hook CCScheduler: Đảm bảo nhịp xử lý vật lý không bao giờ tụt dưới 480Hz
class $modify(ZeroScheduler, CCScheduler) {
    void update(float dt) {
        constexpr float floorDt = static_cast<float>(TARGET_FRAME_TIME);
        
        // Nếu game chạy mượt ở mức >= 480 FPS (hoặc được Eclipse đẩy lên 720/1440+ FPS):
        // Cho phép chạy thẳng không giới hạn trần để đạt FPS cao nhất.
        if (dt > 0.0f && dt <= floorDt * 1.05f) {
            CCScheduler::update(dt);
            return;
        }

        // Nếu gặp cảnh nặng làm phần cứng khựng tạm thời dưới 480 FPS:
        // Ghim khoảng thời gian về mức sàn 480 FPS để giữ vững tốc độ khung hình thực tế.
        if (dt > floorDt * 1.05f && dt < 0.1f) {
            CCScheduler::update(floorDt);
        } else {
            CCScheduler::update(dt);
        }
    }
};

// 3. Hook CCEGLView: Mở khóa V-Sync phần cứng để màn hình 60Hz xuất từ 480 FPS thật trở lên
class $modify(ZeroEGLView, CCEGLView) {
    void swapBuffers() {
        if (auto* view = CCEGLView::sharedOpenGLView()) {
            view->toggleVerticalSync(false);
        }
        CCEGLView::swapBuffers();
    }

    void toggleVerticalSync(bool) {
        CCEGLView::toggleVerticalSync(false);
    }
};

// 4. Tự động kích hoạt toàn bộ tính năng ngay khi mở Geometry Dash 2.2081
$on_mod(Loaded) {
    Loader::get()->queueInMainThread([]() {
        if (auto* gm = GameManager::sharedState()) {
            gm->setGameVariable("0030", false); // Tắt V-Sync nội bộ
            gm->setGameVariable("0116", true);  // Bật Custom FPS Bypass
            gm->m_customFPSTarget = static_cast<float>(MIN_FLOOR_FPS);
        }
        if (auto* app = CCApplication::sharedApplication()) {
            app->setAnimationInterval(TARGET_FRAME_TIME);
        }
        if (auto* view = CCEGLView::sharedOpenGLView()) {
            view->toggleVerticalSync(false);
        }
    });
}
