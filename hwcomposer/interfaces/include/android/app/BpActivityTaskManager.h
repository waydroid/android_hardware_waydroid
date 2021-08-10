#ifndef AIDL_GENERATED_ANDROID_APP_BP_ACTIVITY_TASK_MANAGER_H_
#define AIDL_GENERATED_ANDROID_APP_BP_ACTIVITY_TASK_MANAGER_H_

#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <utils/Errors.h>
#include <android/app/IActivityTaskManager.h>

namespace android {

namespace app {

class BpActivityTaskManager : public ::android::BpInterface<IActivityTaskManager> {
public:
  explicit BpActivityTaskManager(const ::android::sp<::android::IBinder>& _aidl_impl);
  virtual ~BpActivityTaskManager() = default;
  ::android::binder::Status removeTask(int32_t taskId, bool* _aidl_return) override;
  ::android::binder::Status removeAllVisibleRecentTasks() override;
};  // class BpActivityTaskManager

}  // namespace app

}  // namespace android

#endif  // AIDL_GENERATED_ANDROID_APP_BP_ACTIVITY_TASK_MANAGER_H_
